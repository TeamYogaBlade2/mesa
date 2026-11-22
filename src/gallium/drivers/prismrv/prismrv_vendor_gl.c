/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 */
#include "prismrv_vendor_gl.h"

#include <errno.h>
#include <math.h>
#include <string.h>

static uint32_t
pack_unorm8(float f)
{
   if (!(f > 0.0f)) return 0;
   if (f >= 1.0f) return 255;
   return (uint32_t)(f * 255.0f + 0.5f);
}

int
prismrv_vk_gl_init(struct prismrv_vk_gl *g, const struct prismrv_vk_ops *ops, uint32_t hwrc_va, const char **why)
{
   int r;

   memset(g, 0, sizeof(*g));
   if (!hwrc_va && !ops->hwrc_create) { *why = "no HW render context address (PRISMRV_VENDOR_HWRC) and the kernel cannot create one (no HWRC_CREATE ioctl)"; return -EINVAL; }
   r = prismrv_vk_ctx_init(&g->vk, ops, why);
   if (r) return r;
   r = prismrv_vk_scene_create(&g->vk, &g->scene, why);
   if (r) return r;
   if (!hwrc_va) {
      /* vendor: the KM allocates the 0x54-byte context and patches the PD address into the user image at +0x08 */
      g->hwrc_handle = ops->hwrc_create(ops->user, PRISMRV_VK_HWRC_SIZE, PRISMRV_VK_HWRC_PD_OFFSET, &hwrc_va);
      if (!g->hwrc_handle) { *why = "DRM_PRISMRV_HWRC_CREATE failed (needs CAP_SYS_RAWIO and the kernel's vendor_abi)"; return -EIO; }
   }
   g->hwrc_va = hwrc_va;
   g->ready = true;
   return 0;
}

int
prismrv_vk_gl_record_draw(struct prismrv_vk_gl *g, const struct prismrv_vk_gl_draw *d, const char **why)
{
#define REJECT(m) do { *why = m; g->dropped++; return -ENOTSUP; } while (0)
   if (!g->ready) REJECT("vendor path not initialised");
   if (g->have_draw) REJECT("more than one draw per frame (the vendor frame shape has exactly one)");
   if (!d->triangles) REJECT("only triangle lists are supported");
   if (!d->rt_rgba8 || !d->rt_va) REJECT("render target must be an RGBA8 resource");
   if (d->has_zs) REJECT("depth/stencil is not wired (the depth surface address is a caller resource)");
   if (d->blend) REJECT("blending is not wired");
   if (d->textures) REJECT("textures are not wired");
   if (d->vs_uniforms || d->fs_uniforms) REJECT("shader uniforms are not wired (VS constants go into the access block, FS ones into an unmeasured place)");
   if (!d->vs_code || !d->vs_words || !d->fs_code || !d->fs_words) REJECT("shader has no native USSE binary");
   if (d->n_attrs < 1 || d->n_attrs > PRISMRV_VK_GL_MAX_ATTRS) REJECT("1 or 2 vertex attributes only");
   for (unsigned i = 0; i < d->n_attrs; i++)
      if (!d->attr[i].is_float || d->attr[i].ncomp < 1 || d->attr[i].ncomp > 4 || !d->attr[i].va || !d->attr[i].stride)
         REJECT("vertex attributes must be 1..4 floats in a resource");
   /* the only measured shapes: no varying + one attribute, or one colour varying + two attributes (analysis/VS_FS_BINDING_SPLIT.md).
    * "does the VS write a varying" is derived from the shader interface; any shape that does not fit is refused, not approximated. */
   if (d->vs_varyings != d->fs_varyings) REJECT("the VS varying outputs and the FS varying inputs differ (unmeasured)");
   if (d->vs_varyings > 1) REJECT("more than one varying is not measured");
   if ((d->vs_varyings == 1) != (d->n_attrs == 2)) REJECT("only 'one varying + two attributes' or 'no varying + one attribute' are measured");
   if (d->count < 3) REJECT("empty draw");
   if (d->indexed && !d->index_va) REJECT("index buffer must be a resource");
   if (d->count > 0xffff) REJECT("more than 65535 vertices/indices");
   g->draw = *d;
   g->have_draw = true;
   return 0;
}

void
prismrv_vk_gl_record_clear(struct prismrv_vk_gl *g, float r, float gr, float b, float a)
{
   g->clear_colour = pack_unorm8(r) | pack_unorm8(gr) << 8 | pack_unorm8(b) << 16 | pack_unorm8(a) << 24;
   g->have_clear = true;
}

