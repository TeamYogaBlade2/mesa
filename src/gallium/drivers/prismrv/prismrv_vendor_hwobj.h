/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * The HW objects a client-CCB entry points at: HWPBDESC (entry +0x24, 0x40 bytes), sync info (+0x3c, 0x38 bytes) and the render
 * target dataset with the render target data inside it (+0x40 / +0x44, 0xb0 bytes in one allocation, rtdata = rtdataset + 0x64).
 * Layout measured on 119 captured kicks / 9 distinct object sets (analysis/HW_OBJECT_CONTENTS.md in the prismrv repository);
 * everything here is the capture's encoding, not a hardware-verified model.  Words the caller's allocations decide are
 * parameters; the rest are the constants every capture shows.
 */
#ifndef PRISMRV_VENDOR_HWOBJ_H
#define PRISMRV_VENDOR_HWOBJ_H

#include <stdint.h>
#include <string.h>

#define PRISMRV_VK_HWPBDESC_DWORDS   16
#define PRISMRV_VK_SYNCINFO_DWORDS   14
#define PRISMRV_VK_RTDATASET_DWORDS  44
#define PRISMRV_VK_RTDATA_OFFSET     0x64

/* one half of the render target dataset (the dataset itself and the render target data repeat the same block) */
struct prismrv_vk_rtblock {
   uint32_t s;          /* heap 3 object (0xf004b000 / 0xf004c000) */
   uint32_t h1a, h1b;   /* heap 1 (0xd000a000, +0x3c or +0xb4 into it): equal to 3DREGISTERS dw17 for the dataset, dw8 for the data */
   uint32_t h10a;       /* heap 10 */
   uint32_t h1c;        /* heap 1 */
   uint32_t h10b;       /* heap 10 */
   uint32_t h1d, h1e, h1f;   /* heap 1: 0xd0008000 / 0xd0009000 / 0xd0007000 in every capture */
};

struct prismrv_vk_hwobj {
   uint32_t pb_end, pb_base;   /* parameter buffer range (end, base); 0x802077ff/0x801f0000 once, otherwise 0x80217800/0x80200000 */
   uint32_t pb_heap10;         /* 0xc8822000 in every capture */
   uint32_t pb_next;           /* 0xf0001000 in every capture (heap 3) */
   uint32_t sync_obj;          /* heap 8 object (0xe001f000 / 0xe003a000) */
   uint32_t rtdataset_va;      /* VA of the 0xb0-byte allocation: the object refers to itself at +0x60 / +0xac */
   uint32_t rt_ctl;            /* dw6 and dw25 = sCommand.ui32Data[1], the HW render context address (0x00108368, 0x00109368, 0x0010c368, 0x40805368 in the captures) */
   uint32_t p0;                /* dw0: heap 3 object (0xf0049000) */
   struct prismrv_vk_rtblock blk[2];
   uint32_t sync_obj2;         /* render details +0xec: the heap 8 object one page below sync_obj in the captures; 0 = sync_obj - 0x1000 */
};

static inline void
prismrv_vk_build_hwpbdesc(const struct prismrv_vk_hwobj *h, uint32_t out[PRISMRV_VK_HWPBDESC_DWORDS])
{
   memset(out, 0, PRISMRV_VK_HWPBDESC_DWORDS * 4);
   out[1] = 0x822;
   out[2] = out[6] = h->pb_end;
   out[3] = out[7] = h->pb_base;
   out[4] = h->pb_heap10;
   out[11] = 0x832;
   out[12] = 0x800;
   out[13] = 0x800;
   out[14] = 0x700;
   out[15] = h->pb_next;
}

static inline void
prismrv_vk_build_syncinfo(const struct prismrv_vk_hwobj *h, uint32_t out[PRISMRV_VK_SYNCINFO_DWORDS])
{
   memset(out, 0, PRISMRV_VK_SYNCINFO_DWORDS * 4);
   out[0] = h->sync_obj;
}

static inline void
prismrv_vk_build_rtblock(const struct prismrv_vk_rtblock *b, uint32_t ctl, uint32_t self_ref, uint32_t *o /* 19 dwords from the ctl word */)
{
   o[0] = ctl;
   o[3] = 4;
   o[4] = 1;
   o[5] = 0xcc;
   o[6] = b->s;
   o[7] = b->h1a;
   o[8] = b->h1b;
   o[9] = b->h10a;
   o[10] = b->h1c;
   o[11] = b->h10b;
   o[13] = b->h1d;
   o[14] = b->h1e;
   o[15] = b->h1f;
   o[16] = 0x200;
   o[17] = self_ref;
   o[18] = 0x10;
}

static inline void
prismrv_vk_build_rtdataset(const struct prismrv_vk_hwobj *h, uint32_t out[PRISMRV_VK_RTDATASET_DWORDS])
{
   memset(out, 0, PRISMRV_VK_RTDATASET_DWORDS * 4);
   out[0] = h->p0;
   out[2] = 8;
   out[5] = 2;
   /* block 0 occupies dw6..dw24 (its first word is the control word), block 1 dw25..dw43 */
   uint32_t b0[19] = { 0 }, b1[19] = { 0 };
   prismrv_vk_build_rtblock(&h->blk[0], h->rt_ctl, h->rtdataset_va + 0x60, b0);
   prismrv_vk_build_rtblock(&h->blk[1], h->rt_ctl, h->rtdataset_va + 0xac, b1);
   memcpy(out + 6, b0, sizeof(b0));
   memcpy(out + 25, b1, sizeof(b1));
}

/*
 * HWRENDERDETAILS words after the 3DREGISTERS block (emit's three_d[59], +0x00..+0xeb): +0xec = the sync object one page below
 * syncinfo[0] (2 of 2 observed values), +0xf4 = rtdataset VA, +0xf8 = rtdata VA, +0x14c = 0x02000000; the rest of the 0x47c
 * bytes is zero in every capture.  `out` has PRISMRV_VK_RENDER_DETAILS_BYTES / 4 dwords whose first 59 are already filled.
 */
#define PRISMRV_VK_RENDER_DETAILS_BYTES 0x47c

static inline void
prismrv_vk_build_render_details_tail(const struct prismrv_vk_hwobj *h, uint32_t *out)
{
   out[0xec / 4] = h->sync_obj2 ? h->sync_obj2 : h->sync_obj - 0x1000;
   out[0xf4 / 4] = h->rtdataset_va;
   out[0xf8 / 4] = h->rtdataset_va + PRISMRV_VK_RTDATA_OFFSET;
   out[0x14c / 4] = 0x02000000;
}

#endif
