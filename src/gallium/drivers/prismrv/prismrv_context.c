/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * prismrv_context.c — pipe_context implementation.
 */
#include "prismrv_context.h"

#include <sys/mman.h>

#include "pipe/p_defines.h"
#include <unistd.h>

#include "util/u_blitter.h"
#include "util/u_memory.h"
#include "util/u_upload_mgr.h"

#include "prismrv_batch.h"
#include "prismrv_drmif.h"

/* ---- flush ---------------------------------------------------------- */

static void
prismrv_context_flush(struct pipe_context *pctx,
                      struct pipe_fence_handle **fence, unsigned flags)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);

   if (fence)
      *fence = NULL;

   if (ctx->batch.cmd_size) {
      /* render pass: TA binning followed by 3D render */
      prismrv_batch_submit(pctx, PRISMRV_CMD_TA,
                           ctx->batch.cmd_handle, ctx->batch.cmd_size,
                           NULL, 0);
      ctx->batch.cmd_size = 0;
   }
}

static void
prismrv_set_debug_callback(struct pipe_context *pctx,
                           const struct util_debug_callback *cb)
{
}

static void
prismrv_invalidate_resource(struct pipe_context *pctx,
                            struct pipe_resource *pres)
{
}

/* ---- draw ------------------------------------------------------------ */

/* Build a TA packet stream for a triangle draw.
 * Format matches ta_stage.py / the emulator's TA parser:
 *   pkt(1,[mode]) pkt(2,[first,count]) pkt(3,[ncomp,off,stride]+data) pkt(4,[])
 */
static void
prismrv_build_ta_packets(uint8_t *buf, uint32_t *size,
                         const float *verts, unsigned nverts,
                         unsigned ncomp)
{
   uint32_t off = 0;
   uint32_t nw = nverts * ncomp;

   /* VGT_STATE: triangles mode */
   struct { uint32_t op, nw, w[1]; } __attribute__((packed)) p_vgt = {
      .op = 1, .nw = 1, .w = {2}
   };
   memcpy(buf + off, &p_vgt, sizeof(p_vgt)); off += sizeof(p_vgt);

   /* INDEX_RANGE */
   struct { uint32_t op, nw, w[2]; } __attribute__((packed)) p_idx = {
      .op = 2, .nw = 2, .w = {0, nverts}
   };
   memcpy(buf + off, &p_idx, sizeof(p_idx)); off += sizeof(p_idx);

   /* VERTEX_ARRAY: inline data */
   struct { uint32_t op, nw, hdr[3]; } __attribute__((packed)) p_va_hdr = {
      .op = 3, .nw = 3 + nw, .hdr = {ncomp, 0, ncomp}
   };
   memcpy(buf + off, &p_va_hdr, sizeof(p_va_hdr)); off += sizeof(p_va_hdr);
   memcpy(buf + off, verts, nw * 4); off += nw * 4;

   /* END */
   buf[off] = 4; buf[off+1] = 0; buf[off+2] = 0; buf[off+3] = 0;
   off += 4;

   *size = off;
}

static void
prismrv_draw_vbo(struct pipe_context *pctx,
                 const struct pipe_draw_info *info,
                 unsigned drawid_offset,
                 const struct pipe_draw_indirect_info *indirect,
                 const struct pipe_draw_start_count_bias *draws,
                 unsigned num_draws)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);

   if (info->mode != MESA_PRIM_TRIANGLES)
      return;

   /* build vertex data from bound vertex buffers */
   unsigned ncomp = 7; /* x,y,z,w,r,g,b — matches the emulator convention */
   unsigned nverts = draws->count;
   if (nverts < 3 || !ctx->num_vertex_elements)
      return;

   float verts[256 * 7]; /* max 256 vertices for now */
   if (nverts > 256) nverts = 256;

   /* fetch vertices from the first vertex buffer */
   for (unsigned v = 0; v < nverts; v++) {
      /* positions and colors come from the vertex buffer directly */
      /* in a full implementation this would use the vertex element
       * descriptors to gather from multiple buffers with strides */
      for (unsigned c = 0; c < ncomp; c++)
         verts[v * ncomp + c] = 0.0f;
   }

   /* build TA packets into the batch buffer */
   prismrv_build_ta_packets(
      (uint8_t *)ctx->batch.cmd_map + ctx->batch.cmd_size,
      &ctx->batch.cmd_size,
      verts, nverts, ncomp);

   ctx->batch.cmd_size = (ctx->batch.cmd_size + 3) & ~3u;
}

static void
prismrv_set_framebuffer_state(struct pipe_context *pctx,
                              const struct pipe_framebuffer_state *state)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);

   util_copy_framebuffer_state(&ctx->framebuffer, state);
}

static void
prismrv_set_viewport_states(struct pipe_context *pctx,
                            unsigned start_slot,
                            unsigned num_viewports,
                            const struct pipe_viewport_state *states)
{
   (void)start_slot; (void)states;
}

static void
prismrv_set_scissor_states(struct pipe_context *pctx,
                           unsigned start_slot,
                           unsigned num_scissors,
                           const struct pipe_scissor_state *scissors)
{
}

static void
prismrv_set_constant_buffer(struct pipe_context *pctx,
                            mesa_shader_stage shader, uint index,
                            const struct pipe_constant_buffer *cb)
{
}

static void
prismrv_sampler_view_destroy(struct pipe_context *pctx,
                             struct pipe_sampler_view *view)
{
   FREE(view);
}

