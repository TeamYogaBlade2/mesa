/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * prismrv_batch.c — command stream construction and submission.
 *
 * Builds the same packet stream the kernel CCB carries (SET_RT /
 * SET_PROG_FS / DRAW), mirroring the layout used by the emulator and
 * the UMD prototype in the PrismRV tools.
 */
#include "prismrv_batch.h"
#include "prismrv_context.h"

#include "util/u_memory.h"

#include "prismrv_drmif.h"

void
prismrv_batch_init_context(struct prismrv_context *ctx)
{
   /* per-context batch state will live here */
}

int
prismrv_batch_submit(struct pipe_context *pctx,
                     enum prismrv_cmd_type type,
                     uint32_t cmd_bo, uint32_t cmd_size,
                     const uint32_t *bos, uint32_t num_bos)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);
   int fence_fd = -1;

   return prismrv_drm_submit(ctx->screen->fd, type, cmd_bo, cmd_size,
                             bos, num_bos, &fence_fd);
}
