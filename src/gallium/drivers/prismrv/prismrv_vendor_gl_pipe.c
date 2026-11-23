/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 */
#include "prismrv_vendor_gl_pipe.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xf86drm.h>

#include "util/format/u_format.h"
#include "compiler/shader_enums.h"
#include "util/bitscan.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"

#include "prismrv_context.h"
#include "prismrv_device.h"
#include "prismrv_resource.h"
#include "prismrv_drmif.h"
#include "prismrv_usse_nir.h"
#include "prismrv_vendor_gl.h"

/* ---- ops over the DRM device -------------------------------------------------------------------------------------- */

static uint32_t
op_bo_create(void *user, uint64_t size, uint32_t heap, uint32_t *gpu_va)
{
   return prismrv_drm_gem_create_heap((int)(intptr_t)user, size, heap, gpu_va);
}

static void *
op_bo_map(void *user, uint32_t handle, uint64_t size)
{
   void *m = prismrv_drm_gem_map((int)(intptr_t)user, handle, size);

   return m == (void *)-1 ? NULL : m;
}

static uint32_t
op_hwrc_create(void *user, uint32_t size, uint32_t pd_offset, uint32_t *gpu_va)
{
   return prismrv_drm_hwrc_create((int)(intptr_t)user, size, pd_offset, gpu_va);
}

static int
op_submit(void *user, const struct drm_prismrv_submit_vendor *a)
{
   struct drm_prismrv_submit_vendor copy = *a;

   return drmIoctl((int)(intptr_t)user, DRM_IOCTL_PRISMRV_SUBMIT_VENDOR, &copy);
}

struct prismrv_vk_gl *
prismrv_vk_gl_pipe_create(int fd)
{
   const char *e = getenv("PRISMRV_VENDOR_HWRC"), *why = "";
   struct prismrv_vk_gl *g;
   struct prismrv_vk_ops ops = { .user = (void *)(intptr_t)fd, .bo_create = op_bo_create, .bo_map = op_bo_map, .submit = op_submit, .hwrc_create = op_hwrc_create };
   int r;

   g = calloc(1, sizeof(*g));
   if (!g)
      return NULL;
   /* PRISMRV_VENDOR_HWRC=<hex> overrides (a context the caller built); otherwise the kernel creates it like the vendor KM */
   r = prismrv_vk_gl_init(g, &ops, e ? (uint32_t)strtoul(e, NULL, 16) : 0, &why);
   if (r) {
      fprintf(stderr, "prismrv: vendor GL path not available: %s (%d)\n", why, r);
      free(g);
      return NULL;
   }
   fprintf(stderr, "prismrv: PRISMRV_VENDOR_GL: draws go to the stock-uKernel vendor path only; unsupported draws are dropped\n");
   return g;
}

void
prismrv_vk_gl_pipe_destroy(struct prismrv_vk_gl *g)
{
   free(g);   /* BOs are released with the DRM file */
}

/* ---- state flattening ---------------------------------------------------------------------------------------------- */

static bool
is_float_format(enum pipe_format f, unsigned *ncomp)
{
   switch (f) {
   case PIPE_FORMAT_R32_FLOAT: *ncomp = 1; return true;
   case PIPE_FORMAT_R32G32_FLOAT: *ncomp = 2; return true;
   case PIPE_FORMAT_R32G32B32_FLOAT: *ncomp = 3; return true;
   case PIPE_FORMAT_R32G32B32A32_FLOAT: *ncomp = 4; return true;
   default: return false;
   }
}

static bool
resource_va(struct pipe_resource *pres, unsigned offset, uint32_t *va, uint32_t *handle)
{
   struct prismrv_resource *r = (struct prismrv_resource *)pres;

   if (!r || !r->gpu_va || !r->gem_handle)
      return false;
   *va = r->gpu_va + offset;
   *handle = r->gem_handle;
   return true;
}

/* ---- user-pointer data: copy into a temporary BO in the default (general) heap, like the vendor UMD's own vertex data ---- */

#define MAX_UPLOAD (16u << 20)

void
prismrv_vk_gl_pipe_release_holds(struct prismrv_context *ctx, unsigned keep)
{
   while (ctx->n_vendor_hold > keep)
      pipe_resource_reference(&ctx->vendor_hold[--ctx->n_vendor_hold], NULL);
}

