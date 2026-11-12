/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * prismrv_vk_stream_build() must reproduce the TA control stream of every REAL captured kick of the covered shape
 * (mainline-tools/gen_vendor_stream_golden.py in the prismrv repository keeps only kicks the model reproduces exactly).
 */
#include <stdio.h>

#include "../prismrv_vendor_stream.h"

struct golden {
   const char *src;
   struct prismrv_vk_stream_params p;
   unsigned n;
   uint32_t w[PRISMRV_VK_STREAM_MAX_DWORDS];
};

static const struct golden golden[] = {
#include "prismrv_vendor_stream_golden.inc"
};

int
main(void)
{
   int fails = 0;

   for (unsigned k = 0; k < sizeof(golden) / sizeof(golden[0]); k++) {
      uint32_t out[PRISMRV_VK_STREAM_MAX_DWORDS];
      unsigned n = prismrv_vk_stream_build(&golden[k].p, out, PRISMRV_VK_STREAM_MAX_DWORDS);

      if (n != golden[k].n) {
         fprintf(stderr, "%s: length %u want %u\n", golden[k].src, n, golden[k].n);
         fails++;
         continue;
      }
      for (unsigned i = 0; i < n; i++)
         if (out[i] != golden[k].w[i]) {
            fprintf(stderr, "%s: dw%u got 0x%08x want 0x%08x\n", golden[k].src, i, out[i], golden[k].w[i]);
            fails++;
         }
   }

   /* parameters the format cannot represent are refused */
   struct prismrv_vk_stream_params ok = golden[0].p, bad;
   uint32_t out[PRISMRV_VK_STREAM_MAX_DWORDS];
   bad = ok; bad.n_attrs = 0;           fails += prismrv_vk_stream_build(&bad, out, 29) != 0;
   bad = ok; bad.n_attrs = 128;         fails += prismrv_vk_stream_build(&bad, out, 29) != 0;
   bad = ok; bad.index_count = 0;       fails += prismrv_vk_stream_build(&bad, out, 29) != 0;
   bad = ok; bad.index_count = 64;      fails += prismrv_vk_stream_build(&bad, out, 29) != 0;
   bad = ok; bad.desc_va[2] |= 8;       fails += prismrv_vk_stream_build(&bad, out, 29) != 0;
   fails += prismrv_vk_stream_build(&ok, out, 28) != 0;

   printf("%zu distinct captured streams, %d failures\n", sizeof(golden) / sizeof(golden[0]), fails);
   return fails ? 1 : 0;
}
