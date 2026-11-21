/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * Gallium-facing side of the vendor-frame glue (prismrv_vendor_gl.h).  Enabled per context by PRISMRV_VENDOR_GL=1; needs
 * PRISMRV_VENDOR_HWRC=<hex address of the HW render context> (its contents were never captured, so the driver cannot create it).
 * EXPERIMENTAL and exclusive: draws the vendor frame cannot express are dropped.  Not compiled into the host tests.
 */
#ifndef PRISMRV_VENDOR_GL_PIPE_H
#define PRISMRV_VENDOR_GL_PIPE_H

#include <stdbool.h>

#include "pipe/p_context.h"

struct prismrv_context;
struct prismrv_vk_gl;

struct prismrv_vk_gl *prismrv_vk_gl_pipe_create(int fd);
void prismrv_vk_gl_pipe_destroy(struct prismrv_vk_gl *g);

/* true = the draw was consumed (recorded or dropped); the native path must not see it */
bool prismrv_vk_gl_pipe_draw(struct prismrv_context *ctx, const struct pipe_draw_info *info,
                             const struct pipe_draw_start_count_bias *draw);
bool prismrv_vk_gl_pipe_clear(struct prismrv_context *ctx, unsigned buffers, const union pipe_color_union *color);
bool prismrv_vk_gl_pipe_flush(struct prismrv_context *ctx);

#endif
