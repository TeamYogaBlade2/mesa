/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 */
#ifndef PRISMRV_FENCE_H_
#define PRISMRV_FENCE_H_

#include "pipe/p_context.h"
#include "pipe/p_state.h"

struct prismrv_screen;
struct prismrv_context;

struct pipe_fence_handle *prismrv_fence_create(int fd);
void prismrv_fence_screen_init(struct prismrv_screen *screen);
void prismrv_fence_context_init(struct prismrv_context *ctx);

#endif /* PRISMRV_FENCE_H_ */
