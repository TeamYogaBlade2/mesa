/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 */
#include "prismrv_vendor_gl_pipe.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xf86drm.h>

#include "util/format/u_format.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"

#include "prismrv_context.h"
#include "prismrv_device.h"
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
   d.has_zs = fb->zsbuf.texture != NULL || ctx->depth.depth_enabled;
   d.blend = ctx->blend.blend_enable;
   for (i = 0; i < 8; i++) d.textures |= ctx->textures[i] != NULL;
   d.vs_uniforms = ctx->vs && ctx->vs->native && (ctx->vs->native->sa_uniform_count || ctx->vs->native->sa_pool_count);
   d.fs_uniforms = ctx->fs && ctx->fs->native && (ctx->fs->native->sa_uniform_count || ctx->fs->native->sa_pool_count);
   d.triangles = info->mode == MESA_PRIM_TRIANGLES;
   d.vp_scale[0] = ctx->viewport_valid ? ctx->viewport.scale[0] : fb->width * 0.5f;
   d.vp_scale[1] = ctx->viewport_valid ? ctx->viewport.scale[1] : fb->height * 0.5f;
   if (ctx->vs && ctx->vs->native) { d.vs_code = ctx->vs->native->code; d.vs_words = ctx->vs->native->num_words; }
   if (ctx->fs && ctx->fs->native) { d.fs_code = ctx->fs->native->code; d.fs_words = ctx->fs->native->num_words; }

   d.n_attrs = ctx->num_vertex_elements;
   for (i = 0; i < d.n_attrs && i < PRISMRV_VK_GL_MAX_ATTRS; i++) {
      const struct prismrv_vertex_element *e = &ctx->vertex_elements[i];
      const struct pipe_vertex_buffer *vb = &ctx->vertex_buffers[e->vertex_buffer_index];

      d.attr[i].is_float = is_float_format(e->src_format, &d.attr[i].ncomp);
      d.attr[i].stride = e->src_stride;
      if (vb->is_user_buffer || !resource_va(vb->buffer.resource, vb->buffer_offset + e->src_offset, &d.attr[i].va, &d.attr[i].handle))
         d.attr[i].va = 0;   /* user-pointer arrays are not uploaded: refused by the glue */
   }
   d.first = draw->start;
   d.count = draw->count;
   if (info->index_size) {
      d.indexed = true;
      if (info->index_size != 2 || info->has_user_indices || !resource_va(info->index.resource, draw->start * 2, &d.index_va, &d.index_handle))
         d.index_va = 0;
      d.first = 0;
   }
   if (prismrv_vk_gl_record_draw(g, &d, &why))
      fprintf(stderr, "prismrv: vendor GL: draw dropped: %s\n", why);
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
   if (r)
      fprintf(stderr, "prismrv: vendor GL: frame failed: %s (%d)\n", why, r);
   return true;
}
