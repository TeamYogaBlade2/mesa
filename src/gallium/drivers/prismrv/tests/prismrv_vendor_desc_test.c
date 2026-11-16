/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * Every descriptor-object builder must reproduce, dword for dword, the REAL captured objects
 * (mainline-tools/gen_vendor_desc_golden.py in the prismrv repository reads them out of the capture memory).
 */
#include <stdio.h>
#include <string.h>

#include "../prismrv_vendor_desc.h"

enum { DESC_KIND_STATE_LIST, DESC_KIND_SCISSOR, DESC_KIND_ACCESS, DESC_KIND_EPI_PAIR, DESC_KIND_EPI_OUT,
       DESC_KIND_EPI_HDRLESS, DESC_KIND_VPROG1 };

struct golden {
   int kind;
   uint32_t task;
   uint32_t v[6];
   unsigned n;
   uint32_t w[20];
};

static const struct golden golden[] = {
#include "prismrv_vendor_desc_golden.inc"
};

int
main(void)
{
   int fails = 0;

   for (unsigned k = 0; k < sizeof(golden) / sizeof(golden[0]); k++) {
      const struct golden *g = &golden[k];
      uint32_t out[20] = { 0 }, var5[5];
      unsigned n = 0;

      switch (g->kind) {
      case DESC_KIND_STATE_LIST:  n = prismrv_vk_desc_state_list(g->task, g->v[0], g->v[1], out); break;
      case DESC_KIND_SCISSOR:     n = prismrv_vk_desc_scissor(g->v[0], out); break;
      case DESC_KIND_ACCESS:      memcpy(var5, g->v + 1, sizeof(var5)); n = prismrv_vk_desc_access(g->v[0], var5, out); break;
      case DESC_KIND_EPI_PAIR:    n = prismrv_vk_desc_epilogue_pair(g->v, out); break;
      case DESC_KIND_EPI_OUT:     n = prismrv_vk_desc_epilogue_out(g->v, out); break;
      case DESC_KIND_EPI_HDRLESS: n = prismrv_vk_desc_epilogue_hdrless(g->v[0], out); break;
      case DESC_KIND_VPROG1:      n = prismrv_vk_desc_vprog1(g->v[0], g->v[1], g->v[2], g->v[3], out); break;
      }
      if (n != g->n) {
         fprintf(stderr, "vector %u (kind %d task 0x%08x): length %u want %u\n", k, g->kind, g->task, n, g->n);
         fails++;
         continue;
      }
      for (unsigned i = 0; i < n; i++)
         if (out[i] != g->w[i]) {
            fprintf(stderr, "vector %u (kind %d task 0x%08x) dw%u: got 0x%08x want 0x%08x\n", k, g->kind, g->task, i, out[i], g->w[i]);
            fails++;
         }
   }

   /* (task, count) pairs that were never captured are refused: the control word cannot be guessed */
   uint32_t out[20];
   fails += prismrv_vk_desc_state_list(0x1a022203, 0xec01b000, 13, out) != 0;
   fails += prismrv_vk_desc_state_list(0x1a022205, 0xec01b080, 11, out) != 0;
   fails += prismrv_vk_desc_state_list(0x12345678, 0xec01b000, 11, out) != 0;

   printf("%zu captured objects, %d failures\n", sizeof(golden) / sizeof(golden[0]), fails);
   /* list B control word: the fitted rule must reproduce every measured B entry, and flag only the unmeasured ones */
   {
      static const unsigned cases[][2] = { { 0x1a022205, 17 }, { 0x1a022205, 18 }, { 0x1a022207, 17 }, { 0x1a022207, 19 } };
      uint32_t a[PRISMRV_VK_DESC_STATE_LIST_DWORDS], b[PRISMRV_VK_DESC_STATE_LIST_DWORDS];
      bool ex;

      for (unsigned i = 0; i < 4; i++) {
         prismrv_vk_desc_state_list(cases[i][0], 0x1000, cases[i][1], a);
         if (!prismrv_vk_desc_state_list_ex(cases[i][0], 0x1000, cases[i][1], b, &ex) || ex || memcmp(a, b, sizeof(a))) {
            fprintf(stderr, "list B _ex differs from the measured table (%u)\n", i);
            fails++;
         }
         if (a[8] != 0x20002u + cases[i][1] * 0x40u) {
            fprintf(stderr, "list B fitted rule does not reproduce measured ctl 0x%08x\n", a[8]);
            fails++;
         }
      }
      if (!prismrv_vk_desc_state_list_ex(0x1a022207, 0x1000, 20, b, &ex) || !ex || b[8] != 0x20502) {
         fprintf(stderr, "list B count 20 not extrapolated as expected\n");
         fails++;
      }
      if (prismrv_vk_desc_state_list_ex(0x1a022203, 0x1000, 9, b, &ex) || prismrv_vk_desc_state_list_ex(0x1a022205, 0x1000, 21, b, &ex)) {
         fprintf(stderr, "out-of-range list accepted\n");
         fails++;
      }
   }
   return fails ? 1 : 0;
}
