/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 */
#include "prismrv_vendor_ctx.h"

#include <errno.h>
#include <string.h>

/* heap ranges the kernel must honour (UAPI v6; vendor sgxconfig.h) */
static const struct { uint32_t heap, base, size; } heap_range[] = {
   { PRISMRV_HEAP_DEFAULT,      0x10000000u, 0x30000000u },
   { PRISMRV_HEAP_KERNEL_DATA,  0xf0000000u, 0x02fff000u },
   { PRISMRV_HEAP_PIXEL_USSE,   0xf4000000u, 0x04fff000u },
   { PRISMRV_HEAP_PDS_PIXEL,    0xe4000000u, 0x01fff000u },
   { PRISMRV_HEAP_PDS_VERTEX,   0xec000000u, 0x01bff000u },
};

static const struct { uint32_t heap, size; } arena_spec[PRISMRV_VK_ARENA_COUNT] = {
   [PRISMRV_VK_ARENA_CCB]   = { PRISMRV_HEAP_KERNEL_DATA, 0x3000 },   /* 8 entries x 0x540 */
   [PRISMRV_VK_ARENA_CODE]  = { PRISMRV_HEAP_PIXEL_USSE, 0x8000 },
   [PRISMRV_VK_ARENA_PDS_V] = { PRISMRV_HEAP_PDS_VERTEX, 0x8000 },
   [PRISMRV_VK_ARENA_PDS_P] = { PRISMRV_HEAP_PDS_PIXEL, 0x8000 },
   [PRISMRV_VK_ARENA_DATA]  = { PRISMRV_HEAP_DEFAULT, 0x10000 },
};

static bool
va_in_heap(uint32_t heap, uint32_t va, uint32_t size)
{
   for (unsigned i = 0; i < sizeof(heap_range) / sizeof(heap_range[0]); i++)
      if (heap_range[i].heap == heap)
         return va >= heap_range[i].base && (uint64_t)va + size <= (uint64_t)heap_range[i].base + heap_range[i].size;
   return false;
}

int
prismrv_vk_ctx_init(struct prismrv_vk_ctx *c, const struct prismrv_vk_ops *ops, const char **why)
{
#define FAIL(e, m) do { *why = m; return e; } while (0)
   memset(c, 0, sizeof(*c));
   c->ops = *ops;
   c->first_kick = true;
   if (!ops->bo_create || !ops->bo_map || !ops->submit)
      FAIL(-EINVAL, "incomplete ops");

   for (unsigned i = 0; i < PRISMRV_VK_ARENA_COUNT; i++) {
      struct prismrv_vk_arena *a = &c->arena[i];

      a->heap = arena_spec[i].heap;
      a->size = arena_spec[i].size;
      a->handle = ops->bo_create(ops->user, a->size, a->heap, &a->va);
      if (!a->handle)
         FAIL(-ENOMEM, "BO creation failed (kernel without UAPI v6 heap support?)");
      /* the vendor path is load-bearing on placement: refuse a BO the kernel put somewhere else */
      if (!va_in_heap(a->heap, a->va, a->size))
         FAIL(-EFAULT, "kernel placed a BO outside the requested heap");
      a->map = ops->bo_map(ops->user, a->handle, a->size);
      if (!a->map)
         FAIL(-ENOMEM, "BO mapping failed");
      memset(a->map, 0, a->size);
   }
   return 0;
}

uint8_t *
prismrv_vk_arena_alloc(struct prismrv_vk_arena *a, uint32_t size, uint32_t align, uint32_t *va)
{
   uint32_t off = (a->used + align - 1) & ~(align - 1);

   if (align == 0 || (align & (align - 1)) || off > a->size || size > a->size - off)
      return NULL;
   a->used = off + size;
   *va = a->va + off;
   return a->map + off;
}

#define ALLOC(id, bytes, align, var_va) prismrv_vk_arena_alloc(&c->arena[id], (bytes), (align), &(var_va))

