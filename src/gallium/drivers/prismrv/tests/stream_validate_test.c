/*
 * Host test: compiles the kernel's prismrv_stream.c and runs it over
 * (a) streams dumped from Mesa by the drm-shim and (b) hostile streams.
 * Usage: stream_validate_test <dump-file>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "prismrv_stream.c"   /* copied from the linux tree by run_stream_validate.sh */

static int fails;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); fails++; } } while (0)

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
      uint32_t *st;

      CHECK(hdr[0] == 0x52545350u && hdr[2] <= 16, "dump header");
      for (unsigned i = 0; i < hdr[2]; i++) {
         uint32_t rec[2];
         if (fread(rec, 4, 2, f) != 2) return 1;
         objs[i].va = rec[0]; objs[i].size = rec[1]; list[i] = &objs[i];
      }
      st = malloc(hdr[1]);
      if (fread(st, 1, hdr[1], f) != hdr[1]) return 1;
      CHECK(prismrv_validate_stream(&pv, st, hdr[1] / 4, list, hdr[2]) == 0,
            "Mesa stream rejected by kernel validator");

      /* hostile variants of the same stream must all be rejected */
      {
         unsigned words = hdr[1] / 4;
         /* 1. no BOs listed: every address is foreign */
         unsigned has_addr = 0;
         for (unsigned pos = 0; pos + 2 <= words;) {
            uint32_t op = st[pos], len = st[pos + 1];
            if (op == 5 || op == 7 || (op == 1 && st[pos + 4])) has_addr = 1;
            pos += 2 + len;
         }
         if (has_addr)
            CHECK(prismrv_validate_stream(&pv, st, words, list, 0) != 0,
                  "stream with addresses accepted with an empty BO list");
         /* 2. truncated */
         CHECK(prismrv_validate_stream(&pv, st, words - 1, list, hdr[2]) != 0 ||
               words < 2, "truncated stream accepted");
         /* 3. unknown opcode */
         uint32_t bad[2] = { 0x7777, 0 };
         CHECK(prismrv_validate_stream(&pv, bad, 2, list, hdr[2]) != 0,
               "unknown opcode accepted");
         /* 4. foreign address: shift every BO away */
         for (unsigned i = 0; i < hdr[2]; i++) objs[i].va += 0x01000000u;
         if (has_addr)
            CHECK(prismrv_validate_stream(&pv, st, words, list, hdr[2]) != 0,
                  "stream pointing outside the listed BOs accepted");
      }
      free(st);
      n++;
   }
   printf("validated %u Mesa stream(s)\n", n);
   CHECK(n > 0, "dump contained no streams");
   return fails ? 1 : 0;
}
