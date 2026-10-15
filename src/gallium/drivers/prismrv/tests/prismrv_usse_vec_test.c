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

   if (fails)
      return 1;
   printf("PRISMRV USSE VEC TEST PASS\n");
   return 0;
}
