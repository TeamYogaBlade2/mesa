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

   /* more refusals: a primitive-class change needs the 3-dword state-list-A object; too many extra draws; bad extra count */
   bad = ok; bad.n_extra = 1; bad.extra[0] = (struct prismrv_vk_extra_draw){ !ok.lines, 4, 0x1019d000, 0 };
   fails += prismrv_vk_stream_build(&bad, out, PRISMRV_VK_STREAM_MAX_DWORDS) != 0;
   bad = ok; bad.n_extra = 4;           fails += prismrv_vk_stream_build(&bad, out, PRISMRV_VK_STREAM_MAX_DWORDS) != 0;
   bad = ok; bad.n_extra = 1; bad.extra[0] = (struct prismrv_vk_extra_draw){ ok.lines, 0, 0x1019d000, 0 };
   fails += prismrv_vk_stream_build(&bad, out, PRISMRV_VK_STREAM_MAX_DWORDS) != 0;

   /*
    * Primitive-class change, verbatim from the vendor-UMD probe `cap-md2` (analysis/agents/multi-draw-pass.md section 1c:
    * TRIANGLES 3 then LINES 4; 37 dwords).  Only the inserted dwords 18..25 are quoted in the document, so only those
    * (plus the length) are checked here; the rest is the single-draw stream already verified against real captures.
    */
   {
      struct prismrv_vk_stream_params q = {
         .n_attrs = 1, .index_count = 3, .clear_quad_va = 0x10160000, .index_va = 0x1019d000, .full = true,
         .desc_va = { 0xec029000, 0xec01b040, 0xec029080, 0xec01b120, 0xec03a000, 0xec039000, 0xec038040, 0xec038040, 0xec038040,
                      0xec038000 },
         .n_extra = 1, .extra = { { .lines = true, .index_count = 4, .index_va = 0x1019d000, .class_a_va = 0xec029100 } },
      };
      static const uint32_t verbatim[8] = { 0x4ec02910, 0x1a022203, 0x85c00004, 0x1019d000, 0x70000000, 0x003fffff, 0xfec03a00, 0x03800202 };
      unsigned n = prismrv_vk_stream_build(&q, out, PRISMRV_VK_STREAM_MAX_DWORDS);

      if (n != 37) { fprintf(stderr, "cap-md2: length %u want 37\n", n); fails++; }
      else
         for (unsigned i = 0; i < 8; i++)
            if (out[18 + i] != verbatim[i]) {
               fprintf(stderr, "cap-md2 dw%u: got 0x%08x want 0x%08x\n", 18 + i, out[18 + i], verbatim[i]);
               fails++;
            }
      /* same class, three draws: +12 (cap-md3) */
      q.lines = false;
      q.n_extra = 2;
      q.extra[0] = (struct prismrv_vk_extra_draw){ false, 3, 0x1019d000, 0 };
      q.extra[1] = (struct prismrv_vk_extra_draw){ false, 3, 0x1019d000, 0 };
      n = prismrv_vk_stream_build(&q, out, PRISMRV_VK_STREAM_MAX_DWORDS);
      if (n != 41) { fprintf(stderr, "cap-md3: length %u want 41\n", n); fails++; }
   }

   printf("%zu distinct captured streams, %d failures\n", sizeof(golden) / sizeof(golden[0]), fails);
   return fails ? 1 : 0;
}
