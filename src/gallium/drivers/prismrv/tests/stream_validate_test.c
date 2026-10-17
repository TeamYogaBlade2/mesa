/*
 * Host test of the Linux driver's command-stream security code
 * (prismrv_stream.c, compiled unmodified by run_stream_validate.sh).
 *
 * Inputs: streams dumped from real Mesa output by the drm-shim, with the
 * contents of every listed BO.  Checks that
 *  - the kernel accepts what Mesa produces,
 *  - the snapshot has the TA data copied and DRAW addresses rewritten
 *    into it, and the copy validates as a TA stream,
 *  - a battery of hostile mutations of the same input is rejected.
 * Usage: stream_validate_test <dump-file>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "prismrv_stream.c"

static int fails;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); fails++; } } while (0)

static int read_bo(void *ctx, struct drm_gem_object *bo, u64 off, void *dst, size_t len)
{
   if (off + len > bo->size) return -EINVAL;
   memcpy(dst, bo->data + off, len);
   return 0;
}

/* find the Nth packet with opcode op; returns payload pointer (mutable) */
static u32 *find_pkt(u32 *st, size_t words, u32 op, unsigned nth, u32 *n_out)
{
   size_t pos = 0; u32 o, n; const u32 *p;
   while (prismrv_stream_next(st, words, &pos, &o, &n, &p))
      if (o == op && nth-- == 0) { if (n_out) *n_out = n; return st + (p - st); }
   return NULL;
}

static int reject_prog(const char *text, const char *what)
{
   u32 buf[2 + 64] = { 2, 0 };    /* SET_PROG_VS */
   size_t len = strlen(text), words = (len + 4) / 4;
   struct prismrv_device pv;
   memset(buf + 2, 0, sizeof(buf) - 8);
   memcpy(buf + 2, text, len);
   buf[1] = words;
   if (prismrv_validate_stream(&pv, buf, 2 + words, NULL, 0) == 0) {
      fprintf(stderr, "FAIL: shader accepted: %s\n", what); return 1;
   }
   return 0;
}

