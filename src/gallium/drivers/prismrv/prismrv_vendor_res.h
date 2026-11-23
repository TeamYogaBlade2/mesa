/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * The memory a vendor frame reaches besides the context's arenas (analysis/GALLIUM_FRAME_RESOURCES.md in the prismrv repository: the
 * allocations reachable from one captured CCB entry, with their roles).  One scene = one render target set: the TA blocks and
 * HW render data objects are created once and reused by every frame.  All BOs are created in their vendor heap and zeroed; only the
 * parameter-buffer free list has measured contents.  Nothing here has run on hardware.
 */
#ifndef PRISMRV_VENDOR_RES_H
#define PRISMRV_VENDOR_RES_H

#include <stdint.h>

#include "prismrv_vendor_ctx.h"
#include "prismrv_vendor_hwobj.h"

enum prismrv_vk_scene_bo {
   SCENE_ZBUF,                 /* default window, 0x80004 B zero buffer: CCB dw3 / list A,B words point inside */
   SCENE_TA_F, SCENE_TA_D, SCENE_TA_E,       /* heap 1: 0x200, 0x80, 0x90 (rtblock h1f, h1d, h1e) */
   SCENE_TA_A0, SCENE_TA_C0,                 /* heap 1: 0x400, 0x6e8 (block 0: h1a/h1b, h1c) */
   SCENE_TA_A1, SCENE_TA_C1,                 /* heap 1: 0x400, 0x6e8 (block 1) */
   SCENE_HWPB, SCENE_PB_NEXT, SCENE_RTDATASET, SCENE_P0, SCENE_S0, SCENE_S1, SCENE_SYNCINFO, SCENE_RDETAILS,   /* heap 3 */
   SCENE_SYNC1, SCENE_SYNC2,                 /* heap 8: two 8-byte sync objects */
   SCENE_PB, SCENE_PB_FREELIST, SCENE_H10A, SCENE_H10B,      /* heap 10: parameter buffer, its free list, two small blocks */
   SCENE_BO_COUNT
};

struct prismrv_vk_scene_bo_info {
   uint32_t handle, va, size;
   uint8_t *map;
};

struct prismrv_vk_scene {
   struct prismrv_vk_scene_bo_info bo[SCENE_BO_COUNT];
   uint32_t handles[SCENE_BO_COUNT];   /* for drm_prismrv_submit_vendor.bos */
   struct prismrv_vk_hwobj hw;         /* every parameter filled except rt_ctl (= hwrc_va, set per frame) */
   uint32_t ta8_va, ta17_va, ccb3_va;  /* emit state */
};

/* doubly linked free list of parameter-buffer pages: entry i = (prev << 16) | next, 0xffff = none (34 entries in every capture) */
#define PRISMRV_VK_PB_PAGES 34
static inline void
prismrv_vk_build_pb_freelist(unsigned pages, uint32_t *out)
{
   for (unsigned i = 0; i < pages; i++) {
      const uint32_t prev = i ? i - 1 : 0xffff, next = i + 1 < pages ? i + 1 : 0xffff;

      out[i] = prev << 16 | next;
   }
}

int prismrv_vk_scene_create(struct prismrv_vk_ctx *c, struct prismrv_vk_scene *s, const char **why);

/* point the frame description at the scene (HW objects, mappings, emit addresses, BOs); rt/depth VAs and the HWRC are the caller's */
void prismrv_vk_scene_fill_frame(const struct prismrv_vk_scene *s, struct prismrv_vk_frame_in *in);

#endif
