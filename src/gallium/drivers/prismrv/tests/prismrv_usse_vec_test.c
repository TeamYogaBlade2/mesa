/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * Golden test of the native USSE vec2-group (vmul/vadd/vmin/vmax) and vmov
 * encoders.  Vectors are synthetic, generated and checked against the vendor
 * disassembler in the prismrv repository (tools/usse_oracle/gen_golden_vec.py).
 */
#include <stdio.h>
#include <string.h>

#include "../prismrv_usse_encode.h"

struct g2 { uint8_t op, flags, pred; struct prismrv_usse_dst dst;
            struct prismrv_usse_src src[2]; uint64_t word; };
struct gm { uint8_t type, repeat, flags, pred; struct prismrv_usse_dst dst;
            struct prismrv_usse_src src; uint64_t word; };

static const struct g2 golden2[] = {
#include "prismrv_usse_vec2_golden.inc"
};
static const struct gm goldenm[] = {
#include "prismrv_usse_vmov_golden.inc"
};

struct gp { uint8_t src_fmt, flags, pred; struct prismrv_usse_dst dst;
            struct prismrv_usse_src src, src2; uint64_t word; };
static const struct gp goldenp[] = {
#include "prismrv_usse_vpck_golden.inc"
};

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fprintf(stderr, "\n"); fails++; } } while (0)

static struct prismrv_usse_vec2
ok2(void)
{
   struct prismrv_usse_vec2 i = { .op = PRISMRV_USSE_VADD, .dst.mask = PRISMRV_USSE_MASK_X };
   for (int n = 0; n < 2; n++)
      memcpy(i.src[n].swz, "xxxx", 5);
   return i;
}

static struct prismrv_usse_vmov
okm(void)
{
   struct prismrv_usse_vmov i = { .type = PRISMRV_USSE_MOV_I32, .repeat = 1,
                                  .dst.mask = PRISMRV_USSE_MASK_X };
   memcpy(i.src.swz, "xxxx", 5);
   return i;
}