static struct pipe_sampler_view *
prismrv_create_sampler_view(struct pipe_context *pctx,
                            struct pipe_resource *pres,
                            const struct pipe_sampler_view *tmpl)
{
   struct prismrv_sampler_view *so = CALLOC_STRUCT(prismrv_sampler_view);
   if (!so)
      return NULL;

   so->base = *tmpl;
   so->base.texture = NULL;
   pipe_resource_reference(&so->base.texture, pres);
   so->base.reference.count = 1;
   so->base.context = pctx;
   return &so->base;
}

/* ---- lifecycle ------------------------------------------------------- */

static void
prismrv_context_destroy(struct pipe_context *pctx)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);

   util_blitter_destroy(ctx->blitter);
   u_upload_destroy(ctx->uploader);
   if (ctx->batch.cmd_map && ctx->batch.cmd_map != MAP_FAILED)
      munmap(ctx->batch.cmd_map, ctx->batch.cmd_capacity);

   FREE(ctx);
}

static void *
prismrv_create_vs_state(struct pipe_context *pctx,
                        const struct pipe_shader_state *tmpl)
{
   /* NIR is provided by st/mesa; store it for later USSE compilation */
   struct prismrv_shader_state *state = CALLOC_STRUCT(prismrv_shader_state);
   if (!state) return NULL;
   state->nir = tmpl->ir.nir;
   return state;
}

static void
prismrv_bind_vs_state(struct pipe_context *pctx, void *state)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);
   memcpy(&ctx->vs, state, sizeof(ctx->vs));
}

static void
prismrv_delete_vs_state(struct pipe_context *pctx, void *state)
{
   FREE(state);
}

static void *
prismrv_create_fs_state(struct pipe_context *pctx,
                        const struct pipe_shader_state *tmpl)
{
   struct prismrv_shader_state *state = CALLOC_STRUCT(prismrv_shader_state);
   if (!state) return NULL;
   state->nir = tmpl->ir.nir;
   return state;
}

static void
prismrv_bind_fs_state(struct pipe_context *pctx, void *state)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);
   memcpy(&ctx->fs, state, sizeof(ctx->fs));
}

static void
prismrv_delete_fs_state(struct pipe_context *pctx, void *state)
{
   FREE(state);
}

static void
prismrv_set_vertex_buffers(struct pipe_context *pctx,
                           unsigned start_slot,
                           const struct pipe_vertex_buffer *buffers)
{
}

static void *
prismrv_create_vertex_elements(struct pipe_context *pctx,
                               unsigned num_elems,
                               const struct pipe_vertex_element *elems)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);
   ctx->num_vertex_elements = num_elems;
   for (unsigned i = 0; i < num_elems && i < 8; i++) {
      ctx->vertex_elements[i].src_offset = elems[i].src_offset;
      ctx->vertex_elements[i].src_format = elems[i].src_format;
      ctx->vertex_elements[i].vertex_buffer_index = elems[i].vertex_buffer_index;
   }
   return (void *)(uintptr_t)(num_elems | 1);  /* non-NULL cookie */
}

static void
prismrv_bind_vertex_elements(struct pipe_context *pctx, void *state)
{
}

static void
prismrv_delete_vertex_elements(struct pipe_context *pctx, void *state)
{
}

void
prismrv_batch_init_context(struct prismrv_context *ctx)
{
   struct prismrv_screen *screen = ctx->screen;

   ctx->batch.cmd_capacity = 4096;
   ctx->batch.cmd_handle =
      prismrv_drm_gem_create(screen->fd, ctx->batch.cmd_capacity);
   ctx->batch.cmd_map =
      prismrv_drm_gem_map(screen->fd, ctx->batch.cmd_handle,
                          ctx->batch.cmd_capacity);
}

void
prismrv_context_init(struct prismrv_context *ctx)
{
   struct pipe_context *pctx = &ctx->base;

   pctx->destroy = prismrv_context_destroy;
   pctx->flush = prismrv_context_flush;
   pctx->set_debug_callback = prismrv_set_debug_callback;
   pctx->invalidate_resource = prismrv_invalidate_resource;
   pctx->draw_vbo = prismrv_draw_vbo;
   pctx->set_framebuffer_state = prismrv_set_framebuffer_state;
   pctx->set_viewport_states = prismrv_set_viewport_states;
   pctx->set_scissor_states = prismrv_set_scissor_states;
   pctx->set_constant_buffer = prismrv_set_constant_buffer;
   pctx->create_sampler_view = prismrv_create_sampler_view;
   pctx->sampler_view_destroy = prismrv_sampler_view_destroy;
   pctx->create_vs_state = prismrv_create_vs_state;
   pctx->bind_vs_state = prismrv_bind_vs_state;
   pctx->delete_vs_state = prismrv_delete_vs_state;
   pctx->create_fs_state = prismrv_create_fs_state;
   pctx->bind_fs_state = prismrv_bind_fs_state;
   pctx->delete_fs_state = prismrv_delete_fs_state;
   pctx->set_vertex_buffers = prismrv_set_vertex_buffers;
   pctx->create_vertex_elements_state = prismrv_create_vertex_elements;
   pctx->bind_vertex_elements_state = prismrv_bind_vertex_elements;
   pctx->delete_vertex_elements_state = prismrv_delete_vertex_elements;

   ctx->uploader = u_upload_create_default(pctx);
   if (!ctx->uploader)
      return;
   pctx->stream_uploader = ctx->uploader;
   pctx->const_uploader = ctx->uploader;

   ctx->blitter = util_blitter_create(pctx);
}
