/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * Golden test of the native USSE VMAD encoder.  The vectors are synthetic
 * (generated and checked against the vendor disassembler in the prismrv
 * repository); no vendor code or shader binary is involved.
 */
#include <inttypes.h>
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

   /* ---- test group (comparisons): every word below was disassembled by the vendor
    * disassembler and prints exactly the instruction described here ---- */
   {
      uint64_t w;
      struct prismrv_usse_test t = { .op = PRISMRV_USSE_TEST_AND,
                                     .cond = PRISMRV_USSE_TEST_TAZ, .chan = 0,
                                     .dst_pred = 0, .src1 = 3, .src2 = 8,
                                     .imm_form = true };

      CHECK(prismrv_usse_encode_test(&t, &w), "test instruction rejected");
      CHECK(w == UINT64_C(0x48090181200C0188),
            "and.testtaz.chan0: got %016" PRIx64, w);

      t = (struct prismrv_usse_test){ .op = PRISMRV_USSE_TEST_VF16SUB,
                                      .cond = PRISMRV_USSE_TEST_NANZ, .chan = 0,
                                      .dst_pred = 0, .src1 = 4, .src2 = 6,
                                      .imm_form = false };
      CHECK(prismrv_usse_encode_test(&t, &w), "register-form compare rejected");
      CHECK(w == UINT64_C(0x4808068100038206),
            "vf16sub.testnanz r8, r12: got %016" PRIx64, w);

      t = (struct prismrv_usse_test){ .op = PRISMRV_USSE_TEST_VF16SUB,
                                      .cond = PRISMRV_USSE_TEST_NANZ, .chan = 0,
                                      .dst_pred = 0, .src1 = 4, .src2 = 8,
                                      .imm_form = true };
      CHECK(prismrv_usse_encode_test(&t, &w), "immediate-form compare rejected");
      CHECK(w == UINT64_C(0x4809068120038208),
            "vf16sub.testnanz #8: got %016" PRIx64, w);

      t = (struct prismrv_usse_test){ .op = PRISMRV_USSE_TEST_AND,
                                      .cond = PRISMRV_USSE_TEST_TANZ, .chan = 1,
                                      .dst_pred = 1, .src1 = 7, .src2 = 9,
                                      .imm_form = true };
      CHECK(prismrv_usse_encode_test(&t, &w), "channel/predicate form rejected");
      CHECK(w == UINT64_C(0x48090295200C0389),
            "and.testtanz.chan1 p1: got %016" PRIx64, w);
   }

   {
      struct prismrv_usse_test t = { .op = 64, .cond = 3, .chan = 0 };
      uint64_t w;

      CHECK(!prismrv_usse_encode_test(&t, &w), "opcode 64 accepted");
      t = (struct prismrv_usse_test){ .op = 14, .cond = 22 };
      CHECK(!prismrv_usse_encode_test(&t, &w), "condition 22 accepted");
      t = (struct prismrv_usse_test){ .op = 14, .cond = 3, .chan = 3 };
      CHECK(!prismrv_usse_encode_test(&t, &w), "channel 3 accepted");
      t = (struct prismrv_usse_test){ .op = 14, .cond = 3, .dst_pred = 3 };
      CHECK(!prismrv_usse_encode_test(&t, &w), "predicate 3 accepted");
      t = (struct prismrv_usse_test){ .op = 14, .cond = 3, .src1 = 128 };
      CHECK(!prismrv_usse_encode_test(&t, &w), "source register 128 accepted");
   }

   /* ---- sampler group: the first vector is the word the vendor compiler
    * emitted for "texture2D(tex, uv)" (smp1d r0, r72, r0, drc0) ---- */
   {
      uint64_t w;
      struct prismrv_usse_smp s = { .dim = 1, .drc = 0, .pred = PRISMRV_USSE_PRED_NONE,
                                    .dst_bank = PRISMRV_USSE_BANK_R, .dst = 0,
                                    .coord = 72, .desc = 0 };

      CHECK(prismrv_usse_encode_smp(&s, &w), "smp1d rejected");
      CHECK(w == UINT64_C(0xE000000000090000),
            "vendor smp1d word: got %016" PRIx64, w);

      s.dim = 2; s.dst = 8; s.coord = 0; s.drc = 1; s.desc = 0;
      CHECK(prismrv_usse_encode_smp(&s, &w), "smp2d rejected");
      CHECK(w == UINT64_C(0xE000040101000000),
            "smp2d r8, r0, drc1: got %016" PRIx64, w);

      s = (struct prismrv_usse_smp){ .dim = 3, .drc = 2, .dst_bank = PRISMRV_USSE_BANK_PA,
                                     .dst = 127, .coord = 120, .desc = 2 };
      CHECK(prismrv_usse_encode_smp(&s, &w), "smp3d rejected");
      CHECK(w == UINT64_C(0xE00008820FEF0080),
            "smp3d pa127, r120, r2, drc2: got %016" PRIx64, w);

      s = (struct prismrv_usse_smp){ .dim = 0, .dst_bank = PRISMRV_USSE_BANK_R };
      CHECK(!prismrv_usse_encode_smp(&s, &w), "dimension 0 accepted");
      s.dim = 4;
      CHECK(!prismrv_usse_encode_smp(&s, &w), "dimension 4 accepted");
      s.dim = 1; s.coord = 3;
      CHECK(!prismrv_usse_encode_smp(&s, &w), "odd coordinate accepted");
      s.coord = 0; s.drc = 3;
      CHECK(!prismrv_usse_encode_smp(&s, &w), "drc3 accepted");
      s.drc = 0; s.dst_bank = PRISMRV_USSE_BANK_SA;
      CHECK(!prismrv_usse_encode_smp(&s, &w), "sa destination accepted");
   }

   if (fails)
      return 1;
   printf("PRISMRV USSE ENCODE TEST PASS\n");
   return 0;
}