int main(int argc, char **argv)
{
   struct prismrv_device pv;
   FILE *f;
   unsigned n = 0;
   uint32_t hdr[3];

   if (argc < 2) return 2;
   f = fopen(argv[1], "rb");
   if (!f) { fprintf(stderr, "no dump\n"); return 1; }

   while (fread(hdr, 4, 3, f) == 3) {
      struct drm_gem_object objs[16], *list[16];
      u32 *st;
      size_t words;

      CHECK(hdr[0] == 0x52545350u && hdr[2] <= 16, "dump header");
      for (unsigned i = 0; i < hdr[2]; i++) {
         uint32_t rec[2];
         if (fread(rec, 4, 2, f) != 2) return 1;
         objs[i].va = rec[0]; objs[i].size = rec[1]; list[i] = &objs[i];
         objs[i].data = malloc(rec[1] ? rec[1] : 1);
         if (rec[1] && fread(objs[i].data, 1, rec[1], f) != rec[1]) return 1;
      }
      words = hdr[1] / 4;
      st = malloc(hdr[1]);
      if (fread(st, 1, hdr[1], f) != hdr[1]) return 1;

      CHECK(prismrv_validate_stream(&pv, st, words, list, hdr[2]) == 0,
            "Mesa stream rejected by kernel validator");

      /* ---- snapshot: copy + rewrite ---- */
      {
         size_t sz = prismrv_stream_snapshot_size(st, words);
         u8 *snap = malloc(sz);
         const u32 snap_va = 0x40000000u;
         u32 dn; u32 *d;

         CHECK(prismrv_stream_snapshot_fill(st, words, list, hdr[2], snap_va,
                                            snap, read_bo, NULL) == 0,
               "snapshot_fill failed on a Mesa stream");
         for (unsigned k = 0; (d = find_pkt((u32 *)snap, words, 5, k, &dn)); k++) {
            u64 va = ((u64)d[1] << 32) | d[0];
            CHECK(va >= snap_va && va + d[2] <= (u64)snap_va + sz,
                  "DRAW not rewritten into the snapshot");
            CHECK(prismrv_validate_ta((u32 *)(snap + (va - snap_va)), d[2] / 4) == 0,
                  "snapshot TA copy does not validate");
         }
         free(snap);
      }

      /* ---- hostile mutations ---- */
      {
         u32 *m = malloc(hdr[1]);
         u32 pn; u32 *d;

         /* does the stream carry any GPU address at all? (a program-only
          * stream, e.g. from the compiler test, carries none) */
         int has_addr = 0;
         { size_t pp = 0; u32 o, nn; const u32 *q;
           while (prismrv_stream_next(st, words, &pp, &o, &nn, &q))
              if (o == 5 || o == 7 || (o == 1 && (q[2] || q[3]))) has_addr = 1; }

         /* 1. no BOs listed: every address is foreign */
         if (has_addr)
            CHECK(prismrv_validate_stream(&pv, st, words, list, 0) != 0,
                  "accepted with an empty BO list");
         /* 2. truncated */
         CHECK(prismrv_validate_stream(&pv, st, words - 1, list, hdr[2]) != 0,
               "truncated stream accepted");
         /* 3. unknown opcode */
         { u32 bad[2] = { 0x7777, 0 };
           CHECK(prismrv_validate_stream(&pv, bad, 2, list, hdr[2]) != 0, "unknown opcode"); }
         /* 4. foreign address: move every BO away */
         { struct drm_gem_object saved[16]; memcpy(saved, objs, sizeof(saved));
           for (unsigned i = 0; i < hdr[2]; i++) objs[i].va += 0x01000000u;
           if (has_addr)
              CHECK(prismrv_validate_stream(&pv, st, words, list, hdr[2]) != 0, "foreign VAs");
           memcpy(objs, saved, sizeof(saved)); }
         /* 5. state values out of range */
         static const struct { u32 op; unsigned idx; u32 val; const char *w; } bad_state[] = {
            { 10, 1, 99, "blend func" }, { 10, 7, 0xff, "colormask" },
            { 11, 2, 9, "depth func" }, { 12, 0, 77, "cull face" },
            { 8, 0, 0x7f800000u, "viewport inf" }, { 9, 0, 5, "scissor enable" },
         };
         for (unsigned k = 0; k < sizeof(bad_state) / sizeof(bad_state[0]); k++) {
            memcpy(m, st, hdr[1]);
            d = find_pkt(m, words, bad_state[k].op, 0, &pn);
            if (!d) continue;
            d[bad_state[k].idx] = bad_state[k].val;
            CHECK(prismrv_validate_stream(&pv, m, words, list, hdr[2]) != 0, bad_state[k].w);
         }
         /* 6. TA stream corrupted after validation of the command stream:
          * snapshot_fill must catch it because it re-validates the copy */
         {
            size_t sz = prismrv_stream_snapshot_size(st, words);
            u8 *snap = malloc(sz);
            u32 dn;
            u32 *dr = find_pkt(m = memcpy(m, st, hdr[1]), words, 5, 0, &dn);
            if (dr) {
               u64 va = ((u64)dr[1] << 32) | dr[0];
               for (unsigned i = 0; i < hdr[2]; i++) {
                  if (va >= objs[i].va && va < objs[i].va + objs[i].size) {
                     u8 *p = objs[i].data + (va - objs[i].va);
                     u32 save[4]; memcpy(save, p, 16);
                     ((u32 *)p)[0] = 0x1234;          /* bad first opcode */
                     CHECK(prismrv_stream_snapshot_fill(st, words, list, hdr[2],
                           0x40000000u, snap, read_bo, NULL) != 0, "corrupt TA accepted");
                     memcpy(p, save, 16);
                  }
               }
            }
            free(snap);
         }
         free(m);
      }
      for (unsigned i = 0; i < hdr[2]; i++) free(objs[i].data);
      free(st);
      n++;
   }
   printf("validated %u Mesa stream(s)\n", n);
   CHECK(n > 0, "dump contained no streams");

   /* ---- shader text: accepted subset vs hostile programs ---- */
   {
      struct prismrv_device p2; u32 ok[2 + 16] = { 2, 0 };
      const char *good = "# hi\nmov r60, #0x3f800000\nvmov o0, r3, swizzle(xxxx)\n"
                         "vmad r70, r1, r2, r3\nfrcp r5, r6\nsmp r80, r32, #3\n";
      memcpy(ok + 2, good, strlen(good) + 1); ok[1] = (strlen(good) + 4) / 4;
      CHECK(prismrv_validate_stream(&p2, ok, 2 + ok[1], NULL, 0) == 0, "good shader rejected");
   }
   fails += reject_prog("ld r0, [r1]\n", "load instruction");
   fails += reject_prog("st [r1], r0\n", "store instruction");
   fails += reject_prog("br 12\n", "branch");
   fails += reject_prog("vmul r256, r1, r2\n", "register out of range");
   fails += reject_prog("vmul r1, o2, r3\n", "output register as source");
   fails += reject_prog("smp r1, r2, #9\n", "texture slot out of range");
   fails += reject_prog("mov r1, #0x1ffffffff\n", "immediate overflow");
   fails += reject_prog("vmul r1, r2\n", "missing operand");
   fails += reject_prog("vmul r1, r2, r3, r4\n", "extra operand");
   fails += reject_prog("vmov r1, r2, swizzle(xxqx)\n", "bad swizzle");
   fails += reject_prog("vmul r1, r2, r3 ; ld r0\n", "trailing junk");
   fails += reject_prog("vmul r1, r2, r3\n\x01\n", "control character");
   fails += reject_prog("\xff\xfe\n", "non-ascii");

   return fails ? 1 : 0;
}
