/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * Every distinct fragment-shader binding object that a captured kick's ISP state list B refers to
 * (mainline-tools/gen_vendor_fsbind_golden.py in the prismrv repository) must be reproduced, and the heap-6 references the
 * emitter writes must equal the captured words once the emitter is given the captured object VAs.
 */
#include <stdio.h>
#include <string.h>

#include "../prismrv_vendor_emit.h"
#include "../prismrv_vendor_pds.h"

static const struct {
   uint32_t va;
   int stencil;
   uint32_t hdr_dw2;
   uint32_t words[8];
   uint32_t list_a_refs[2];
} golden[] = {
#include "prismrv_vendor_fsbind_golden.inc"
};

int
main(void)
{
   int fails = 0;

   for (unsigned i = 0; i < sizeof(golden) / sizeof(golden[0]); i++) {
      uint32_t w[8];
      const uint32_t code = prismrv_vk_shader_descriptor_va(golden[i].words[0]);

      if (!prismrv_vk_pds_fs_binding(code, golden[i].words[1] == 6, golden[i].words[3], w)) {
         fprintf(stderr, "vector %u: 0x%08x rejected\n", i, code);
         fails++;
         continue;
      }
      for (unsigned k = 0; k < 8; k++)
         if (w[k] != golden[i].words[k]) {
            fprintf(stderr, "vector %u dw%u: got 0x%08x want 0x%08x\n", i, k, w[k], golden[i].words[k]);
            fails++;
         }

      /* the emitter must reference the object the way the capture does (state list B dw5, dw6 with a stencil record) */
      struct prismrv_vk_state s;
      struct prismrv_vk_kick k;
      memset(&s, 0, sizeof(s));
      s.width = s.height = 64; s.vw = s.vh = 64; s.depth_far = 1.0;
      s.depth_func = PRISMRV_VK_ALWAYS; s.depth_write = true;
      s.color_mask[0] = s.color_mask[1] = s.color_mask[2] = s.color_mask[3] = true;
      s.stencil = golden[i].stencil;
      s.stencil_test = golden[i].stencil;
      s.st_func = PRISMRV_VK_ALWAYS; s.st_write_mask = s.st_compare_mask = 0xff;
      s.pds_fs_va = golden[i].va;
      prismrv_vk_emit(&s, &k);
      if (k.list_b[golden[i].stencil ? 6 : 5] != (golden[i].va >> 4)) {
         fprintf(stderr, "vector %u: list B reference 0x%08x want 0x%08x\n", i, k.list_b[golden[i].stencil ? 6 : 5], golden[i].va >> 4);
         fails++;
      }
      /* kicks without a list A (continuation kicks) carry 0 in list_a_refs and are not compared for it */
      if (k.ccb[2] != golden[i].hdr_dw2 ||
          (golden[i].list_a_refs[0] && (k.list_a[golden[i].stencil ? 4 : 3] != golden[i].list_a_refs[0] ||
                                        k.list_a[golden[i].stencil ? 6 : 5] != golden[i].list_a_refs[1]))) {
         fprintf(stderr, "vector %u: default process references differ from the capture\n", i);
         fails++;
      }
   }

   /* explicit object VAs flow through to every reference */
   struct prismrv_vk_state s;
   struct prismrv_vk_kick k;
   memset(&s, 0, sizeof(s));
   s.width = s.height = 64; s.vw = s.vh = 64; s.depth_far = 1.0; s.depth_func = PRISMRV_VK_ALWAYS;
   s.pds_bg_va = 0xe4020000; s.pds_bg2_va = 0xe4028120; s.pds_fs_va = 0xe4020020; s.pds_arena2_va = 0xe4028000;
   prismrv_vk_emit(&s, &k);
   if (k.ccb[2] != 0x0e402000 || k.list_a[3] != 0x0e402000 || k.list_a[5] != 0x0e402812 || k.list_b[5] != 0x0e402002 ||
       k.three_d[30] != 0xe4028000) {
      fprintf(stderr, "explicit references not applied\n");
      fails++;
   }

   printf("%zu captured fragment bindings, %d failures\n", sizeof(golden) / sizeof(golden[0]), fails);
   return fails ? 1 : 0;
}
