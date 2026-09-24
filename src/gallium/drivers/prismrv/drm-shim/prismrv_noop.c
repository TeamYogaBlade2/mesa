/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * drm-shim backend for the PrismRV SGX driver: implements enough of the
 * prismrv uAPI (GEM_CREATE / GEM_MMAP_OFFSET / SUBMIT / GET_PARAM) for
 * running the Gallium driver against a fake DRM device without hardware.
 */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/eventfd.h>

#include "drm-shim/drm_shim.h"
#include "drm-uapi/prismrv_drm.h"

#include "util/u_math.h"

/*
 * Matches what the real kernel detects on MT6589 (SGX544 rev 115).
 *
 * EUR_CR_CORE_ID layout (offset 0x20):
 *   [31:16] designer (Imagination = 0x0000 in published defs)
 *   [15:0]  core_id  (SGX544 = 0x0144)
 *
 * EUR_CR_CORE_REVISION layout (offset 0x24):
 *   [31:24] designer  [23:16] major=0x73  [15:8] minor  [7:0] maintenance
 *
 * The previous SHIM_GPU_ID 0x05440073 was interpreted as
 * (gpu_id >> 16) & 0xffff == 0x0544, which does not match the kernel's
 * PRISMRV_CORE_SGX544 = 0x0144.  That mismatch meant core_lookup()
 * always fell through to the (now removed) fallback.
 */
#define SHIM_CORE_ID    0x00000144ull   /* EUR_CR_CORE_ID: designer=0, core=SGX544 */
#define SHIM_CORE_REV   0x00730000ull   /* EUR_CR_CORE_REVISION: major=0x73 */
#define SHIM_CORES      1
#define SHIM_UKSIZE     (106956)
#define SHIM_ERRATA     0x801 /* BRN_31780 | BRN_36513 */

static int
prismrv_ioctl_noop(int fd, unsigned long request, void *arg)
{
   return 0;
}

static int
prismrv_ioctl_get_param(int fd, unsigned long request, void *arg)
{
   struct drm_prismrv_get_param *gp = arg;

   switch (gp->param) {
   case PRISMRV_PARAM_CORE_ID:
      gp->value = SHIM_CORE_ID;
      return 0;
   case PRISMRV_PARAM_CORE_REVISION:
      gp->value = SHIM_CORE_REV;
      return 0;
   case PRISMRV_PARAM_CORE_COUNT:
      gp->value = SHIM_CORES;
      return 0;
   case PRISMRV_PARAM_UKERNEL_SIZE:
      gp->value = SHIM_UKSIZE;
      return 0;
   case PRISMRV_PARAM_ERRATA:
      gp->value = SHIM_ERRATA;
      return 0;
   default:
      fprintf(stderr, "Unknown DRM_IOCTL_PRISMRV_GET_PARAM %u\n", gp->param);
      return -1;
   }
}

static int
prismrv_ioctl_gem_create(int fd, unsigned long request, void *arg)
{
   struct drm_prismrv_gem_create *create = arg;

   struct shim_fd *shim_fd = drm_shim_fd_lookup(fd);
   struct shim_bo *bo = calloc(1, sizeof(*bo));
   size_t size = align64(create->size, 4096);

   drm_shim_bo_init(bo, size);

   create->handle = drm_shim_bo_get_handle(shim_fd, bo);

   drm_shim_bo_put(bo);

   return 0;
}

static int
prismrv_ioctl_gem_mmap_offset(int fd, unsigned long request, void *arg)
{
   struct drm_prismrv_gem_mmap_offset *mo = arg;

   struct shim_fd *shim_fd = drm_shim_fd_lookup(fd);
   struct shim_bo *bo = drm_shim_bo_lookup(shim_fd, mo->handle);

   mo->offset = drm_shim_bo_get_mmap_offset(shim_fd, bo);

   drm_shim_bo_put(bo);

   return 0;
}

static int
prismrv_ioctl_submit(int fd, unsigned long request, void *arg)
{
   struct drm_prismrv_submit *submit = arg;

   /*
    * Complete immediately.  Hand out an eventfd and write to it so it
    * is READABLE right away — prismrv_fence_finish() poll()s for
    * POLLIN, and an eventfd that was never written would block
    * forever (glFinish hung on exactly this).
    */
   uint64_t one = 1;
   int efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
   if (efd < 0)
      return -1;
   if (write(efd, &one, sizeof(one)) != sizeof(one)) {
      close(efd);
      return -1;
   }
   submit->out_fence_fd = efd;
   return 0;
}

static ioctl_fn_t driver_ioctls[] = {
   [DRM_PRISMRV_GEM_CREATE]       = prismrv_ioctl_gem_create,
   [DRM_PRISMRV_GEM_MMAP_OFFSET]  = prismrv_ioctl_gem_mmap_offset,
   [DRM_PRISMRV_SUBMIT]           = prismrv_ioctl_submit,
   [DRM_PRISMRV_GET_PARAM]        = prismrv_ioctl_get_param,
};

void
drm_shim_driver_init(void)
{
   shim_device.driver_ioctls = driver_ioctls;
   shim_device.driver_ioctl_count = ARRAY_SIZE(driver_ioctls);

   shim_device.version_major = PRISMRV_UAPI_VERSION;
   shim_device.version_minor = 0;
   shim_device.version_patchlevel = 0;

   /* The real node is a render-capable platform device behind simple-bus. */
   drm_shim_platform_device_setup("prismrv", "/soc/gpu@13000000",
                                  "mediatek,mt6589-gpu");
}
