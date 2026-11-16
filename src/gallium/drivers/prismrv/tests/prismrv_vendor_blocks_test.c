/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * The clear-quad vertex blocks and the access blocks of the captured kicks (mainline-tools/gen_vendor_blocks_golden.py in the
 * prismrv repository) must be rebuilt from their parameters.
 */
#include <stdio.h>

#include "../prismrv_vendor_blocks.h"

static const uint32_t cq[][12] = {
#define BLOCKS_CLEAR_QUAD
#include "prismrv_vendor_blocks_golden.inc"
#undef BLOCKS_CLEAR_QUAD
};
static const uint32_t ac[][10] = {
#define BLOCKS_ACCESS
#include "prismrv_vendor_blocks_golden.inc"
#undef BLOCKS_ACCESS
};

int
main(void)
{
   int fails = 0;
   uint32_t o[12];

   for (unsigned i = 0; i < sizeof(cq) / sizeof(cq[0]); i++) {
      float w, h;
      memcpy(&w, &cq[i][4], 4);
      memcpy(&h, &cq[i][9], 4);
      prismrv_vk_build_clear_quad_vertices(w, h, cq[i][3], o);
      if (memcmp(o, cq[i], sizeof(cq[i]))) { fprintf(stderr, "clear quad %u differs\n", i); fails++; }
   }
   for (unsigned i = 0; i < sizeof(ac) / sizeof(ac[0]); i++) {
      prismrv_vk_build_access_block(ac[i][0], ac[i][1], &ac[i][6], o);
      if (memcmp(o, ac[i], sizeof(ac[i]))) { fprintf(stderr, "access block %u differs\n", i); fails++; }
   }
   printf("%zu clear-quad blocks, %zu access blocks, %d failures\n", sizeof(cq) / sizeof(cq[0]), sizeof(ac) / sizeof(ac[0]), fails);
   return fails ? 1 : 0;
}
