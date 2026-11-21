/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * The five small programs of the vertex code heap that every frame refers to (analysis/CODE_HANDLES.md): they are identical in
 * 24 captured configurations, so they are fixed per-process programs, here generated with this driver's own encoders.
 *   END      phas.end                                         (the access object's ctl)
 *   CLEAR    phas, or o0..o2 <- pa0..pa2, vmov o1.y <- c3.x, 4x unpack u8->f32 (o3 -> o4.x o4.y o6.x o6.y), emitpds taske
 *            (the clear quad's vertex program: position + a packed colour, which the pixel program copies out)
 *   EPI_A    phas, or o0..o11 <- pa0..pa11, emitpds freep
 *   EPI_B    phas, or o0..o15 <- pa0..pa15, or o16..o17 <- pa16..pa17, emitpds freep
 *   EPI_PAIR phas, or o0..o1 <- pa0..pa1, emitpds freep
 * Their GPU semantics are NOT verified (no hardware); the words are verified against the oracle's decode and the captures.
 */
#ifndef PRISMRV_VENDOR_VSPROG_H
#define PRISMRV_VENDOR_VSPROG_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "prismrv_usse_encode.h"
#include "prismrv_usse_ir.h"

#define PRISMRV_VK_VS_PHAS UINT64_C(0xfa44070000000000)   /* same word as PRISMRV_USSE_PHAS_PIXEL (prismrv_usse_nir.h needs NIR) */
#define PRISMRV_VK_VS_PHAS_END UINT64_C(0xfa4c070000000000)

#define PRISMRV_VK_VS_END_QWORDS      1
#define PRISMRV_VK_VS_CLEAR_QWORDS    8
#define PRISMRV_VK_VS_EPI_A_QWORDS    3
#define PRISMRV_VK_VS_EPI_B_QWORDS    4
#define PRISMRV_VK_VS_EPI_PAIR_QWORDS 3

static inline bool
prismrv_vk_vs_or_copy(unsigned dst, unsigned src, unsigned repeat, uint64_t *out)
{
   struct prismrv_usse_bitwise b;

   memset(&b, 0, sizeof(b));
   b.op = PRISMRV_USSE_BW_OR;
   b.dst.bank = PRISMRV_USSE_BANK_O;
   b.dst.val = dst;
   b.src1.bank = PRISMRV_USSE_BANK_PA;
   b.src1.val = src;
   b.src2.bank = PRISMRV_USSE_BANK_IMM;
   b.repeat = repeat;
   b.flags = PRISMRV_USSE_BW_ONCEONLY | PRISMRV_USSE_BW_SKIPINV;
   return prismrv_usse_encode_bitwise(&b, out);
}

static inline void
prismrv_vk_vs_end(uint64_t out[PRISMRV_VK_VS_END_QWORDS])
{
   out[0] = PRISMRV_VK_VS_PHAS_END;
}

/*
 * The copy program a state-list descriptor's ctl word refers to (measured on 358 captured descriptors, every list length seen):
 * `phas`, `or o<k> <- pa<k>` for the list's (count + 1) dwords in chunks of at most 16, `emitpds freep`.  The second chunk starts
 * at register 16 (count 17 -> 16 + 2, 18 -> 16 + 3, 19 -> 16 + 4 in the captures).  Up to 32 dwords (two chunks) are measured.
 * Returns the number of qwords written (at most 4), 0 when not encodable.
 */
#define PRISMRV_VK_VS_COPY_MAX_QWORDS 4
static inline unsigned
prismrv_vk_vs_copy_program(unsigned ndwords, uint64_t out[PRISMRV_VK_VS_COPY_MAX_QWORDS])
{
   unsigned n = 0, off = 0;

   if (ndwords < 1 || ndwords > 32)
      return 0;
   out[n++] = PRISMRV_VK_VS_PHAS;
   while (off < ndwords) {
      const unsigned c = ndwords - off > 16 ? 16 : ndwords - off;

      if (!prismrv_vk_vs_or_copy(off, off, c, &out[n]))
         return 0;
      n++;
      off += c;
   }
   out[n++] = PRISMRV_USSE_EMITPDS_FREEP;
   return n;
}

static inline bool
prismrv_vk_vs_epi_a(uint64_t out[PRISMRV_VK_VS_EPI_A_QWORDS])
{
   out[0] = PRISMRV_VK_VS_PHAS;
   out[2] = PRISMRV_USSE_EMITPDS_FREEP;
   return prismrv_vk_vs_or_copy(0, 0, 12, &out[1]);
}

static inline bool
prismrv_vk_vs_epi_b(uint64_t out[PRISMRV_VK_VS_EPI_B_QWORDS])
{
   out[0] = PRISMRV_VK_VS_PHAS;
   out[3] = PRISMRV_USSE_EMITPDS_FREEP;
   return prismrv_vk_vs_or_copy(0, 0, 16, &out[1]) && prismrv_vk_vs_or_copy(16, 16, 2, &out[2]);
}

static inline bool
prismrv_vk_vs_epi_pair(uint64_t out[PRISMRV_VK_VS_EPI_PAIR_QWORDS])
{
   out[0] = PRISMRV_VK_VS_PHAS;
   out[2] = PRISMRV_USSE_EMITPDS_FREEP;
   return prismrv_vk_vs_or_copy(0, 0, 2, &out[1]);
}

static inline bool
prismrv_vk_vs_clear(uint64_t out[PRISMRV_VK_VS_CLEAR_QWORDS])
{
   const unsigned fl = PRISMRV_USSE_FLAG_ONCEONLY | PRISMRV_USSE_FLAG_SKIPINV;
   const struct { unsigned byte, reg, mask; } u[4] = { { 0, 4, PRISMRV_USSE_MASK_X }, { 1, 4, PRISMRV_USSE_MASK_Y },
                                                        { 2, 6, PRISMRV_USSE_MASK_X }, { 3, 6, PRISMRV_USSE_MASK_Y } };
   struct prismrv_usse_vmov v;

   out[0] = PRISMRV_VK_VS_PHAS;
   if (!prismrv_vk_vs_or_copy(0, 0, 3, &out[1]))
      return false;
   memset(&v, 0, sizeof(v));
   v.type = PRISMRV_USSE_MOV_FLT;
   v.repeat = 1;
   v.flags = PRISMRV_USSE_FLAG_SKIPINV;
   v.dst.bank = PRISMRV_USSE_BANK_O;
   v.dst.reg = 1;
   v.dst.mask = PRISMRV_USSE_MASK_Y;
   v.src.bank = PRISMRV_USSE_BANK_C;
   v.src.reg = 3;
   strcpy(v.src.swz, "xxxx");
   if (!prismrv_usse_encode_vmov(&v, &out[2]))
      return false;
   for (unsigned i = 0; i < 4; i++)
      if (!prismrv_usse_encode_vunpck_u8f32(u[i].byte, u[i].reg, u[i].mask, 3, fl, &out[3 + i]))
         return false;
   out[7] = PRISMRV_USSE_EMITPDS_TASKE;
   return true;
}

#endif
