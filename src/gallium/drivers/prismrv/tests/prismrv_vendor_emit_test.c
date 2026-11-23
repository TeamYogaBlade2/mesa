/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * Golden test of the vendor-ABI kick emitter: prismrv_vk_emit() must reproduce, dword for dword, what the
 * verified model (mainline-tools/ta_emit.py in the prismrv repository) produces.  The vectors are generated
 * by mainline-tools/gen_vendor_emit_golden.py; no vendor code or binary is involved.
 */
#include <stdio.h>
#include <string.h>

#include "../prismrv_vendor_emit.h"

struct golden {
   struct prismrv_vk_state s;
   uint32_t ccb[0x14], ta[18], three_d[59];
   unsigned n_a;
   uint32_t list_a[12];
   unsigned n_b;
   uint32_t list_b[20];
};

static const struct golden golden[] = {
#include "prismrv_vendor_emit_golden.inc"
};

static int fails;
#define CMP(name, k, got, want, n)                                                                  \
   do {                                                                                             \
      for (unsigned i_ = 0; i_ < (n); i_++)                                                         \
         if ((got)[i_] != (want)[i_]) {                                                             \
            fprintf(stderr, "case %u %s[%u]: got 0x%08x want 0x%08x\n", k, name, i_, (got)[i_],      \
                    (want)[i_]);                                                                    \
            fails++;                                                                                \
         }                                                                                          \
   } while (0)

int main(void)
{
   for (unsigned k = 0; k < sizeof(golden) / sizeof(golden[0]); k++) {
      struct prismrv_vk_kick o;
      const struct golden *g = &golden[k];

      prismrv_vk_emit(&g->s, &o);
      CMP("ccb", k, o.ccb, g->ccb, 0x14);
      CMP("ta", k, o.ta, g->ta, 18);
      CMP("three_d", k, o.three_d, g->three_d, 59);
      if (o.n_a != g->n_a || o.n_b != g->n_b) {
         fprintf(stderr, "case %u: list lengths %u/%u want %u/%u\n", k, o.n_a, o.n_b, g->n_a, g->n_b);
         fails++;
         continue;
      }
      CMP("list_a", k, o.list_a, g->list_a, g->n_a);
      CMP("list_b", k, o.list_b, g->list_b, g->n_b);
   }
   printf("%zu cases, %d mismatches\n", sizeof(golden) / sizeof(golden[0]), fails);
   return fails ? 1 : 0;
}
