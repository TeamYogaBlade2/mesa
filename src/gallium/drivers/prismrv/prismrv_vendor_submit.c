/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 */
#include "prismrv_vendor_submit.h"

#include <string.h>

#include "prismrv_vendor_kick.h"

bool
prismrv_vk_fill_submit(struct drm_prismrv_submit_vendor *out, const struct prismrv_vk_submit_args *a)
{
   if ((a->entry_offset & 3) || (a->num_bos && !a->bo_handles) || (a->num_in_fences && !a->in_fences))
      return false;

   memset(out, 0, sizeof(*out));      /* pad0/pad1 must be zero */
   out->ccb_handle = a->ccb_handle;
   /* the kernel wants the CMDTA, which follows the 0x50-byte entry header */
   out->ccb_offset = a->entry_offset + PRISMRV_VK_CMDTA_OFFSET;
   out->data0 = 0;                    /* ui32Data[0] is 0 in every captured kick */
   out->hwrc_va = a->hwrc_va;
   out->flags = (a->first_kick ? PRISMRV_VKICK_FIRST : 0) | (a->last_in_scene ? PRISMRV_VKICK_LAST_IN_SCENE : 0);
   out->num_bos = a->num_bos;
   out->bos = (uintptr_t)a->bo_handles;
   out->num_in_fences = a->num_in_fences;
   out->in_fences = (uintptr_t)a->in_fences;
   out->out_fence_fd = -1;
   return true;
}
