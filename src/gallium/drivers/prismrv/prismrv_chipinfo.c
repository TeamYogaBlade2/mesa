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

static const struct prismrv_chip_info prismrv_sgx540_info = {
   .name = "sgx540",
   .core_id = PRISMRV_CORE_SGX540,
   .num_cores = 1,
   .has_isp2 = false,
   .has_multi_event_kick = false,
   .has_pbe_mipmap = true,
   .max_rt_width = 2048,
   .max_rt_height = 2048,
};

static const struct prismrv_chip_info prismrv_sgx530_info = {
   .name = "sgx530",
   .core_id = PRISMRV_CORE_SGX530,
   .num_cores = 1,
   .has_isp2 = false,
   .has_multi_event_kick = false,
   .has_pbe_mipmap = false,
   .max_rt_width = 1024,
   .max_rt_height = 1024,
};

static const struct {
   uint32_t core_id;
   const struct prismrv_chip_info *info;
} prismrv_core_table[] = {
   { PRISMRV_CORE_SGX544, &prismrv_sgx544_info },
   { PRISMRV_CORE_SGX540, &prismrv_sgx540_info },
   { PRISMRV_CORE_SGX530, &prismrv_sgx530_info },
};

const struct prismrv_chip_info *
prismrv_core_lookup(uint32_t core_id)
{
   for (unsigned i = 0; i < ARRAY_SIZE(prismrv_core_table); i++) {
      if (prismrv_core_table[i].core_id == core_id)
         return prismrv_core_table[i].info;
   }
   /* unknown core: fall back to the newest well-known one */
   return &prismrv_sgx544_info;
}
