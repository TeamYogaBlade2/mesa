/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 */
#ifndef PRISMRV_DRM_PUBLIC_H_
#define PRISMRV_DRM_PUBLIC_H_

struct pipe_screen;
struct pipe_screen_config;

struct pipe_screen *prismrv_drm_screen_create(int fd,
                                              const struct pipe_screen_config *config);

#endif /* PRISMRV_DRM_PUBLIC_H_ */
