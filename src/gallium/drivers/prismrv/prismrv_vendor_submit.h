/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * DRM_PRISMRV_SUBMIT_VENDOR argument construction (kept free of the ioctl itself so it can be tested on the host).
 */
#ifndef PRISMRV_VENDOR_SUBMIT_H
#define PRISMRV_VENDOR_SUBMIT_H

#include <stdbool.h>
#include <stdint.h>

#include "drm-uapi/prismrv_drm.h"

struct prismrv_vk_submit_args {
   uint32_t ccb_handle;          /* GEM handle of the BO holding the client CCB ring */
   uint32_t entry_offset;        /* byte offset of this kick's 0x540-byte entry (ring slot) */
   uint32_t hwrc_va;             /* device VA of the HW render context (command ui32Data[1]) */
   bool first_kick;              /* PRISMRV_VKICK_FIRST: bFirstKickOrResume */
   bool last_in_scene;           /* PRISMRV_VKICK_LAST_IN_SCENE: a 3D render follows */
   const uint32_t *bo_handles;   /* every other BO the GPU touches: stream, PDS, USSE code, RT, textures ... */
   unsigned num_bos;
   const int32_t *in_fences;
   unsigned num_in_fences;
};

/* false if a slot/offset cannot be represented (entry not 4-aligned, array/count mismatch) */
bool prismrv_vk_fill_submit(struct drm_prismrv_submit_vendor *out, const struct prismrv_vk_submit_args *a);

#endif
