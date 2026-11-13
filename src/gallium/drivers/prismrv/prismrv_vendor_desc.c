/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 */
#include "prismrv_vendor_desc.h"

#include <string.h>

unsigned
prismrv_vk_desc_state_list(uint32_t task, uint32_t block_va, uint32_t count, uint32_t *out)
{
   static const struct { uint32_t task, count, ctl; } ctl_table[] = {
      { 0x1a022203, 8, 0x00020282 },  { 0x1a022203, 11, 0x00020312 }, { 0x1a022204, 12, 0x00020342 },
      { 0x1a022205, 17, 0x00020442 }, { 0x1a022205, 18, 0x00020482 }, { 0x1a022207, 17, 0x00020442 },
      { 0x1a022207, 19, 0x000204c2 },
   };
   static const uint32_t code[8] = { 0x87000000, 0x90000006, 0x07000103, 0x87040000, 0x90000006, 0x07040103, 0x07080185, 0xaf000000 };
   unsigned i;

   for (i = 0; i < sizeof(ctl_table) / sizeof(ctl_table[0]); i++) {
      if (ctl_table[i].task == task && ctl_table[i].count == count) {
         memset(out, 0, PRISMRV_VK_DESC_STATE_LIST_DWORDS * 4);
         out[0] = block_va;
         out[1] = count;
         out[8] = ctl_table[i].ctl;
         memcpy(out + 12, code, sizeof(code));
         return PRISMRV_VK_DESC_STATE_LIST_DWORDS;
      }
   }
   return 0;
}

unsigned
prismrv_vk_desc_scissor(uint32_t block_va, uint32_t *out)
{
   static const uint32_t t[12] = { 0, 0x00000003, 0x00000010, 0, 0x00020032, 0, 0, 0, 0x67800072, 0x2f0191a3, 0x030401f5, 0xaf000000 };

   memcpy(out, t, sizeof(t));
   out[0] = block_va;
   return PRISMRV_VK_DESC_SCISSOR_DWORDS;
}

unsigned
prismrv_vk_desc_access(uint32_t block_va, const uint32_t var[5], uint32_t *out)
{
   static const uint32_t t[11] = { 0, 9, 0, 0, 0, 0, 0, 0, 0x07018113, 0x070401a5, 0xaf000000 };

   memcpy(out, t, sizeof(t));
   out[0] = block_va;
   memcpy(out + 3, var, 5 * sizeof(uint32_t));
   return PRISMRV_VK_DESC_ACCESS_DWORDS;
}

unsigned
prismrv_vk_desc_epilogue_pair(const uint32_t var[3], uint32_t *out)
{
   static const uint32_t t[16] = { 0x00000800, 0, 0, 0, 0, 0x00000100, 0, 0, 0x00033fe2, 0, 0, 0,
                                   0x070181a6, 0x070581a6, 0x070801a5, 0xaf000000 };

   memcpy(out, t, sizeof(t));
   out[3] = var[0];
   out[6] = var[1];
   out[7] = var[2];
   return PRISMRV_VK_DESC_EPI_PAIR_DWORDS;
}

unsigned
prismrv_vk_desc_epilogue_out(const uint32_t var[4], uint32_t *out)
{
   static const uint32_t t[16] = { 0x00002000, 0, 0, 0, 0, 0x00000100, 0, 0, 0x00033fe2, 0, 0, 0,
                                   0x070181a6, 0x070581a6, 0x070801a5, 0xaf000000 };

   memcpy(out, t, sizeof(t));
   out[3] = var[0];
   out[4] = var[1];
   out[7] = var[2];
   out[10] = var[3];
   return PRISMRV_VK_DESC_EPI_OUT_DWORDS;
}

unsigned
prismrv_vk_desc_epilogue_hdrless(uint32_t dw3, uint32_t *out)
{
   static const uint32_t t[6] = { 0x0002ffe2, 0, 0, 0, 0x070001a5, 0xaf000000 };

   memcpy(out, t, sizeof(t));
   out[3] = dw3;
   return PRISMRV_VK_DESC_EPI_HDRLESS_DWORDS;
}

unsigned
prismrv_vk_desc_vprog1(uint32_t attr_va, uint32_t dwords_m1, uint32_t stride, uint32_t ctl, uint32_t *out)
{
   static const uint32_t t[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0x67800072, 0x2f0191a3, 0x030401f5, 0xaf000000 };

   memcpy(out, t, sizeof(t));
   out[0] = attr_va;
   out[1] = dwords_m1;
   out[2] = stride;
   out[4] = ctl;
   return PRISMRV_VK_DESC_VPROG1_DWORDS;
}
