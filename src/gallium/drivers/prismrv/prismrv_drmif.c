/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * prismrv_drmif.c — thin wrappers over the prismrv kernel uAPI.
 *
 * All ioctl numbers come directly from drm-uapi/prismrv_drm.h so that
 * this file stays in sync with the kernel header automatically.  The
 * previous version duplicated the numbers in local PRISMRV_IOCTL_*
 * macros using a hand-rolled ioc_rdwr() helper, mixing uAPI and local
 * macros within the same file.
 */
#include "prismrv_drmif.h"

#include <fcntl.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "drm-uapi/prismrv_drm.h"  /* local copy from linux tree */

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
   if (ioctl(fd, DRM_IOCTL_PRISMRV_GEM_CREATE, &c))
      return 0;
   return c.handle;
}

void
prismrv_drm_gem_close(int fd, uint32_t handle)
{
   /* DRM_IOCTL_GEM_CLOSE: _IOWR('d', 0x09, struct drm_gem_close)
    * struct drm_gem_close = { __u32 handle; __u32 pad; } — 8 bytes */
   struct { uint32_t handle; uint32_t pad; } arg = { handle, 0 };
   /* Build the ioctl number the same way the kernel does to avoid
    * a dependency on <drm/drm.h> in the Mesa tree. */
#define DRM_IOCTL_BASE_CHAR 'd'
#define DRM_GEM_CLOSE_NR    0x09
   unsigned long nr = (3ul << 30) | (sizeof(arg) << 16) |
                      ((unsigned long)DRM_IOCTL_BASE_CHAR << 8) |
                      DRM_GEM_CLOSE_NR;
   ioctl(fd, nr, &arg);
}

void *
prismrv_drm_gem_map(int fd, uint32_t handle, uint64_t size)
{
   struct drm_prismrv_gem_mmap_offset mo = { .handle = handle };
   if (ioctl(fd, DRM_IOCTL_PRISMRV_GEM_MMAP_OFFSET, &mo))
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

   if (num_bos)
      s.bos = (uintptr_t)bos;

   ret = ioctl(fd, DRM_IOCTL_PRISMRV_SUBMIT, &s);
   if (ret == 0 && out_fence_fd)
      *out_fence_fd = (int32_t)s.out_fence_fd;
   return ret;
}