int
main(void)
{
   uint64_t w;

   for (unsigned k = 0; k < sizeof(golden2) / sizeof(golden2[0]); k++) {
      const struct g2 *g = &golden2[k];
      struct prismrv_usse_vec2 in = { .op = g->op, .flags = g->flags,
                                      .pred = g->pred, .dst = g->dst }, back;

      memcpy(in.src, g->src, sizeof(in.src));
      w = 0;
      CHECK(prismrv_usse_encode_vec2(&in, &w), "vec2 vector %u not encodable", k);
      CHECK(w == g->word, "vec2 vector %u: %016llx != %016llx", k,
            (unsigned long long)w, (unsigned long long)g->word);
      CHECK(prismrv_usse_decode_vec2(g->word, &back) &&
            !memcmp(&back, &in, sizeof(in)), "vec2 vector %u: decode mismatch", k);
   }
   for (unsigned k = 0; k < sizeof(goldenm) / sizeof(goldenm[0]); k++) {
      const struct gm *g = &goldenm[k];
      struct prismrv_usse_vmov in = { .type = g->type, .repeat = g->repeat,
                                      .flags = g->flags, .pred = g->pred,
                                      .dst = g->dst, .src = g->src }, back;

      w = 0;
      CHECK(prismrv_usse_encode_vmov(&in, &w), "vmov vector %u not encodable", k);
      CHECK(w == g->word, "vmov vector %u: %016llx != %016llx", k,
            (unsigned long long)w, (unsigned long long)g->word);
      CHECK(prismrv_usse_decode_vmov(g->word, &back) &&
            !memcmp(&back, &in, sizeof(in)), "vmov vector %u: decode mismatch", k);
   }
   for (unsigned k = 0; k < sizeof(goldenp) / sizeof(goldenp[0]); k++) {
      const struct gp *g = &goldenp[k];
      struct prismrv_usse_vpck in = { .src_fmt = g->src_fmt, .flags = g->flags,
                                      .pred = g->pred, .dst = g->dst,
                                      .src = g->src, .src2 = g->src2 }, back;

      w = 0;
      CHECK(prismrv_usse_encode_vpck(&in, &w), "vpck vector %u not encodable", k);
      CHECK(w == g->word, "vpck vector %u: %016llx != %016llx", k,
            (unsigned long long)w, (unsigned long long)g->word);
      CHECK(prismrv_usse_decode_vpck(g->word, &back), "vpck vector %u: not decodable", k);
      CHECK(back.src_fmt == in.src_fmt && back.flags == in.flags && back.pred == in.pred &&
            !memcmp(&back.dst, &in.dst, sizeof(in.dst)) &&
            back.src.bank == in.src.bank && back.src.reg == in.src.reg &&
            !strcmp(back.src.swz, in.src.swz) &&
            (in.src_fmt != PRISMRV_USSE_PCK_F32 ||
             (back.src2.bank == in.src2.bank && back.src2.reg == in.src2.reg)),
            "vpck vector %u: decode mismatch", k);
   }
   printf("checked %zu vec2 + %zu vmov golden vectors\n",
          sizeof(golden2) / sizeof(golden2[0]), sizeof(goldenm) / sizeof(goldenm[0]));

   /* refusals */
   struct prismrv_usse_vec2 a;
   struct prismrv_usse_vmov m;

   a = ok2(); a.dst.mask = 0;
   CHECK(!prismrv_usse_encode_vec2(&a, &w), "empty vec2 mask accepted");
   a = ok2(); a.src[1].neg = true;
   CHECK(!prismrv_usse_encode_vec2(&a, &w), "negate on vec2 src2 accepted");
   a = ok2(); memcpy(a.src[1].swz, "xzyw", 5);
   CHECK(!prismrv_usse_encode_vec2(&a, &w), "non-preset src2 swizzle accepted");
   a = ok2(); memcpy(a.src[0].swz, "xzyq", 5);
   CHECK(!prismrv_usse_encode_vec2(&a, &w), "bad src1 swizzle char accepted");
   a = ok2(); a.src[0].reg = 1;
   CHECK(!prismrv_usse_encode_vec2(&a, &w), "odd vec2 register accepted");
   a = ok2(); a.dst.bank = PRISMRV_USSE_BANK_I; a.dst.reg = 4;
   CHECK(!prismrv_usse_encode_vec2(&a, &w), "internal dst bank accepted");
   a = ok2(); a.flags = PRISMRV_USSE_FLAG_END;
   CHECK(!prismrv_usse_encode_vec2(&a, &w), "END flag on vec2 accepted");
   CHECK(!prismrv_usse_decode_vec2(UINT64_C(1) << 48, &a), "indexed vec2 decoded");
   CHECK(!prismrv_usse_decode_vec2((UINT64_C(1) << 59) | (UINT64_C(1) << 49) |
                                   (UINT64_C(3) << 30), &a), "r[ih+#n] src1 decoded");
   a = ok2(); a.src[0].bank = PRISMRV_USSE_BANK_C; a.src[0].reg = 5;
   a.src[1].bank = PRISMRV_USSE_BANK_IMM; a.src[1].reg = 63;
   CHECK(prismrv_usse_encode_vec2(&a, &w) && ((w >> 49) & 1) && ((w >> 48) & 1),
         "c/# sources not encoded with the extended-bank bits");
   a.src[1].reg = 64;
   CHECK(!prismrv_usse_encode_vec2(&a, &w), "# register 64 accepted");
   a = ok2(); a.dst.bank = PRISMRV_USSE_BANK_C;
   CHECK(!prismrv_usse_encode_vec2(&a, &w), "c as a destination accepted");
   CHECK(!prismrv_usse_decode_vec2((UINT64_C(1) << 59) | (UINT64_C(7) << 56), &a), "Pn decoded");
   CHECK(!prismrv_usse_decode_vec2((UINT64_C(1) << 59) | (UINT64_C(7) << 12), &a), "vdp decoded as vec2");

   m = okm(); m.src.reg = 7;                      /* vmov is 1:1: odd is fine */
   CHECK(prismrv_usse_encode_vmov(&m, &w), "odd vmov register refused");
   m = okm(); m.src.neg = true;
   CHECK(!prismrv_usse_encode_vmov(&m, &w), "modifier on vmov accepted");
   m = okm(); m.repeat = 5;
   CHECK(!prismrv_usse_encode_vmov(&m, &w), "repeat 5 accepted");
   m = okm(); m.dst.mask = 0;
   CHECK(!prismrv_usse_encode_vmov(&m, &w), "empty vmov mask accepted");
   CHECK(!prismrv_usse_decode_vmov(UINT64_C(7) << 59 | (UINT64_C(5) << 40), &m), "vmov.flt decoded");
   CHECK(!prismrv_usse_decode_vmov(UINT64_C(7) << 59 | 1, &m), "vmov with reserved bit decoded");

   {
      struct prismrv_usse_vpck p = { .src_fmt = PRISMRV_USSE_PCK_F32,
                                     .dst = { PRISMRV_USSE_BANK_O, 0, PRISMRV_USSE_MASK_XYZW },
                                     .src = { PRISMRV_USSE_BANK_R, 4, 0, 0, "xyzw" },
                                     .src2 = { PRISMRV_USSE_BANK_R, 6 } };
      CHECK(prismrv_usse_encode_vpck(&p, &w), "plain vpcku8f32 not encodable");
      p.src.reg = 3;
      CHECK(!prismrv_usse_encode_vpck(&p, &w), "odd vpck source register accepted");
      p.src.reg = 4; p.src_fmt = 3;
      CHECK(!prismrv_usse_encode_vpck(&p, &w), "u16 vpck source accepted");
      p.src_fmt = PRISMRV_USSE_PCK_F32; p.dst.reg = 128;
      CHECK(!prismrv_usse_encode_vpck(&p, &w), "vpck dst register 128 accepted");
      p.dst.reg = 0; p.src.swz[0] = 'q';
      CHECK(!prismrv_usse_encode_vpck(&p, &w), "bad vpck swizzle accepted");
   }
   if (fails)
      return 1;
   printf("PRISMRV USSE VEC TEST PASS\n");
   return 0;
}
