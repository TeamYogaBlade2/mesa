/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * prismrv_context.c — pipe_context implementation.
 *
 * Minimal but functional context: uploader, blitter and flush wired to
 * the command stream builder.  Draw-state translation lands here as the
 * driver grows.
 */
#include "prismrv_context.h"

#include <unistd.h>

#include "util/u_blitter.h"
#include <sys/mman.h>

#include "util/u_blitter.h"
#include "util/u_memory.h"
#include "util/u_upload_mgr.h"
#include "util/u_upload_mgr.h"

#include "prismrv_batch.h"
#include "prismrv_drmif.h"

static void
prismrv_context_flush(struct pipe_context *pctx, struct pipe_fence_handle **fence,
                      unsigned flags)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);

   if (fence)
      *fence = NULL;

   /* submit the accumulated command stream (TA service) */
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
   /* no async debug messages yet */
}

static void
prismrv_context_destroy(struct pipe_context *pctx)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);

   if (ctx->blitter)
      util_blitter_destroy(ctx->blitter);
   if (ctx->uploader)
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

   ctx->uploader = u_upload_create_default(pctx);
   if (!ctx->uploader)
      return;
   pctx->stream_uploader = ctx->uploader;
   pctx->const_uploader = ctx->uploader;

   ctx->blitter = util_blitter_create(pctx);
}
