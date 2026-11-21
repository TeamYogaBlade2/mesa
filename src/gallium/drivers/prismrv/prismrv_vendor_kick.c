/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 */
#include "prismrv_vendor_kick.h"

#include <string.h>

void
prismrv_vk_build_entry(const struct prismrv_vk_kick *k, const struct prismrv_vk_alloc *a, uint32_t *out)
{
   uint32_t *ta = out + PRISMRV_VK_TA_BLOCK_OFF / 4;

   memset(out, 0, PRISMRV_VK_ENTRY_BYTES);
   memcpy(out, k->ccb, sizeof(k->ccb));        /* 0x50 bytes */
   out[0x24 / 4] = a->hwpbdesc;
   out[0x3c / 4] = a->syncinfo;
   out[0x40 / 4] = a->rtdataset;
   out[0x44 / 4] = a->rtdata;

   memcpy(ta, k->ta, sizeof(k->ta));            /* 18 dwords */
   ta[8] = a->ta8;
   ta[9] = a->ta9;
   ta[15] = a->ta15;
}
