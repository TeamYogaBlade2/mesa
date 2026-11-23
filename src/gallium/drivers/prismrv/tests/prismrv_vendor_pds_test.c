/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * Every distinct 6-dword USSE-binding object found in the captured memory (mainline-tools/gen_vendor_pds_golden.py in
 * the prismrv repository) must be reproduced from its decoded code address.
 */
#include <stdio.h>

#include "../prismrv_vendor_pds.h"

static const struct {
   uint32_t va;
   uint32_t words[6];
} golden[] = {
#include "prismrv_vendor_pds_golden.inc"
};

int
main(void)
{
   int fails = 0;

   for (unsigned i = 0; i < sizeof(golden) / sizeof(golden[0]); i++) {
      uint32_t w[6];

      if (prismrv_vk_shader_descriptor_va(golden[i].words[0]) != golden[i].va) {
         fprintf(stderr, "vector %u: descriptor decodes to 0x%08x, want 0x%08x\n", i,
                 prismrv_vk_shader_descriptor_va(golden[i].words[0]), golden[i].va);
         fails++;
      }
      if (!prismrv_vk_pds_binding(golden[i].va, w)) {
         fprintf(stderr, "vector %u: 0x%08x rejected\n", i, golden[i].va);
         fails++;
         continue;
      }
      for (unsigned k = 0; k < 6; k++)
         if (w[k] != golden[i].words[k]) {
            fprintf(stderr, "vector %u dw%u: got 0x%08x want 0x%08x\n", i, k, w[k], golden[i].words[k]);
            fails++;
         }
   }

   /* addresses the hardware form cannot represent */
   uint32_t w[6];
   const uint32_t bad[] = { 0xf3fffff8, 0xf4000004, 0xf9000000, 0, 0xf4000001 };
   for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
      if (prismrv_vk_pds_binding(bad[i], w)) {
         fprintf(stderr, "bad address 0x%08x accepted\n", bad[i]);
         fails++;
      }

   printf("%zu captured bindings, %d failures\n", sizeof(golden) / sizeof(golden[0]), fails);
   return fails ? 1 : 0;
}
