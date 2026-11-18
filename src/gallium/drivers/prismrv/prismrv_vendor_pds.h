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

/*
 * Code handles: w = (((code_va - code_base) >> 3) << 4) | nibble, nibble 7 for the pixel heap's programs (PDS records) and 2 for
 * the vertex heap's programs (a vertex program object's ctl word at dw[4n], an access object's at dw4).  code_base is the
 * code-base register value of the heap (the UMD's per run constant, 0xf4000668 / 0xfc000010 in the captures; analysis/CODE_HANDLES.md):
 * exact on 40 distinct (base, VA, handle) captured tuples.  The hardware adds the same base, so the driver must encode with the base
 * its kernel programs.
 */
#define PRISMRV_VK_HANDLE_PIXEL  7u
#define PRISMRV_VK_HANDLE_VERTEX 2u

static inline bool
prismrv_vk_code_handle(uint32_t code_base, uint32_t code_va, uint32_t nibble, uint32_t *out)
{
   if (code_va < code_base || (code_va & 7) || ((code_va - code_base) >> 3) >= (1u << 28) || nibble > 0xf)
      return false;
   *out = (((code_va - code_base) >> 3) << 4) | nibble;
   return true;
}

static inline uint32_t
prismrv_vk_code_handle_va(uint32_t code_base, uint32_t handle)
{
   return code_base + ((handle >> 4) << 3);
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

/*
 * The draw's pixel PDS program record (heap 6), the one ISP state list B refers to (dw5, dw6 with a stencil record).  Its handle
 * follows the FRAGMENT shader only (variants3 cfg0/1/6: identical FS, three different VS, same handle); dw1/dw3/dw5 follow
 * properties of the VERTEX shader (varying outputs; non-trivial VS) - analysis/VS_FS_BINDING_SPLIT.md:
 *
 *     [descriptor][dw1][0][dw3][0x070001b5][dw5][dw6][0]            8 dwords, slot pitch 0x20
 *
 * Measured on all 119 captured kicks (analysis/FRAGMENT_BINDING_REF.md).  The object is NOT the one prismrv_vk_pds_binding()
 * builds (that shape, 0x070001a5, is the per-process object CCB header dw2 / list A dw3 point at).
 *   dw1 = 6 and dw5 = 0x07040c12 on the captures whose VS writes a colour varying (variants3 cfg2/3/4/7/8, variants8 cfg5/6),
 *         otherwise dw1 = 2 and dw5 = 0xaf000000 (halt).  `colour_varying` is the observed correlate, not a derived rule
 *         (texture-coordinate varyings did not set it).
 *   dw3 = 0x2fc0000f (0x2f80000f once) for every capture except the trivial-VS cfg0/1 (cfg6 has the same FS as cfg0 yet
 *         gets it, so it is not FS-keyed).  Meaning UNKNOWN, so it is a caller-supplied word.
 *   dw6 = halt when dw3 != 0 or dw5 is not the halt, else 0 (rule fitted to the 13 distinct captured objects).
 */
#define PRISMRV_VK_PDS_FS_BINDING_DWORDS 8

static inline bool
prismrv_vk_pds_fs_binding(uint32_t code_va, bool colour_varying, uint32_t dw3, uint32_t out[PRISMRV_VK_PDS_FS_BINDING_DWORDS])
{
   if (!prismrv_vk_shader_descriptor(code_va, &out[0]))
      return false;
   out[1] = colour_varying ? 6 : 2;
   out[2] = 0;
   out[3] = dw3;
   out[4] = 0x070001b5;
   out[5] = colour_varying ? 0x07040c12 : 0xaf000000;
   out[6] = (colour_varying || dw3) ? 0xaf000000 : 0;
   out[7] = 0;
   return true;
}

#endif
