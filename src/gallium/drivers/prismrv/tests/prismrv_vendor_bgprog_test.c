/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * The generated background programs must equal the words found at the decoded targets in the captures (variants9/kv10: code VA =
 * code base + (handle >> 4) * 8): END 0xfa4c070000000000, CLEAR 0xfa44070000000000 + 0x50a50009a0000000, and the fragment
 * programs' `or o0, sa10 / sa9, #0` (END|SKIPINV) words the same encoder reproduces.
 */
#include <stdio.h>

#include "../prismrv_vendor_bgprog.h"

int
main(void)
{
   int fails = 0;
   uint32_t e[2], c[4];
   uint64_t w;
   struct prismrv_usse_bitwise b;
   const struct { uint8_t bank, reg; uint64_t want; } fs[] = {
      { PRISMRV_USSE_BANK_SA, 10, UINT64_C(0x50850009e0000500) },
      { PRISMRV_USSE_BANK_SA, 9, UINT64_C(0x50850009e0000480) },
   };

   prismrv_vk_build_bg_end(e);
   if (e[0] != 0 || e[1] != 0xfa4c0700) { fprintf(stderr, "END program 0x%08x%08x\n", e[1], e[0]); fails++; }
   if (!prismrv_vk_build_bg_clear(c) || c[0] != 0 || c[1] != 0xfa440700 || c[2] != 0xa0000000 || c[3] != 0x50a50009) {
      fprintf(stderr, "CLEAR program 0x%08x%08x 0x%08x%08x\n", c[1], c[0], c[3], c[2]);
      fails++;
   }
   for (unsigned i = 0; i < 2; i++) {
      memset(&b, 0, sizeof(b));
      b.op = PRISMRV_USSE_BW_OR; b.dst.bank = PRISMRV_USSE_BANK_O; b.src1.bank = fs[i].bank; b.src1.val = fs[i].reg;
      b.src2.bank = PRISMRV_USSE_BANK_IMM; b.repeat = 1; b.flags = PRISMRV_USSE_BW_END | PRISMRV_USSE_BW_SKIPINV;
      if (!prismrv_usse_encode_bitwise(&b, &w) || w != fs[i].want) { fprintf(stderr, "fragment word %u: %016llx\n", i, (unsigned long long)w); fails++; }
   }
   printf("background programs: %d failures\n", fails);
   return fails ? 1 : 0;
}
