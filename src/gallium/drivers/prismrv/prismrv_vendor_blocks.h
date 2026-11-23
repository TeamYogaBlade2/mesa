/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * Data blocks the vendor descriptor objects point at, measured on the captured kicks (analysis/HW_OBJECT_CONTENTS.md,
 * "Datenblöcke"): the clear quad's vertex block and the 10-dword access block.  Encoding verified against the captures; the
 * meaning of the variable access-block words is NOT known.
 */
#ifndef PRISMRV_VENDOR_BLOCKS_H
#define PRISMRV_VENDOR_BLOCKS_H

#include <stdint.h>
#include <string.h>

#define PRISMRV_VK_CLEAR_QUAD_DWORDS 12   /* what prismrv_vk_desc_scissor()'s object (the clear quad's vertex program) fetches */
#define PRISMRV_VK_ACCESS_BLOCK_DWORDS 10 /* prismrv_vk_desc_access() dw1 = 9 */

static inline uint32_t
prismrv_vk_f32(float f)
{
   uint32_t u;
   memcpy(&u, &f, 4);
   return u;
}

/*
 * Three vertices [x y z colour] = (0,0) (w,0) (0,h), z = 1.0, colour 0xff000000 in every capture.  Captured with w = h = 128.0
 * and 256.0 only (100 + 4 kicks); non-square render targets and other colours are extrapolation.  The index data
 * (prismrv_vk_stream_params.clear_quad_va) is the caller's.
 */
static inline void
prismrv_vk_build_clear_quad_vertices(float w, float h, uint32_t colour, uint32_t out[PRISMRV_VK_CLEAR_QUAD_DWORDS])
{
   const uint32_t one = 0x3f800000;
   const uint32_t v[12] = { 0, 0, one, colour,
                            prismrv_vk_f32(w), 0, one, colour,
                            0, prismrv_vk_f32(h), one, colour };
   memcpy(out, v, sizeof(v));
}

/*
 * Index data of the clear triangle: u16 {0, 1, 2, 0} (indices 0,1,2 padded to 8 bytes).  Every one of the 105 captured kicks
 * that draw the clear quad starts its index buffer with exactly these 8 bytes; what follows in that buffer belongs to other draws.
 */
#define PRISMRV_VK_CLEAR_INDEX_DWORDS 2
static inline void
prismrv_vk_build_clear_quad_indices(uint32_t out[PRISMRV_VK_CLEAR_INDEX_DWORDS])
{
   out[0] = 0x00010000;
   out[1] = 0x00000002;
}

/*
 * Access block: [dw0][dw1][0x3c008081][0x3b808081][0x38000080][0x37800080][dw6][dw7][dw8][dw9].  dw2..dw5 are the same in all 25
 * distinct captured blocks.  dw0 is 0xec01b0dc (0xec01b1dc once, a heap 7 address); dw1 and dw6..dw9 hold values of the
 * userspace driver's own resources (host pointers such as 0x4090b698, memory handles such as 0xa000005b, small integers) that
 * change from capture to capture, so they are caller-supplied.
 */
static inline void
prismrv_vk_build_access_block(uint32_t dw0, uint32_t dw1, const uint32_t dw6_9[4], uint32_t out[PRISMRV_VK_ACCESS_BLOCK_DWORDS])
{
   out[0] = dw0;
   out[1] = dw1;
   out[2] = 0x3c008081;
   out[3] = 0x3b808081;
   out[4] = 0x38000080;
   out[5] = 0x37800080;
   memcpy(out + 6, dw6_9, 16);
}

#endif
