/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * The generated vertex-heap programs must equal the instruction words the oracle decodes in the captured programs (identical in
 * all 24 configurations of analysis/captures/variants10; decode checked with gpu_emu/isa.py).
 */
#include <stdio.h>

#include "../prismrv_vendor_vsprog.h"

#define CHECKP(name, arr, ...)                                                                              \
   do {                                                                                                     \
      const uint64_t want[] = { __VA_ARGS__ };                                                              \
      for (unsigned i = 0; i < sizeof(want) / 8; i++)                                                       \
         if ((arr)[i] != want[i]) {                                                                         \
            fprintf(stderr, "%s qword %u: %016llx want %016llx\n", name, i, (unsigned long long)(arr)[i],  \
                    (unsigned long long)want[i]);                                                           \
            fails++;                                                                                        \
         }                                                                                                  \
   } while (0)

int
main(void)
{
   int fails = 0;
   uint64_t e[1], a[3], b[4], p[3], c[8];

   prismrv_vk_vs_end(e);
   CHECKP("END", e, UINT64_C(0xfa4c070000000000));
   if (!prismrv_vk_vs_epi_a(a) || !prismrv_vk_vs_epi_b(b) || !prismrv_vk_vs_epi_pair(p) || !prismrv_vk_vs_clear(c)) {
      fprintf(stderr, "program not encodable\n");
      return 1;
   }
   CHECKP("EPI_A", a, UINT64_C(0xfa44070000000000), UINT64_C(0x50a1b009a0000000), UINT64_C(0xfb2f4000a0200000));
   CHECKP("EPI_PAIR", p, UINT64_C(0xfa44070000000000), UINT64_C(0x50a11009a0000000), UINT64_C(0xfb2f4000a0200000));
   CHECKP("EPI_B", b, UINT64_C(0xfa44070000000000), UINT64_C(0x50a1f009a0000000), UINT64_C(0x50a11009a2000800),
          UINT64_C(0xfb2f4000a0200000));
   CHECKP("CLEAR", c, UINT64_C(0xfa44070000000000), UINT64_C(0x50a12009a0000000), UINT64_C(0x38820501420400c0),
          UINT64_C(0x40a00185a0840180), UINT64_C(0x40a00189a08d4181), UINT64_C(0x40a00185a0d68182),
          UINT64_C(0x40a00189a0dfc183), UINT64_C(0xfb275000a0200000));
   /* the unpack encoder refuses layouts it has no measurement for */
   {
      uint64_t w;
      if (prismrv_usse_encode_vunpck_u8f32(4, 4, PRISMRV_USSE_MASK_X, 3, 0, &w) ||
          prismrv_usse_encode_vunpck_u8f32(0, 4, PRISMRV_USSE_MASK_Z, 3, 0, &w) ||
          prismrv_usse_encode_vunpck_u8f32(0, 4, PRISMRV_USSE_MASK_X, 2, 0, &w)) {
         fprintf(stderr, "unmeasured unpack layout accepted\n");
         fails++;
      }
   }
   /*
    * state-list copy programs: the (count -> copy chunks) pairs measured on 358 captured descriptors
    * (analysis/CODE_HANDLES.md: 6/7, 8/9, 11/12, 12/13, 17/16+2, 18/16+3, 19/16+4 dwords)
    */
   {
      static const struct { unsigned count, chunks[2]; } m[] = { { 6, { 7, 0 } }, { 8, { 9, 0 } }, { 11, { 12, 0 } }, { 12, { 13, 0 } },
                                                                 { 17, { 16, 2 } }, { 18, { 16, 3 } }, { 19, { 16, 4 } } };
      for (unsigned i = 0; i < sizeof(m) / sizeof(m[0]); i++) {
         uint64_t q[PRISMRV_VK_VS_COPY_MAX_QWORDS], w;
         const unsigned n = prismrv_vk_vs_copy_program(m[i].count + 1, q), nc = m[i].chunks[1] ? 2 : 1;

         if (n != 2 + nc || q[0] != PRISMRV_VK_VS_PHAS || q[n - 1] != PRISMRV_USSE_EMITPDS_FREEP) {
            fprintf(stderr, "copy program for count %u has %u qwords\n", m[i].count, n);
            fails++;
            continue;
         }
         for (unsigned k = 0; k < nc; k++) {
            if (!prismrv_vk_vs_or_copy(k * 16, k * 16, m[i].chunks[k], &w) || q[1 + k] != w) {
               fprintf(stderr, "copy program for count %u chunk %u\n", m[i].count, k);
               fails++;
            }
         }
      }
      /* the generic builder reproduces the fixed epilogue programs and refuses what is not measured */
      {
         uint64_t q[PRISMRV_VK_VS_COPY_MAX_QWORDS];
         if (prismrv_vk_vs_copy_program(12, q) != 3 || q[1] != a[1] || prismrv_vk_vs_copy_program(0, q) || prismrv_vk_vs_copy_program(33, q)) {
            fprintf(stderr, "copy program bounds / epilogue A equivalence\n");
            fails++;
         }
      }
   }
   printf("vertex-heap programs: %d failures\n", fails);
   return fails ? 1 : 0;
}