static bool
upload_user(struct prismrv_context *ctx, const void *src, size_t size, uint32_t *va, uint32_t *handle)
{
   struct pipe_screen *ps = ctx->base.screen;
   struct pipe_resource t;
   struct pipe_resource *r;
   void *p;

   if (!src || !size || size > MAX_UPLOAD || ctx->n_vendor_hold >= ARRAY_SIZE(ctx->vendor_hold))
      return false;
   memset(&t, 0, sizeof(t));
   t.target = PIPE_BUFFER;
   t.format = PIPE_FORMAT_R8_UNORM;
   t.bind = PIPE_BIND_VERTEX_BUFFER | PIPE_BIND_INDEX_BUFFER;
   t.width0 = size;
   t.height0 = t.depth0 = t.array_size = 1;
   r = ps->resource_create(ps, &t);
   if (!r)
      return false;
   p = prismrv_resource_map(r);
   if (!p || !resource_va(r, 0, va, handle)) {
      pipe_resource_reference(&r, NULL);
      return false;
   }
   memcpy(p, src, size);
   ctx->vendor_hold[ctx->n_vendor_hold++] = r;
   return true;
}

static uint32_t
read_index(const void *idx, unsigned size, unsigned i)
{
   return size == 4 ? ((const uint32_t *)idx)[i] : size == 2 ? ((const uint16_t *)idx)[i] : ((const uint8_t *)idx)[i];
}

