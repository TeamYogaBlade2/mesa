/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * Client-CCB entry for DRM_PRISMRV_SUBMIT_VENDOR.  Layout (measured on captured kicks, analysis/KERNEL_KICK_ABI.md and
 * TA_KICK_EMITTER_SPEC.md in the prismrv repository):
 *
 *   +0x000  entry header, 0x50 bytes  (the emitter's ccb[], plus the caller's allocation VAs at +0x24/+0x3c/+0x40/+0x44)
 *   +0x050  CMDTA, 0x508 bytes        (+0x050..+0x4b7: CMDTA_SHARED, filled by the KERNEL - left zero here;
 *                                      +0x4b8: the 18-dword TA block)
 *   entries are 0x540 bytes apart in the ring.
 */
#ifndef PRISMRV_VENDOR_KICK_H
#define PRISMRV_VENDOR_KICK_H

#include "prismrv_vendor_emit.h"

#define PRISMRV_VK_ENTRY_BYTES   0x540
#define PRISMRV_VK_CMDTA_OFFSET  0x50     /* value for drm_prismrv_submit_vendor.ccb_offset */
#define PRISMRV_VK_TA_BLOCK_OFF  (PRISMRV_VK_CMDTA_OFFSET + 0x468)

/* words that depend on how the caller laid out its device memory (not on GL state) */
struct prismrv_vk_alloc {
   uint32_t hwpbdesc;     /* entry +0x24: HWPBDESC object */
   uint32_t syncinfo;     /* entry +0x3c */
   uint32_t rtdataset;    /* entry +0x40 */
   uint32_t rtdata;       /* entry +0x44 */
   uint32_t ta8, ta9;     /* TA block dw8 / dw9: two device VAs (constant per context in every capture) */
   uint32_t ta15;         /* TA block dw15 */
};

/* write the 0x540-byte entry image; `out` has PRISMRV_VK_ENTRY_BYTES / 4 dwords */
void prismrv_vk_build_entry(const struct prismrv_vk_kick *k, const struct prismrv_vk_alloc *a, uint32_t *out);

#endif
