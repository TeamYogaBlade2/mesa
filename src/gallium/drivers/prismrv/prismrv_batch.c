/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * prismrv_batch.c — command stream construction and submission.
 */
#include "prismrv_batch.h"
#include "prismrv_context.h"

#include "util/u_memory.h"

#include "prismrv_drmif.h"

void
prismrv_batch_init_context(struct prismrv_context *ctx)
{
}

int
prismrv_batch_submit(struct pipe_context *pctx,
                     enum prismrv_cmd_type type,
                     uint32_t cmd_bo, uint32_t cmd_size,
                     const uint32_t *bos, uint32_t num_bos,
                     int *out_fence_fd)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);
   int fence_fd = -1;
   int ret;

   ret = prismrv_drm_submit(ctx->screen->fd, type, cmd_bo, cmd_size,
                            bos, num_bos, &fence_fd);
   if (out_fence_fd)
      *out_fence_fd = fence_fd;
   return ret;
}