int
prismrv_vk_ctx_frame(struct prismrv_vk_ctx *c, const struct prismrv_vk_frame_in *in, struct prismrv_vk_frame_result *res,
                     const char **why)
{
   struct prismrv_vk_state gl = in->gl;
   struct prismrv_vk_kick k;
   struct prismrv_vk_stream_params sp;
   uint32_t stream_va, code_va, bind_va, blk_a_va, blk_b_va, sc_va, ac_va, a_va, b_va, scd_va, acd_va, vp_va, e0_va, ep_va, eo_va;
   uint32_t w[PRISMRV_VK_STREAM_MAX_DWORDS];
   bool extrap_b = false, hw_written;
   uint32_t entry[PRISMRV_VK_ENTRY_BYTES / 4], bos[8 + 32];
   uint8_t *p;
   unsigned n, slot, nb = 0;

   if (in->n_attrs != 1 && in->n_attrs != 2)
      FAIL(-ENOTSUP, "vertex programs with more than two attributes are not built (3 attributes captured once, layout ambiguous)");
   if (!in->code || !in->code_size || (in->code_size & 7))
      FAIL(-EINVAL, "shader code size must be a non-zero multiple of 8");
   if (in->n_extra_bos > 32)
      FAIL(-EINVAL, "too many extra BOs");

   /* 1. shader code and the heap-6 objects: [bg object 0x20][fragment binding 0x20][bg2 object]; ISP state list B points at the
    * fragment binding (va >> 4), the CCB header and list A at the two background objects (measured, not hardware verified) */
   if (!(p = ALLOC(PRISMRV_VK_ARENA_CODE, in->code_size, 8, code_va)))
      FAIL(-ENOSPC, "code arena full");
   memcpy(p, in->code, in->code_size);
   {
      uint32_t fb[PRISMRV_VK_PDS_FS_BINDING_DWORDS], bg[PRISMRV_VK_PDS_BINDING_DWORDS] = { 0 };
      uint32_t bg2[8] = { 0 };

      if (!prismrv_vk_pds_fs_binding(code_va, in->colour_varying, in->fs_bind_dw3, fb))
         FAIL(-EFAULT, "code address cannot be encoded as a binding descriptor");
      if (in->bg_code_va) {
         if (!prismrv_vk_pds_binding(in->bg_code_va, bg))
            FAIL(-EFAULT, "background code address cannot be encoded as a binding descriptor");
         bg2[0] = bg[0]; bg2[1] = 6; bg2[3] = 0x03c0a00f; bg2[4] = 0x070001b5; bg2[5] = 0x07040c02; bg2[6] = 0xaf000000;
      }
      uint8_t *pb;
      if (!(pb = ALLOC(PRISMRV_VK_ARENA_PDS_P, 0x20, 0x20, gl.pds_bg_va)))
         FAIL(-ENOSPC, "pixel PDS arena full");
      memcpy(pb, bg, sizeof(bg));
      if (!(pb = ALLOC(PRISMRV_VK_ARENA_PDS_P, 0x20, 0x20, gl.pds_fs_va)))
         FAIL(-ENOSPC, "pixel PDS arena full");
      memcpy(pb, fb, sizeof(fb));
      if (!(pb = ALLOC(PRISMRV_VK_ARENA_PDS_P, 0x40, 0x10, gl.pds_bg2_va)))
         FAIL(-ENOSPC, "pixel PDS arena full");
      memcpy(pb, bg2, sizeof(bg2));
      bind_va = gl.pds_fs_va;
      gl.pds_arena2_va = c->arena[PRISMRV_VK_ARENA_PDS_P].va;   /* UNVERIFIED: the captures' second arena base; here the arena itself */
   }

   /* 2. TA stream storage (filled last, once every descriptor VA is known) and the state it needs */
   if (!(p = ALLOC(PRISMRV_VK_ARENA_DATA, PRISMRV_VK_STREAM_MAX_DWORDS * 4, 16, stream_va)))
      FAIL(-ENOSPC, "data arena full");
   uint8_t *stream_map = p;

   gl.stream_va = stream_va;
   gl.render_details_va = in->render_details_va;
   prismrv_vk_emit(&gl, &k);

   /* 3. state blocks (ISP lists from the emitter, scissor/access from the caller) */
   if (!(p = ALLOC(PRISMRV_VK_ARENA_PDS_V, k.n_a * 4, 16, blk_a_va))) FAIL(-ENOSPC, "vertex PDS arena full");
   memcpy(p, k.list_a, k.n_a * 4);
   if (!(p = ALLOC(PRISMRV_VK_ARENA_PDS_V, k.n_b * 4, 16, blk_b_va))) FAIL(-ENOSPC, "vertex PDS arena full");
   memcpy(p, k.list_b, k.n_b * 4);
   if (!(p = ALLOC(PRISMRV_VK_ARENA_PDS_V, in->n_scissor ? in->n_scissor * 4 : 16, 16, sc_va))) FAIL(-ENOSPC, "vertex PDS arena full");
   if (in->n_scissor) memcpy(p, in->scissor_block, in->n_scissor * 4);
   if (!(p = ALLOC(PRISMRV_VK_ARENA_PDS_V, in->n_access ? in->n_access * 4 : 16, 16, ac_va))) FAIL(-ENOSPC, "vertex PDS arena full");
   if (in->n_access) memcpy(p, in->access_block, in->n_access * 4);

   /* 4. descriptor objects */
   {
      uint32_t o[20];
      unsigned len;
      const bool full = gl.vx == 0 && gl.vy == 0 && gl.vw == (int32_t)gl.width && gl.vh == (int32_t)gl.height;
      const uint32_t task_a = gl.stencil ? 0x1a022204 : 0x1a022203;
      const uint32_t task_b = full ? 0x1a022205 : 0x1a022207;   /* must equal the task word the stream builder emits */

      len = prismrv_vk_desc_state_list(task_a, blk_a_va, k.n_a, o);
      if (!len) FAIL(-ENOTSUP, "state list A: dword count not measured");
      if (!(p = ALLOC(PRISMRV_VK_ARENA_PDS_V, len * 4, 16, a_va))) FAIL(-ENOSPC, "vertex PDS arena full");
      memcpy(p, o, len * 4);

      len = prismrv_vk_desc_state_list_ex(task_b, blk_b_va, k.n_b, o, &extrap_b);
      if (!len) FAIL(-ENOTSUP, "state list B: dword count outside the measured/fitted range");
      if (!(p = ALLOC(PRISMRV_VK_ARENA_PDS_V, len * 4, 16, b_va))) FAIL(-ENOSPC, "vertex PDS arena full");
      memcpy(p, o, len * 4);

      len = prismrv_vk_desc_scissor(sc_va, o);
      if (!(p = ALLOC(PRISMRV_VK_ARENA_PDS_V, len * 4, 16, scd_va))) FAIL(-ENOSPC, "vertex PDS arena full");
      memcpy(p, o, len * 4);

      len = prismrv_vk_desc_access(ac_va, in->access_var, o);
      if (!(p = ALLOC(PRISMRV_VK_ARENA_PDS_V, len * 4, 16, acd_va))) FAIL(-ENOSPC, "vertex PDS arena full");
      memcpy(p, o, len * 4);

      if (in->n_attrs == 2) {
         const uint32_t a0[3] = { in->attr_va, in->attr_dwords_m1, in->attr_stride };
         const uint32_t a1[3] = { in->attr1_va, in->attr1_dwords_m1, in->attr1_stride };
         len = prismrv_vk_desc_vprog2(a0, a1, in->vprog_ctl, o);
      } else {
         len = prismrv_vk_desc_vprog1(in->attr_va, in->attr_dwords_m1, in->attr_stride, in->vprog_ctl, o);
      }
      if (!(p = ALLOC(PRISMRV_VK_ARENA_PDS_V, len * 4, 16, vp_va))) FAIL(-ENOSPC, "vertex PDS arena full");
      memcpy(p, o, len * 4);

      len = prismrv_vk_desc_epilogue_hdrless(in->epi_hdrless_dw3, o);
      if (!(p = ALLOC(PRISMRV_VK_ARENA_PDS_V, len * 4, 16, e0_va))) FAIL(-ENOSPC, "vertex PDS arena full");
      memcpy(p, o, len * 4);

      /* the three 0x1a02{4,6,8}201 records share ONE object in every capture */
      len = prismrv_vk_desc_epilogue_pair(in->epi_pair_var, o);
      if (!(p = ALLOC(PRISMRV_VK_ARENA_PDS_V, len * 4, 16, ep_va))) FAIL(-ENOSPC, "vertex PDS arena full");
      memcpy(p, o, len * 4);

      len = prismrv_vk_desc_epilogue_out(in->epi_out_var, o);
      if (!(p = ALLOC(PRISMRV_VK_ARENA_PDS_V, len * 4, 16, eo_va))) FAIL(-ENOSPC, "vertex PDS arena full");
      memcpy(p, o, len * 4);
   }

   /* 5. the TA stream, pointing at exactly where the objects landed */
   memset(&sp, 0, sizeof(sp));
   sp.stencil = gl.stencil;
   sp.varying = in->varying;
   sp.n_attrs = in->n_attrs;
   sp.index_count = in->index_count;
   sp.clear_quad_va = in->clear_quad_va;
   sp.index_va = in->index_va;
   sp.lines = in->lines;
   sp.full = gl.vx == 0 && gl.vy == 0 && gl.vw == (int32_t)gl.width && gl.vh == (int32_t)gl.height;   /* same rule as `full` above */
   sp.desc_va[PRISMRV_VK_DESC_STATE_A] = a_va;
   sp.desc_va[PRISMRV_VK_DESC_SCISSOR] = scd_va;
   sp.desc_va[PRISMRV_VK_DESC_STATE_B] = b_va;
   sp.desc_va[PRISMRV_VK_DESC_ACCESS] = acd_va;
   sp.desc_va[PRISMRV_VK_DESC_VPROG] = vp_va;
   sp.desc_va[PRISMRV_VK_DESC_EPILOGUE0 + 0] = e0_va;
   sp.desc_va[PRISMRV_VK_DESC_EPILOGUE0 + 1] = ep_va;
   sp.desc_va[PRISMRV_VK_DESC_EPILOGUE0 + 2] = ep_va;
   sp.desc_va[PRISMRV_VK_DESC_EPILOGUE0 + 3] = ep_va;
   sp.desc_va[PRISMRV_VK_DESC_EPILOGUE0 + 4] = eo_va;
   n = prismrv_vk_stream_build(&sp, w, PRISMRV_VK_STREAM_MAX_DWORDS);
   if (!n)
      FAIL(-ENOTSUP, "stream parameters not representable");
   memcpy(stream_map, w, n * 4);

   /* 5b. HW objects, when the caller handed over their mappings */
   hw_written = in->hwobj && in->hw_pb_map && in->hw_sync_map && in->hw_rt_map && in->hw_render_details_map;
   if (hw_written) {
      if (in->hw.rtdata != in->hw.rtdataset + PRISMRV_VK_RTDATA_OFFSET || in->hwobj->rtdataset_va != in->hw.rtdataset)
         FAIL(-EINVAL, "rtdata must sit at rtdataset + 0x64 and hwobj->rtdataset_va must equal hw.rtdataset");
      /* the rtdataset's dw6/dw25 equal sCommand.ui32Data[1] (the HWRC address) in all 119 captured kicks */
      struct prismrv_vk_hwobj ho = *in->hwobj;
      ho.rt_ctl = in->hwrc_va;
      prismrv_vk_build_hwpbdesc(&ho, in->hw_pb_map);
      prismrv_vk_build_syncinfo(&ho, in->hw_sync_map);
      prismrv_vk_build_rtdataset(&ho, in->hw_rt_map);
      memset(in->hw_render_details_map, 0, PRISMRV_VK_RENDER_DETAILS_BYTES);
      memcpy(in->hw_render_details_map, k.three_d, sizeof(k.three_d));
      prismrv_vk_build_render_details_tail(&ho, in->hw_render_details_map);
   }

   /* 6. CCB entry into the next ring slot */
   slot = c->ring_next % PRISMRV_VK_RING_ENTRIES;
   prismrv_vk_build_entry(&k, &in->hw, entry);
   memcpy(c->arena[PRISMRV_VK_ARENA_CCB].map + (size_t)slot * PRISMRV_VK_ENTRY_BYTES, entry, sizeof(entry));

   /* 7. submit: every BO the GPU touches */
   for (unsigned i = 0; i < PRISMRV_VK_ARENA_COUNT; i++)
      if (i != PRISMRV_VK_ARENA_CCB)
         bos[nb++] = c->arena[i].handle;
   for (unsigned i = 0; i < in->n_extra_bos; i++)
      bos[nb++] = in->extra_bos[i];
   {
      struct drm_prismrv_submit_vendor s;
      struct prismrv_vk_submit_args sa = {
         .ccb_handle = c->arena[PRISMRV_VK_ARENA_CCB].handle, .entry_offset = slot * PRISMRV_VK_ENTRY_BYTES,
         .hwrc_va = in->hwrc_va, .first_kick = c->first_kick, .last_in_scene = true, .bo_handles = bos, .num_bos = nb,
      };
      int ret;

      if (!prismrv_vk_fill_submit(&s, &sa))
         FAIL(-EINVAL, "submit arguments not representable");
      ret = c->ops.submit(c->ops.user, &s);
      if (ret)
         FAIL(ret, "vendor submit failed");
   }
   c->first_kick = false;
   c->ring_next++;

   res->slot = slot;
   res->code_va = code_va;
   res->binding_va = bind_va;
   res->bg_va = gl.pds_bg_va;
   res->bg2_va = gl.pds_bg2_va;
   res->arena2_va = gl.pds_arena2_va;
   res->stream_va = stream_va;
   res->stream_dwords = n;
   res->unwired = (in->bg_code_va ? 0 : PRISMRV_VK_UNWIRED_BACKGROUND_PROGRAM) | (hw_written ? 0 : PRISMRV_VK_UNWIRED_HW_OBJECT_CONTENTS) | PRISMRV_VK_UNWIRED_HWRC |
                 (extrap_b ? PRISMRV_VK_UNWIRED_EXTRAPOLATED_LIST_B : 0);
   return 0;
#undef FAIL
}
