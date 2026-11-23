/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * prismrv_chipinfo.c — per-core feature table.
 *
 * One entry per supported SGX core.  The kernel driver exposes the raw
 * EUR_CR_CORE_REVISION register through PRISMRV_PARAM_GPU_ID, and the
 * compatible string it bound against determines the core type; together
 * they select a row here.  Supporting another SGX variant means adding
 * one table row plus any feature quirk handling — no other code churn.
 */
#include "prismrv_device.h"

const struct prismrv_chip_info prismrv_sgx544_info = {
   .name = "sgx544",
   .core_id = PRISMRV_CORE_SGX544,
   .num_cores = 1,
   .has_isp2 = true,
   .has_multi_event_kick = false,
   .has_pbe_mipmap = true,
   .max_rt_width = 4096,
   .max_rt_height = 4096,
};

static const struct {
   uint32_t core_id;
   const struct prismrv_chip_info *info;
} prismrv_core_table[] = {
   { PRISMRV_CORE_SGX544, &prismrv_sgx544_info },
};

const struct prismrv_chip_info *
prismrv_core_lookup(uint32_t core_id)
{
   for (unsigned i = 0; i < ARRAY_SIZE(prismrv_core_table); i++) {
      if (prismrv_core_table[i].core_id == core_id)
         return prismrv_core_table[i].info;
   }
   /*
    * Unknown core: return NULL so the caller can fail cleanly.
    *
    * The previous fallback to SGX544 was dangerous: an unsupported
    * core (different register layout, different errata, different
    * shader capability) would silently continue with wrong feature
    * flags and possibly wrong errata assumptions, leading to GPU
    * hangs or memory corruption that look unrelated to the root cause.
    *
    * Bring-up engineers should add a table entry for the new core
    * rather than relying on a fallback.
    */
   return NULL;
}
