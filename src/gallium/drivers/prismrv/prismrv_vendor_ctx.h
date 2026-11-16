/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * Vendor-path context: the memory arenas the vendor kick needs (each in the heap the hardware path requires), the client-CCB
 * ring, and the assembly of one frame's kick (descriptor objects + TA stream + CCB entry) from the verified builders, with
 * every cross-reference resolved against where the objects actually landed.
 *
 * The DRM calls go through `struct prismrv_vk_ops`, so the whole thing is tested on the host with a fake kernel.
 *
 * WHAT IS NOT WIRED (reported in prismrv_vk_frame_result.unwired, never faked):
 *   - the per-process background program the CCB header and ISP state list A refer to (the fragment binding itself IS wired:
 *     ISP state list B dw5 / dw6, analysis/FRAGMENT_BINDING_REF.md in the prismrv repository),
 *   - the contents of the HW objects the CCB entry points at (HWPBDESC, HWRTDATASET, sync info) and of the HW render
 *     context (`ui32Data[1]`): the caller supplies their VAs, this module does not fill them,
 *   - the contents of the scissor / access blocks, vertex data and the clear quad (caller buffers).
 * Nothing here has run on hardware.
 */
#ifndef PRISMRV_VENDOR_CTX_H
#define PRISMRV_VENDOR_CTX_H

#include <stdbool.h>
#include <stdint.h>

#include "drm-uapi/prismrv_drm.h"
#include "prismrv_vendor_desc.h"
#include "prismrv_vendor_emit.h"
#include "prismrv_vendor_kick.h"
#include "prismrv_vendor_hwobj.h"
#include "prismrv_vendor_pds.h"
#include "prismrv_vendor_stream.h"
#include "prismrv_vendor_submit.h"

struct prismrv_vk_ops {
   void *user;
   /* create a BO in `heap`; returns its handle (0 = failure) and the GPU VA the kernel chose */
   uint32_t (*bo_create)(void *user, uint64_t size, uint32_t heap, uint32_t *gpu_va);
   void *(*bo_map)(void *user, uint32_t handle, uint64_t size);   /* CPU mapping, NULL on failure */
   int (*submit)(void *user, const struct drm_prismrv_submit_vendor *args);
};

struct prismrv_vk_arena {
   uint32_t handle, va, size, used, heap;
   uint8_t *map;
};

#define PRISMRV_VK_RING_ENTRIES 8

enum prismrv_vk_arena_id {
   PRISMRV_VK_ARENA_CCB,       /* client CCB ring, kernel-data heap */
   PRISMRV_VK_ARENA_CODE,      /* USSE shader code, pixel USSE heap */
   PRISMRV_VK_ARENA_PDS_V,     /* descriptor objects + state blocks, vertex PDS heap */
   PRISMRV_VK_ARENA_PDS_P,     /* fragment USSE binding objects, pixel PDS heap */
   PRISMRV_VK_ARENA_DATA,      /* TA stream, default window */
   PRISMRV_VK_ARENA_COUNT,
};

struct prismrv_vk_ctx {
   struct prismrv_vk_ops ops;
   struct prismrv_vk_arena arena[PRISMRV_VK_ARENA_COUNT];
   unsigned ring_next;
   bool first_kick;
};

int prismrv_vk_ctx_init(struct prismrv_vk_ctx *c, const struct prismrv_vk_ops *ops, const char **why);

/* bump-allocate `size` bytes (alignment a power of two); returns the CPU pointer and sets *va, NULL if the arena is full */
uint8_t *prismrv_vk_arena_alloc(struct prismrv_vk_arena *a, uint32_t size, uint32_t align, uint32_t *va);

struct prismrv_vk_frame_in {
   struct prismrv_vk_state gl;        /* stream_va / render_details_va are filled in by the context */
   const void *code;                  /* fragment USSE code */
   uint32_t code_size;                /* multiple of 8 */
   /* standard shape: one attribute, one clear quad, one indexed draw */
   uint32_t attr_va, attr_dwords_m1, attr_stride, vprog_ctl;   /* attribute 0 */
   uint32_t attr1_va, attr1_dwords_m1, attr1_stride;           /* attribute 1, when n_attrs == 2 */
   uint8_t n_attrs;
   bool varying;
   uint32_t clear_quad_va, index_va;
   uint8_t index_count;
   bool lines;
   /* blocks the caller owns the contents of (copied into the PDS arena) */
   const uint32_t *scissor_block; unsigned n_scissor;
   const uint32_t *access_block; unsigned n_access;
   /* caller words whose cause is not understood (descriptor-object `var` arguments) */
   uint32_t access_var[5], epi_pair_var[3], epi_out_var[4], epi_hdrless_dw3;
   /* VAs of the HW objects the CCB entry points at (caller allocated and filled) */
   struct prismrv_vk_alloc hw;
   uint32_t render_details_va;
   /*
    * Optional CPU mappings of the caller's HW objects (hw.hwpbdesc / syncinfo / rtdataset / render_details_va).  When all are
    * given together with `hwobj`, the context writes their contents (prismrv_vendor_hwobj.h); otherwise the caller owns them.
    */
   const struct prismrv_vk_hwobj *hwobj;
   uint32_t *hw_pb_map, *hw_sync_map, *hw_rt_map, *hw_render_details_map;
   uint32_t hwrc_va;
   const uint32_t *extra_bos; unsigned n_extra_bos;   /* BOs the GPU touches that this context did not allocate */
   /* fragment binding object variant words (cause unknown, see prismrv_vk_pds_fs_binding) */
   bool colour_varying;
   uint32_t fs_bind_dw3;
   /* USSE code (heap 4) of the per-process background program the CCB header / list A refer to; 0 = not available */
   uint32_t bg_code_va;
};

enum prismrv_vk_unwired {
   PRISMRV_VK_UNWIRED_BACKGROUND_PROGRAM   = 1u << 0,   /* no bg_code_va: the process-constant objects list A / CCB dw2 refer to are zero */
   PRISMRV_VK_UNWIRED_EXTRAPOLATED_LIST_B  = 1u << 2,   /* list B control word came from the fitted rule, not from a capture */
   PRISMRV_VK_UNWIRED_HW_OBJECT_CONTENTS   = 1u << 1,   /* HWPBDESC / rtdataset / sync info / render details not written by the context (caller's) */
   PRISMRV_VK_UNWIRED_HWRC                 = 1u << 3,   /* hwrc_va's contents are the caller's: never captured with content */
};

struct prismrv_vk_frame_result {
   unsigned slot;                     /* CCB ring slot used */
   uint32_t code_va, binding_va, stream_va;   /* binding_va = the fragment binding object (ISP state list B points at it) */
   uint32_t bg_va, bg2_va, arena2_va;          /* the heap-6 objects CCB dw2 / list A / 3DREGISTERS dw30 refer to */
   unsigned stream_dwords;
   uint32_t unwired;                  /* enum prismrv_vk_unwired bits */
};

/* 0 on success; negative errno with a static message in *why */
int prismrv_vk_ctx_frame(struct prismrv_vk_ctx *c, const struct prismrv_vk_frame_in *in, struct prismrv_vk_frame_result *res,
                         const char **why);

#endif