int
prismrv_vk_gl_flush(struct prismrv_vk_gl *g, const char **why)
{
   struct prismrv_vk_frame_in in;
   struct prismrv_vk_frame_result res;
   const struct prismrv_vk_gl_draw *d = &g->draw;
   unsigned i;
   int r = 0;

   if (!g->have_draw) {
      g->have_clear = false;
      return 0;
   }
   memset(&in, 0, sizeof(in));
   in.gl.width = in.gl.height = 0;
   in.gl.width = d->fb_w;
   in.gl.height = d->fb_h;
   in.gl.vw = (unsigned)(fabsf(d->vp_scale[0]) * 2.0f + 0.5f);
   in.gl.vh = (unsigned)(fabsf(d->vp_scale[1]) * 2.0f + 0.5f);
   in.gl.depth_far = 1.0;
   in.gl.depth_func = PRISMRV_VK_ALWAYS;
   in.gl.depth_test = -1;
   in.gl.color_mask[0] = in.gl.color_mask[1] = in.gl.color_mask[2] = in.gl.color_mask[3] = true;
   in.gl.rt_va = d->rt_va;

   in.code = d->fs_code;
   in.code_size = d->fs_words * 8;
   in.vs_code = d->vs_code;
   in.vs_code_size = d->vs_words * 8;
   /* varying: derived from the shader interface (record_draw only lets through the measured shapes).  UNVERIFIED: the FS record's
    * colour-varying form is the observed correlate of "the VS writes a varying" (it was never seen with a texture-coordinate varying);
    * its dw3 is the placeholder value of the kv10 captures */
   in.varying = d->vs_varyings == 1;
   in.colour_varying = in.varying;
   in.fs_bind_dw3 = 0x2fc0000f;

   in.n_attrs = d->n_attrs;
   in.attr_va = d->attr[0].va;
   in.attr_dwords_m1 = d->attr[0].ncomp - 1;
   in.attr_stride = d->attr[0].stride;
   if (d->n_attrs == 2) {
      in.attr1_va = d->attr[1].va;
      in.attr1_dwords_m1 = d->attr[1].ncomp - 1;
      in.attr1_stride = d->attr[1].stride;
   }

   /* index data: the caller's u16 buffer, or 0,1,2,... generated for a non-indexed draw */
   if (d->indexed) {
      in.index_va = d->index_va;
   } else {
      uint32_t va;
      uint16_t *ix = (uint16_t *)prismrv_vk_arena_alloc(&g->vk.arena[PRISMRV_VK_ARENA_DATA], (d->count * 2 + 15) & ~15u, 16, &va);

      if (!ix) { *why = "data arena full"; r = -ENOSPC; goto out; }
      for (i = 0; i < d->count; i++) ix[i] = d->first + i;
      in.index_va = va;
   }
   in.index_count = d->count;
   in.clear_w = (float)d->fb_w;
   in.clear_h = (float)d->fb_h;
   in.clear_colour = g->have_clear ? g->clear_colour : 0xff000000;
   in.access_auto = true;
   in.hwrc_va = g->hwrc_va;
   /* the epilogue / access words the caller owns carry userspace-driver values: zero */

   prismrv_vk_scene_fill_frame(&g->scene, &in);
   /* scene BOs + the resources this frame reads/writes */
   {
      uint32_t bos[SCENE_BO_COUNT + PRISMRV_VK_GL_MAX_EXTRA_BOS];
      unsigned n = 0;

      for (i = 0; i < SCENE_BO_COUNT; i++) bos[n++] = g->scene.handles[i];
      bos[n++] = d->rt_handle;
      if (g->hwrc_handle) bos[n++] = g->hwrc_handle;
      for (i = 0; i < d->n_attrs && n < SCENE_BO_COUNT + PRISMRV_VK_GL_MAX_EXTRA_BOS; i++) bos[n++] = d->attr[i].handle;
      if (d->indexed && n < SCENE_BO_COUNT + PRISMRV_VK_GL_MAX_EXTRA_BOS) bos[n++] = d->index_handle;
      memcpy(g->extra, bos + SCENE_BO_COUNT, (n - SCENE_BO_COUNT) * 4);
      g->n_extra = n - SCENE_BO_COUNT;
      /* the scene handles lead; extra_bos must hold all of them in one array */
      static uint32_t all[SCENE_BO_COUNT + PRISMRV_VK_GL_MAX_EXTRA_BOS];
      memcpy(all, bos, n * 4);
      in.extra_bos = all;
      in.n_extra_bos = n;
   }

   r = prismrv_vk_ctx_frame(&g->vk, &in, &res, why);
out:
   g->have_draw = g->have_clear = false;
   return r;
}
