/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 */
#ifndef PRISMRV_DRMIF_H_
#define PRISMRV_DRMIF_H_

#include <stdint.h>

uint64_t prismrv_drm_get_param(int fd, uint32_t param);
uint32_t prismrv_drm_gem_create(int fd, uint64_t size);
void prismrv_drm_gem_close(int fd, uint32_t handle);
void *prismrv_drm_gem_map(int fd, uint32_t handle, uint64_t size);
int prismrv_drm_submit(int fd, uint32_t cmd_type,
                       uint32_t cmd_handle, uint32_t cmd_size,
                       const uint32_t *bos, uint32_t num_bos,
                       int32_t *out_fence_fd);

#endif /* PRISMRV_DRMIF_H_ */
