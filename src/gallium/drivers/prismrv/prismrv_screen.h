/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 */
#ifndef PRISMRV_SCREEN_H_
#define PRISMRV_SCREEN_H_

#include <stdbool.h>

#include "frontend/drm_driver.h"

struct pipe_screen *prismrv_screen_create(int fd,
                                          const struct pipe_screen_config *config);

/* called from the winsys layer */
struct pipe_screen *prismrv_drm_screen_create(int fd,
                                              const struct pipe_screen_config *config);

#endif /* PRISMRV_SCREEN_H_ */
