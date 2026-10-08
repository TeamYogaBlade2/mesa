/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * Golden test of the native USSE VMAD encoder.  The vectors are synthetic
 * (generated and checked against the vendor disassembler in the prismrv
 * repository); no vendor code or shader binary is involved.
 */
#include <stdio.h>
#include <string.h>

#include "../prismrv_usse_encode.h"

struct golden {
   uint8_t flags, pred;
   struct prismrv_usse_dst dst;
   struct prismrv_usse_src src[3];
   uint64_t word;
};

static const struct golden golden[] = {
#include "prismrv_usse_vmad_golden.inc"
};

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fprintf(stderr, "\n"); fails++; } } while (0)

static struct prismrv_usse_vmad
ok_instr(void)
{
   struct prismrv_usse_vmad i = { 0 };
   for (int n = 0; n < 3; n++)
      memcpy(i.src[n].swz, "xxxx", 5);
   return i;
}

int
main(void)
{
   for (unsigned k = 0; k < sizeof(golden) / sizeof(golden[0]); k++) {
      const struct golden *g = &golden[k];
      struct prismrv_usse_vmad in = { .flags = g->flags, .pred = g->pred,
                                      .dst = g->dst }, back;
      uint64_t w = 0;

      memcpy(in.src, g->src, sizeof(in.src));
      CHECK(prismrv_usse_encode_vmad(&in, &w), "vector %u not encodable", k);
      CHECK(w == g->word, "vector %u: encoded %016llx, expected %016llx", k,
            (unsigned long long)w, (unsigned long long)g->word);
      CHECK(prismrv_usse_decode_vmad(g->word, &back), "vector %u not decodable", k);
      CHECK(!memcmp(&back, &in, sizeof(in)) || 1, "unreachable");
      if (memcmp(&back, &in, sizeof(in))) {
         CHECK(0, "vector %u: decode(word) != operands", k);
      }
   }
   printf("checked %zu golden vectors\n", sizeof(golden) / sizeof(golden[0]));

   /* inputs the hardware cannot express must be refused, not mis-encoded */
   struct prismrv_usse_vmad i;
   uint64_t w;

   i = ok_instr(); i.src[0].reg = 1;
   CHECK(!prismrv_usse_encode_vmad(&i, &w), "odd register accepted");
   i = ok_instr(); i.src[1].reg = 120;
   CHECK(!prismrv_usse_encode_vmad(&i, &w), "r120 (an internal register slot) accepted");
   i = ok_instr(); i.dst.bank = PRISMRV_USSE_BANK_I; i.dst.reg = 4;
   CHECK(!prismrv_usse_encode_vmad(&i, &w), "i4 accepted");
   i = ok_instr(); memcpy(i.src[1].swz, "xzyw", 5);
   CHECK(!prismrv_usse_encode_vmad(&i, &w), "arbitrary swizzle accepted");
   i = ok_instr(); memcpy(i.src[0].swz, "xyyz", 5);   /* a src2 preset, not src1 */
   CHECK(!prismrv_usse_encode_vmad(&i, &w), "another slot's preset accepted");
   i = ok_instr(); i.src[0].neg = true;
   CHECK(!prismrv_usse_encode_vmad(&i, &w), "negate on source 1 accepted");
   i = ok_instr(); i.src[0].bank = PRISMRV_USSE_BANK_O;
   CHECK(!prismrv_usse_encode_vmad(&i, &w), "output bank on source 1 accepted");
   i = ok_instr(); i.dst.mask = 4;
   CHECK(!prismrv_usse_encode_vmad(&i, &w), "bad write mask accepted");
   CHECK(!prismrv_usse_decode_vmad(UINT64_C(1) << 59, &i), "vmul decoded as vmad");
   CHECK(!prismrv_usse_decode_vmad(UINT64_C(1) << 48, &i), "indexed source decoded");
   CHECK(!prismrv_usse_decode_vmad(UINT64_C(1) << 41, &i), "unknown bit decoded");

   if (fails)
      return 1;
   printf("PRISMRV USSE ENCODE TEST PASS\n");
   return 0;
}
