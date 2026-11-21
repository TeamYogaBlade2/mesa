/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * Every distinct set of captured HW objects (mainline-tools/gen_vendor_hwobj_golden.py in the prismrv repository) must be rebuilt
 * from its parameters.
 */
#include <stdio.h>

#include "../prismrv_vendor_hwobj.h"

static const struct {
   uint32_t p[26];
   uint32_t pb[16], ss[14], rs[44];
} golden[] = {
#include "prismrv_vendor_hwobj_golden.inc"
};

int
main(void)
{
   int fails = 0;

   for (unsigned i = 0; i < sizeof(golden) / sizeof(golden[0]); i++) {
      const uint32_t *p = golden[i].p;
      struct prismrv_vk_hwobj h = { p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7] };
      uint32_t *b = &h.blk[0].s;   /* two blocks of 9 words: s,h1a,h1b,h10a,h1c,h10b,h1d,h1e,h1f */
      uint32_t pb[16], ss[14], rs[44];

      for (unsigned k = 0; k < 18; k++)
         b[k] = p[8 + k];
      prismrv_vk_build_hwpbdesc(&h, pb);
      prismrv_vk_build_syncinfo(&h, ss);
      prismrv_vk_build_rtdataset(&h, rs);
      for (unsigned k = 0; k < 16; k++)
         if (pb[k] != golden[i].pb[k]) { fprintf(stderr, "set %u hwpbdesc dw%u: 0x%08x want 0x%08x\n", i, k, pb[k], golden[i].pb[k]); fails++; }
      for (unsigned k = 0; k < 14; k++)
         if (ss[k] != golden[i].ss[k]) { fprintf(stderr, "set %u syncinfo dw%u: 0x%08x want 0x%08x\n", i, k, ss[k], golden[i].ss[k]); fails++; }
      for (unsigned k = 0; k < 44; k++)
         if (rs[k] != golden[i].rs[k]) { fprintf(stderr, "set %u rtdataset dw%u: 0x%08x want 0x%08x\n", i, k, rs[k], golden[i].rs[k]); fails++; }
   }
   printf("%zu captured HW-object sets, %d failures\n", sizeof(golden) / sizeof(golden[0]), fails);
   return fails ? 1 : 0;
}