bool
prismrv_vk_gl_pipe_draw(struct prismrv_context *ctx, const struct pipe_draw_info *info,
                        const struct pipe_draw_start_count_bias *draw)
{
   struct prismrv_vk_gl *g = ctx->vk_gl;
   const struct pipe_framebuffer_state *fb = &ctx->framebuffer;
   struct prismrv_vk_gl_draw d;
   const char *why = "";
   unsigned i;

   if (!g)
      return ctx->vendor_only;   /* vendor ABI without a usable vendor context (e.g. no HWRC): swallow, never emit STREAM_V1 */
   memset(&d, 0, sizeof(d));
   if (fb->nr_cbufs < 1 || !fb->cbufs[0].texture) { fprintf(stderr, "prismrv: vendor GL: draw dropped: no colour buffer\n"); return true; }
   d.fb_w = fb->width;
   d.fb_h = fb->height;
   d.rt_rgba8 = fb->cbufs[0].format == PIPE_FORMAT_R8G8B8A8_UNORM && fb->cbufs[0].level == 0;
   if (!resource_va(fb->cbufs[0].texture, 0, &d.rt_va, &d.rt_handle)) { fprintf(stderr, "prismrv: vendor GL: draw dropped: colour buffer has no GPU address\n"); return true; }
   /* depth: only a Z16 surface is wired (the one measured format, analysis/CMDTA_REGISTER_BLOCKS.md dw18); depth testing without a
    * depth surface passes in GL, so it is simply off.  Any other surface (stencil, Z24, Z32) is refused by record_draw. */
   if (fb->zsbuf.texture) {
      if (fb->zsbuf.format == PIPE_FORMAT_Z16_UNORM && fb->zsbuf.level == 0 &&
          resource_va(fb->zsbuf.texture, 0, &d.zs_va, &d.zs_handle))
         d.has_depth = true;
      else
         d.has_zs = true;
   }
   d.depth_test = d.has_depth && ctx->depth.depth_enabled;
   d.depth_write = d.depth_test && ctx->depth.depth_writemask;
   d.depth_func = ctx->depth.depth_func;            /* PIPE_FUNC_* and enum prismrv_vk_func share the 3-bit order */
   d.stencil_test = ctx->depth.stencil_enabled;
   d.alpha_test = ctx->depth.alpha_enabled;
   d.blend = ctx->blend.blend_enable;
   for (i = 0; i < 4; i++) d.color_mask[i] = (ctx->blend.colormask >> i) & 1;   /* PIPE_MASK_R/G/B/A */
   /* culling: measured with the default (counter-clockwise) front face only */
   d.cull = ctx->raster.cull_face == PIPE_FACE_NONE ? PRISMRV_VK_CULL_NONE :
            ctx->raster.cull_face == PIPE_FACE_FRONT ? PRISMRV_VK_CULL_FRONT :
            ctx->raster.cull_face == PIPE_FACE_BACK ? PRISMRV_VK_CULL_BACK : PRISMRV_VK_CULL_NONE;
   if (ctx->raster.cull_face == PIPE_FACE_FRONT_AND_BACK || (ctx->raster.cull_face != PIPE_FACE_NONE && !ctx->raster.front_ccw))
      d.unmapped_state = true;
   if (ctx->raster.scissor_enable) {
      const struct pipe_scissor_state *sc = &ctx->scissors[0];

      d.scissor_partial = sc->minx > 0 || sc->miny > 0 || sc->maxx < fb->width || sc->maxy < fb->height;
   }
   /* viewport rectangle (GL window coordinates) and depth range from the gallium transform x_w = scale * x_ndc + translate */
   if (ctx->viewport_valid) {
      const float *sc = ctx->viewport.scale, *tr = ctx->viewport.translate;
      const float hx = fabsf(sc[0]), hy = fabsf(sc[1]);
      const float fx = tr[0] - hx;
      const float fy = sc[1] >= 0 ? tr[1] - hy : (float)fb->height - (tr[1] + hy);   /* an inverted-y transform flips the rows */
      const float near = tr[2] - sc[2], far = tr[2] + sc[2];

      d.vx = (int)floorf(fx + 0.5f); d.vy = (int)floorf(fy + 0.5f);
      d.vw = (int)floorf(hx * 2 + 0.5f); d.vh = (int)floorf(hy * 2 + 0.5f);
      d.depth_near = near; d.depth_far = far;
      if (fabsf(fx - d.vx) > 0.01f || fabsf(fy - d.vy) > 0.01f || fabsf(hx * 2 - d.vw) > 0.01f || fabsf(hy * 2 - d.vh) > 0.01f ||
          d.vx < 0 || d.vy < 0 || d.vx + d.vw > (int)fb->width || d.vy + d.vh > (int)fb->height ||
          near < -1e-4f || far > 1.0001f || near > far)
         d.unmapped_state = true;
   } else {
      d.vx = d.vy = 0; d.vw = fb->width; d.vh = fb->height;
      d.depth_near = 0.0; d.depth_far = 1.0;
   }
   for (i = 0; i < 8; i++) d.textures |= ctx->textures[i] != NULL;
   d.vs_uniforms = ctx->vs && ctx->vs->native && (ctx->vs->native->sa_uniform_count || ctx->vs->native->sa_pool_count);
   d.fs_uniforms = ctx->fs && ctx->fs->native && (ctx->fs->native->sa_uniform_count || ctx->fs->native->sa_pool_count);
   d.triangles = info->mode == MESA_PRIM_TRIANGLES;
   d.lines = info->mode == MESA_PRIM_LINES;
   d.vp_scale[0] = ctx->viewport_valid ? ctx->viewport.scale[0] : fb->width * 0.5f;
   d.vp_scale[1] = ctx->viewport_valid ? ctx->viewport.scale[1] : fb->height * 0.5f;
   if (ctx->vs && ctx->vs->native) { d.vs_code = ctx->vs->native->code; d.vs_words = ctx->vs->native->num_words; }
   if (ctx->fs && ctx->fs->native) { d.fs_code = ctx->fs->native->code; d.fs_words = ctx->fs->native->num_words; }

   {
      /* interface slots that are not varyings: position, point size, face, point coord, primitive id, layer, viewport */
      const uint64_t not_varying = BITFIELD64_BIT(VARYING_SLOT_POS) | BITFIELD64_BIT(VARYING_SLOT_PSIZ) |
                                   BITFIELD64_BIT(VARYING_SLOT_FACE) | BITFIELD64_BIT(VARYING_SLOT_PNTC) |
                                   BITFIELD64_BIT(VARYING_SLOT_PRIMITIVE_ID) | BITFIELD64_BIT(VARYING_SLOT_LAYER) |
                                   BITFIELD64_BIT(VARYING_SLOT_VIEWPORT);

      d.vs_varyings = ctx->vs ? util_bitcount64(ctx->vs->outputs_written & ~not_varying) : 0;
      d.fs_varyings = ctx->fs ? util_bitcount64(ctx->fs->inputs_read & ~not_varying) : 0;
   }
   d.n_attrs = ctx->num_vertex_elements;
   d.first = draw->start;
   d.count = draw->count;
   {
      const unsigned holds_before = ctx->n_vendor_hold;
      const void *indices = NULL;
      uint32_t max_index = 0;
      bool ok = true;

      if (d.count > 0xffff) { why = "more than 65535 vertices/indices"; goto refuse; }
      if (info->index_size) {
         unsigned isz = info->index_size;

         d.indexed = true;
         if (draw->index_bias) { why = "index_bias is not wired"; goto refuse; }
         if (isz != 1 && isz != 2 && isz != 4) { why = "index size"; goto refuse; }
         indices = info->has_user_indices ? info->index.user :
                   (info->index.resource ? prismrv_resource_map(info->index.resource) : NULL);
         if (!indices) { why = "no index data"; goto refuse; }
         for (i = 0; i < d.count; i++) {
            uint32_t v = read_index(indices, isz, draw->start + i);

            if (v > 0xffff) { why = "index above 65535 (the vendor index format is u16)"; goto refuse; }
            max_index = MAX2(max_index, v);
         }
         if (isz == 2 && !info->has_user_indices) {
            if (!resource_va(info->index.resource, draw->start * 2, &d.index_va, &d.index_handle))
               d.index_va = 0;
         } else {
            /* user indices, or u8: copy as u16, starting at the first index of the draw */
            uint16_t *ix = malloc((size_t)d.count * 2);

            if (!ix) { why = "out of memory"; goto refuse; }
            for (i = 0; i < d.count; i++)
               ix[i] = read_index(indices, isz, draw->start + i);
            ok = upload_user(ctx, ix, (size_t)d.count * 2, &d.index_va, &d.index_handle);
            free(ix);
            if (!ok) { why = "index upload failed"; goto refuse; }
         }
         d.first = 0;
      } else {
         max_index = draw->start + (d.count ? d.count - 1 : 0);
      }

      for (i = 0; i < d.n_attrs && i < PRISMRV_VK_GL_MAX_ATTRS; i++) {
         const struct prismrv_vertex_element *e = &ctx->vertex_elements[i];
         const struct pipe_vertex_buffer *vb = &ctx->vertex_buffers[e->vertex_buffer_index];

         d.attr[i].is_float = is_float_format(e->src_format, &d.attr[i].ncomp);
         d.attr[i].stride = e->src_stride;
         if (!vb->is_user_buffer) {
            if (!resource_va(vb->buffer.resource, vb->buffer_offset + e->src_offset, &d.attr[i].va, &d.attr[i].handle))
               d.attr[i].va = 0;
         } else if (d.attr[i].is_float && d.attr[i].stride) {
            /* user-pointer array: copy elements 0..max_index (the array starts at the element's first byte) */
            const uint8_t *src = (const uint8_t *)ctx->user_vertex_buffers[e->vertex_buffer_index];
            size_t bytes = (size_t)max_index * d.attr[i].stride + d.attr[i].ncomp * 4u;

            if (!src || !upload_user(ctx, src + vb->buffer_offset + e->src_offset, bytes, &d.attr[i].va, &d.attr[i].handle))
               d.attr[i].va = 0;
         }
      }
      if (prismrv_vk_gl_record_draw(g, &d, &why)) {
refuse:
         prismrv_vk_gl_pipe_release_holds(ctx, holds_before);   /* nothing was recorded: free this draw's temporaries */
         fprintf(stderr, "prismrv: vendor GL: draw dropped: %s\n", why);
      }
   }
   return true;
}

