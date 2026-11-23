/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * Captured (kind, code base, code VA, handle) tuples (mainline-tools/gen_vendor_handles_golden.py in the prismrv repository, base
 * calibrated per capture): encoding and decoding must reproduce the handle / the VA, nibble 7 for pixel and 2 for vertex programs.
 */
#include <stdio.h>

#include "../prismrv_vendor_pds.h"

static const struct { int kind; uint32_t base, va, handle; } golden[] = {
#include "prismrv_vendor_handles_golden.inc"
};

int
main(void)
{
   int fails = 0;

   for (unsigned i = 0; i < sizeof(golden) / sizeof(golden[0]); i++) {
      uint32_t h;
      const uint32_t nib = golden[i].kind ? PRISMRV_VK_HANDLE_VERTEX : PRISMRV_VK_HANDLE_PIXEL;

      if (!prismrv_vk_code_handle(golden[i].base, golden[i].va, nib, &h) || h != golden[i].handle) {
         fprintf(stderr, "tuple %u (kind %d): 0x%08x -> 0x%08x, want 0x%08x\n", i, golden[i].kind, golden[i].va, h, golden[i].handle);
         fails++;
      }
      if ((golden[i].handle & 0xf) != nib || prismrv_vk_code_handle_va(golden[i].base, golden[i].handle) != golden[i].va) {
         fprintf(stderr, "tuple %u: decode mismatch\n", i);
         fails++;
      }
   }
   printf("%zu captured code handles, %d failures\n", sizeof(golden) / sizeof(golden[0]), fails);
   return fails ? 1 : 0;
}
