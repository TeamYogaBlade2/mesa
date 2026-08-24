/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 */
#include "util/os_file.h"
#include "util/u_screen.h"

#include "prismrv_drm_public.h"
#include "prismrv/prismrv_screen.h"

struct pipe_screen *
prismrv_drm_screen_create(int fd, const struct pipe_screen_config *config)
{
   return u_pipe_screen_lookup_or_create(os_dupfd_cloexec(fd), config,
                                         NULL, prismrv_screen_create);
}
