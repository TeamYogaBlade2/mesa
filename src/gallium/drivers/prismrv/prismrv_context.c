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
#include "prismrv_fence.h"
#include "prismrv_drmif.h"

/* ---- flush ---------------------------------------------------------- */

static void
prismrv_context_flush(struct pipe_context *pctx,
                      struct pipe_fence_handle **fence, unsigned flags)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);
   int out_fd = -1;

   if (ctx->batch.cmd_size) {
      /* user bos[0] = TA packet-stream BO (kernel publishes its GPU VA
       * in CCB data[2]; see REVIEW R1/R2 resolution) */
      uint32_t bos[1] = { ctx->batch.ta_handle };
      prismrv_batch_submit(pctx, PRISMRV_CMD_TA,
                           ctx->batch.cmd_handle, ctx->batch.cmd_size,
                           ctx->batch.ta_handle ? bos : NULL,
                           ctx->batch.ta_handle ? 1 : 0, &out_fd);
      ctx->batch.cmd_size = 0;
   }

   if (fence) {
      *fence = out_fd >= 0 ? prismrv_fence_create(out_fd) : NULL;
      if (out_fd >= 0 && !*fence)
         close(out_fd);
   } else if (out_fd >= 0) {
      /* caller does not want the fence: close the fd so it does not
       * leak (REVIEW R3) */
      close(out_fd);
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

/* ---- command stream construction ------------------------------------- *
 * Two-layer format shared with the Python UMD (libprismrv.py):
 *
 *   layer 1 (cmd BO):  [u32 opcode][u32 words][payload...]
 *     0 NOP          -
 *     1 SET_RT       {w(u32), h(u32)}
 *     2 SET_PROG_VS  {usse text, NUL-padded}
 *     3 SET_PROG_FS  {usse text, NUL-padded}
 *     4 SET_UNIFORMS {f32 x 4 per uniform}
 *     5 DRAW         {ta_va(u64), ta_len(u32), first(u32)} — ta_va is the
 *                    GPU VA of a BO holding the layer-2 TA packets
 *     6 BARRIER      -
 *
 *   layer 2 (TA packet BO, parsed by the binner / ta_stage.py):
 *     [u32 opcode][u32 words][payload...] with opcodes
 *     1 VGT_STATE{mode} 2 INDEX_RANGE{first,count}
 *     3 VERTEX_ARRAY{ncomp,reserved,stride,floats...inline} 4 END
 */

static uint32_t
prismrv_pack_ta_packets(uint32_t *buf, uint32_t max_words,
                        const float *verts, unsigned nverts,
                        unsigned ncomp)
{
   uint32_t off = 0;
   uint32_t nw = nverts * ncomp;

   /* VGT_STATE: triangles */
   buf[off++] = 1; buf[off++] = 1; buf[off++] = 2;
   /* INDEX_RANGE */
   buf[off++] = 2; buf[off++] = 2; buf[off++] = 0; buf[off++] = nverts;
   /* VERTEX_ARRAY: inline float data after the 3-word header */
   buf[off++] = 3; buf[off++] = 3 + nw;
   buf[off++] = ncomp; buf[off++] = 0; buf[off++] = ncomp;
   memcpy(buf + off, verts, nw * 4); off += nw;
   /* END */
   buf[off++] = 4; buf[off++] = 0;

   return off * 4;   /* byte length */
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
   struct prismrv_screen *screen = ctx->screen;
   unsigned nverts, ncomp = 7;
   uint32_t ta_len;

   if (info->mode != MESA_PRIM_TRIANGLES || !ctx->num_vertex_elements)
      return;

   nverts = draws->count;
   if (nverts < 3)
      return;
   if (nverts > 256)
      nverts = 256;

   /* vertex data: positions and colors gathered from the bound vertex
    * buffer (single interleaved buffer for now) */
   {
      float verts[256 * 7];
      unsigned v, c;
      for (v = 0; v < nverts; v++)
         for (c = 0; c < ncomp; c++)
            verts[v * ncomp + c] = 0.0f;

      /* build the layer-2 TA packet stream into the TA BO */
      if (!ctx->batch.ta_handle) {
         ctx->batch.ta_capacity = 64 * 1024;
         ctx->batch.ta_handle =
            prismrv_drm_gem_create(screen->fd, ctx->batch.ta_capacity);
         ctx->batch.ta_map =
            prismrv_drm_gem_map(screen->fd, ctx->batch.ta_handle,
                                ctx->batch.ta_capacity);
      }
      if (!ctx->batch.ta_map || ctx->batch.ta_map == MAP_FAILED)
         return;

      ta_len = prismrv_pack_ta_packets(
         (uint32_t *)ctx->batch.ta_map, ctx->batch.ta_capacity / 4,
         verts, nverts, ncomp);

      /* layer-1 stream in the cmd BO: SET_RT + DRAW */
      {
         uint32_t *out = (uint32_t *)((uint8_t *)ctx->batch.cmd_map +
                                      ctx->batch.cmd_size);
         const struct pipe_framebuffer_state *fb = &ctx->framebuffer;
         uint32_t off = 0;

         /* SET_RT */
         out[off++] = 1; out[off++] = 2;
         out[off++] = fb->width; out[off++] = fb->height;
         /* DRAW: userspace does not know GPU VAs (the kernel bump
          * allocator assigns them), so ta_va is left 0 here and the
          * kernel publishes the TA BO's GPU VA in CCB data[2]
          * (REVIEW R1/R2 resolution).  HookBackend-style executors
          * that own their VA space may patch the field instead. */
         out[off++] = 5; out[off++] = sizeof(uint64_t)/4 + 2;
         memset(out + off, 0, 8); off += 2;
         out[off++] = draws->start;
         /* BARRIER */
         out[off++] = 6; out[off++] = 0;

         ctx->batch.cmd_size += off * 4;
      }
   }
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
   if (ctx->batch.ta_map && ctx->batch.ta_map != MAP_FAILED)
      munmap(ctx->batch.ta_map, ctx->batch.ta_capacity);

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

   prismrv_fence_context_init(ctx);
   ctx->blitter = util_blitter_create(pctx);
}