bool
prismrv_vk_gl_pipe_clear(struct prismrv_context *ctx, unsigned buffers, const union pipe_color_union *color)
{
   struct prismrv_vk_gl *g = ctx->vk_gl;

   if (!g)
      return ctx->vendor_only;   /* vendor ABI without a usable vendor context (e.g. no HWRC): swallow, never emit STREAM_V1 */
   if ((buffers & PIPE_CLEAR_COLOR) && color)
      prismrv_vk_gl_record_clear(g, color->f[0], color->f[1], color->f[2], color->f[3]);
   if (buffers & PIPE_CLEAR_DEPTHSTENCIL)
      fprintf(stderr, "prismrv: vendor GL: depth/stencil clear ignored (not wired)\n");
   return true;
}

bool
prismrv_vk_gl_pipe_flush(struct prismrv_context *ctx)
{
   struct prismrv_vk_gl *g = ctx->vk_gl;
   const char *why = "";
   int r;

   if (!g)
      return ctx->vendor_only;   /* vendor ABI without a usable vendor context (e.g. no HWRC): swallow, never emit STREAM_V1 */
   r = prismrv_vk_gl_flush(g, &why);
   prismrv_vk_gl_pipe_release_holds(ctx, 0);   /* the submit holds its own BO references */
   if (r)
      fprintf(stderr, "prismrv: vendor GL: frame failed: %s (%d)\n", why, r);
   return true;
}
