/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * prismrv_drmif.c — thin wrappers over the prismrv kernel uAPI.
 *
 * Mirrors include/uapi/drm/prismrv_drm.h from the kernel driver.
 */
#include "prismrv_drmif.h"

#include <fcntl.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "drm-uapi/prismrv_drm.h"  /* local copy from linux tree */

#define DRM_IOCTL_BASE 'd'
#define DRM_COMMAND_BASE 0x40

static uint32_t
ioc_rdwr(uint32_t nr, size_t size)
{
   return (3u << 30) | ((uint32_t)size << 16) | (DRM_IOCTL_BASE << 8) | nr;
}

#define PRISMRV_IOCTL_GEM_CREATE       ioc_rdwr(0x40, sizeof(struct drm_prismrv_gem_create))
#define PRISMRV_IOCTL_GEM_MMAP_OFFSET  ioc_rdwr(0x41, sizeof(struct drm_prismrv_gem_mmap_offset))
#define PRISMRV_IOCTL_SUBMIT           ioc_rdwr(0x42, sizeof(struct drm_prismrv_submit))
#define PRISMRV_IOCTL_GET_PARAM        ioc_rdwr(0x43, sizeof(struct drm_prismrv_get_param))

uint64_t
prismrv_drm_get_param(int fd, uint32_t param)
{
   struct drm_prismrv_get_param p = { .param = param };
   if (ioctl(fd, DRM_IOCTL_PRISMRV_GET_PARAM, &p))
      return UINT64_MAX;
   return p.value;
}

uint32_t
prismrv_drm_gem_create(int fd, uint64_t size)
{
   struct drm_prismrv_gem_create c = { .size = size };
   if (ioctl(fd, PRISMRV_IOCTL_GEM_CREATE, &c))
      return 0;
   return c.handle;
}

/* DRM_IOCTL_GEM_CLOSE: _IOWR('d', 0x09, struct drm_gem_close) */
#define DRM_GEM_CLOSE_HANDLE 0x09
#ifndef DRM_IOCTL_BASE
#define DRM_IOCTL_BASE 'd'
#endif

void
prismrv_drm_gem_close(int fd, uint32_t handle)
{
   /* struct drm_gem_close { __u32 handle; __u32 pad; }; — 8 bytes */
   uint32_t arg[2] = { handle, 0 };
   unsigned long nr = (3u << 30) | (sizeof(arg) << 16) |
                      ((unsigned long)DRM_IOCTL_BASE << 8) |
                      DRM_GEM_CLOSE_HANDLE;
   ioctl(fd, nr, arg);
}

void *
prismrv_drm_gem_map(int fd, uint32_t handle, uint64_t size)
{
   struct drm_prismrv_gem_mmap_offset mo = { .handle = handle };
   if (ioctl(fd, PRISMRV_IOCTL_GEM_MMAP_OFFSET, &mo))
      return MAP_FAILED;

   return mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
               fd, mo.offset);
}

int
prismrv_drm_submit(int fd, uint32_t cmd_type,
                   uint32_t cmd_handle, uint32_t cmd_size,
                   const uint32_t *bos, uint32_t num_bos,
                   int32_t *out_fence_fd)
{
   struct drm_prismrv_submit s = {
      .cmd_handle = cmd_handle,
      .cmd_size = cmd_size,
      .num_bos = num_bos,
      .cmd_type = cmd_type,
   };
   int ret;

   if (num_bos) {
      s.bos = (uintptr_t)bos;
   }

   ret = ioctl(fd, PRISMRV_IOCTL_SUBMIT, &s);
   if (ret == 0 && out_fence_fd)
      *out_fence_fd = (int32_t)s.out_fence_fd;
   return ret;
}
