/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * prismrv_context.c — pipe_context implementation.
 */
#include "prismrv_context.h"

#include <sys/mman.h>
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

static void
prismrv_draw_vbo(struct pipe_context *pctx,
                 const struct pipe_draw_info *info,
                 unsigned drawid_offset,
                 const struct pipe_draw_indirect_info *indirect,
                 const struct pipe_draw_start_count_bias *draws,
                 unsigned num_draws)
{
   /* command stream construction from draw state will be implemented
    * with the NIR→USSE backend integration; for now the flush path
    * submits whatever batch content exists. */
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
                            enum pipe_shader_type shader, uint index,
                            bool take_ownership,
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

   ctx->uploader = u_upload_create_default(pctx);
   if (!ctx->uploader)
      return;
   pctx->stream_uploader = ctx->uploader;
   pctx->const_uploader = ctx->uploader;

   ctx->blitter = util_blitter_create(pctx);
}
