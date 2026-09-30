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

#include <errno.h>
#include <fcntl.h>
#include <xf86drm.h>
#include <drm-uapi/drm.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "drm-uapi/prismrv_drm.h"  /* local copy from linux tree */

int
prismrv_drm_get_param(int fd, uint32_t param, uint64_t *value)
{
   struct drm_prismrv_get_param p = { .param = param };
   if (drmIoctl(fd, DRM_IOCTL_PRISMRV_GET_PARAM, &p))
      return -errno;
   *value = p.value;
   return 0;
}

uint32_t
prismrv_drm_gem_create(int fd, uint64_t size, uint32_t *gpu_va)
{
   struct drm_prismrv_gem_create c = { .size = size };
   if (drmIoctl(fd, DRM_IOCTL_PRISMRV_GEM_CREATE, &c))
      return 0;
   *gpu_va = c.gpu_va;
   return c.handle;
}

void
prismrv_drm_gem_close(int fd, uint32_t handle)
{
   struct drm_gem_close arg = { .handle = handle };
   drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &arg);
}

void *
prismrv_drm_gem_map(int fd, uint32_t handle, uint64_t size)
{
   struct drm_prismrv_gem_mmap_offset mo = { .handle = handle };
   if (drmIoctl(fd, DRM_IOCTL_PRISMRV_GEM_MMAP_OFFSET, &mo))
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

   ret = drmIoctl(fd, DRM_IOCTL_PRISMRV_SUBMIT, &s);
   if (ret == 0 && out_fence_fd)
      *out_fence_fd = (int32_t)s.out_fence_fd;
   return ret;
}
