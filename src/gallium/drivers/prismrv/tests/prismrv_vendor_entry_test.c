/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * The client-CCB entry built by prismrv_vk_emit() + prismrv_vk_build_entry() must equal, byte for byte, the entry
 * of a REAL captured kick (mainline-tools/gen_vendor_entry_golden.py in the prismrv repository recovers the GL state
 * and the allocation VAs from each capture and records the captured bytes).
 */
#include <stdio.h>
#include <string.h>

#include "../prismrv_vendor_kick.h"

struct golden {
   const char *src;
   struct prismrv_vk_state s;
   struct prismrv_vk_alloc a;
   unsigned n;
   struct { uint16_t off; uint32_t val; } e[64];
};

static const struct golden golden[] = {
#include "prismrv_vendor_entry_golden.inc"
};

int
main(void)
{
   int fails = 0;

   for (unsigned k = 0; k < sizeof(golden) / sizeof(golden[0]); k++) {
      const struct golden *g = &golden[k];
      uint32_t want[PRISMRV_VK_ENTRY_BYTES / 4] = { 0 }, got[PRISMRV_VK_ENTRY_BYTES / 4];
      struct prismrv_vk_kick o;

      for (unsigned i = 0; i < g->n; i++)
         want[g->e[i].off / 4] = g->e[i].val;
      prismrv_vk_emit(&g->s, &o);
      prismrv_vk_build_entry(&o, &g->a, got);
      for (unsigned i = 0; i < PRISMRV_VK_ENTRY_BYTES / 4; i++) {
         if (got[i] != want[i]) {
            fprintf(stderr, "%s: entry +0x%03x got 0x%08x want 0x%08x\n", g->src, i * 4, got[i], want[i]);
            fails++;
         }
      }
   }
   printf("%zu captured kicks, %d mismatching dwords\n", sizeof(golden) / sizeof(golden[0]), fails);
   return fails ? 1 : 0;
}
