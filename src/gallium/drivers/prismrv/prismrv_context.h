/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 */
#ifndef PRISMRV_CONTEXT_H_
#define PRISMRV_CONTEXT_H_

#include "prismrv_device.h"

struct blitter_context;
struct u_upload_mgr;

struct prismrv_batch {
   uint32_t cmd_handle;
   uint8_t *cmd_map;
   uint32_t cmd_size;
   uint32_t cmd_capacity;
};

struct prismrv_sampler_view {
   struct pipe_sampler_view base;
};

#define PRISMRV_MAX_VIEWPORTS 16

static inline const struct pipe_framebuffer_state *
prismrv_framebuffer(struct prismrv_context *ctx)
{
   return &ctx->framebuffer;
}

struct prismrv_context {
   struct pipe_context base;
   struct prismrv_screen *screen;

   struct prismrv_batch batch;
   struct blitter_context *blitter;
   struct u_upload_mgr *uploader;

   struct pipe_framebuffer_state framebuffer;
   struct pipe_scissor_state scissors[PRISMRV_MAX_VIEWPORTS];
};

struct pipe_context *
prismrv_context_create(struct pipe_screen *pscreen, void *priv,
                       unsigned flags);

void prismrv_batch_init_context(struct prismrv_context *ctx);
void prismrv_context_init(struct prismrv_context *ctx);

#endif /* PRISMRV_CONTEXT_H_ */
