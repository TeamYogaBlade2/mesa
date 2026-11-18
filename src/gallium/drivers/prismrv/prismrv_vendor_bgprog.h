/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * The two small pixel programs the per-process PDS objects refer to (analysis/BACKGROUND_PROGRAMS.md): once the descriptor
 * handles are decoded against the code base (code VA = base + (w >> 4) * 8) the targets are in the dumped pages.
 *   END   : a single `phas.end` instruction (8 bytes) - CCB header dw2 and ISP state list A dw3 point at it through the
 *           0x070001a5 PDS record.
 *   CLEAR : `phas` + `or o0, pa0, #0` with end/onceonly/skipinv (16 bytes) - list A dw5 points at it through the 0x070001b5 record.
 * Both are generated with this driver's own encoder.
 */
#ifndef PRISMRV_VENDOR_BGPROG_H
#define PRISMRV_VENDOR_BGPROG_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "prismrv_usse_encode.h"
#include "prismrv_usse_ir.h"

/* the same two words as PRISMRV_USSE_PHAS_END_PIXEL / PRISMRV_USSE_PHAS_PIXEL (prismrv_usse_nir.h; not included: it needs NIR) */
#define PRISMRV_VK_BG_PHAS_END_PIXEL UINT64_C(0xfa4c070000000000)
#define PRISMRV_VK_BG_PHAS_PIXEL     UINT64_C(0xfa44070000000000)

#define PRISMRV_VK_BG_END_DWORDS   2
#define PRISMRV_VK_BG_CLEAR_DWORDS 4

static inline void
prismrv_vk_build_bg_end(uint32_t out[PRISMRV_VK_BG_END_DWORDS])
{
   const uint64_t w = PRISMRV_VK_BG_PHAS_END_PIXEL;
   memcpy(out, &w, 8);
}

static inline bool
prismrv_vk_build_bg_clear(uint32_t out[PRISMRV_VK_BG_CLEAR_DWORDS])
{
   struct prismrv_usse_bitwise b;
   uint64_t w[2] = { PRISMRV_VK_BG_PHAS_PIXEL, 0 };

   memset(&b, 0, sizeof(b));
   b.op = PRISMRV_USSE_BW_OR;
   b.pred = PRISMRV_USSE_PRED_NONE;
   b.dst.bank = PRISMRV_USSE_BANK_O;
   b.src1.bank = PRISMRV_USSE_BANK_PA;
   b.src2.bank = PRISMRV_USSE_BANK_IMM;
   b.repeat = 1;
   b.flags = PRISMRV_USSE_BW_END | PRISMRV_USSE_BW_ONCEONLY | PRISMRV_USSE_BW_SKIPINV;
   if (!prismrv_usse_encode_bitwise(&b, &w[1]))
      return false;
   memcpy(out, w, 16);
   return true;
}

#endif
