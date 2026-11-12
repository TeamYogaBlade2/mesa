/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * USSE program binding: the PDS-heap object that makes a shader's code address reach the GPU.  Encoding found by the
 * prismrv project (analysis/agents/shaderbind.md, TA_KICK_EMITTER_SPEC.md section 8 item 9):
 *
 *     descriptor = (((code_va - 0xf4000000) / 8) << 4) | 7        code_va in the USSE code heap (heap 4), 8-aligned
 *     object     = [descriptor][0][0][0][0x070001a5 (PDS program)][0xaf000000 (halt)]
 *
 * Evidence level: ENCODING.  The 6-dword shape and the descriptor round-trip on every captured binding; what the class
 * nibble (7) selects (USE_CODE_BASE_n) and the DM field are NOT decoded, and nothing here has run on hardware.
 */
#ifndef PRISMRV_VENDOR_PDS_H
#define PRISMRV_VENDOR_PDS_H

#include <stdbool.h>
#include <stdint.h>

#define PRISMRV_VK_CODE_HEAP_BASE  0xf4000000u
#define PRISMRV_VK_CODE_HEAP_SIZE  0x05000000u   /* heap 4: 80 MiB */
#define PRISMRV_VK_PDS_BINDING_DWORDS 6

static inline bool
prismrv_vk_shader_descriptor(uint32_t code_va, uint32_t *out)
{
   if (code_va < PRISMRV_VK_CODE_HEAP_BASE || code_va - PRISMRV_VK_CODE_HEAP_BASE >= PRISMRV_VK_CODE_HEAP_SIZE ||
       (code_va & 7))
      return false;
   *out = (((code_va - PRISMRV_VK_CODE_HEAP_BASE) >> 3) << 4) | 7;
   return true;
}

static inline uint32_t
prismrv_vk_shader_descriptor_va(uint32_t desc)
{
   return PRISMRV_VK_CODE_HEAP_BASE + ((desc >> 4) << 3);
}

/* the 6-dword binding object for a shader at code_va; false if the address cannot be encoded */
static inline bool
prismrv_vk_pds_binding(uint32_t code_va, uint32_t out[PRISMRV_VK_PDS_BINDING_DWORDS])
{
   if (!prismrv_vk_shader_descriptor(code_va, &out[0]))
      return false;
   out[1] = out[2] = out[3] = 0;
   out[4] = 0x070001a5;
   out[5] = 0xaf000000;
   return true;
}

#endif
