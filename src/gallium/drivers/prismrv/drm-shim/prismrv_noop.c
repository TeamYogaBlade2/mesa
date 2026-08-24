/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * prismrv_noop.c — drm-shim backend for the PrismRV driver.
 *
 * Intercepts the prismrv DRM ioctls so that the gallium driver can be
 * exercised without a real kernel module.  GEM objects are tracked in
 * a simple table; SUBMIT is a no-op that records the command size.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "drm-shim/drm_shim.h"
#include "drm-uapi/prismrv_drm.h"

/* simple GEM handle → size table (drm-shim core handles actual memory) */
static struct {
   uint64_t size;
} gem_sizes[1024];
static uint32_t next_handle = 1;

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
   case 1: /* PRISMRV_PARAM_GPU_ID: raw EUR_CR_CORE_REVISION for SGX544 rev115 */
      gp->value = (0x0544 << 16) | (115 << 8);
      return 0;
   case 2: /* PRISMRV_PARAM_CORE_COUNT */
      gp->value = 1;
      return 0;
   case 3: /* PRISMRV_PARAM_UKERNEL_SIZE */
      gp->value = 106956;
      return 0;
   case 4: /* PRISMRV_PARAM_ERRATA */
      gp->value = (1 << 7) | (1 << 11); /* BRN_31780 | BRN_36513 */
      return 0;
   default:
      fprintf(stderr, "prismrv shim: unknown param %u\n", gp->param);
      return -1;
   }
}

static int
prismrv_ioctl_gem_create(int fd, unsigned long request, void *arg)
{
   struct drm_prismrv_gem_create *c = arg;

   if (next_handle >= 1024) {
      fprintf(stderr, "prismrv shim: GEM handle exhausted\n");
      return -1;
   }
   c->handle = next_handle++;
   gem_sizes[c->handle] = c->size;
   return 0;
}

static int
prismrv_ioctl_gem_mmap_offset(int fd, unsigned long request, void *arg)
{
   struct drm_prismrv_gem_mmap_offset *m = arg;

   m->offset = (uint64_t)m->handle << 12;
   return 0;
}

static int
prismrv_ioctl_submit(int fd, unsigned long request, void *arg)
{
   /* record submission; no hardware to drive */
   return 0;
}

static int (*driver_ioctls[])(int fd, unsigned long request, void *arg) = {
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

   shim_device.version_major = 1;
   shim_device.version_minor = 0;
   shim_device.version_patchlevel = 0;

   drm_shim_platform_device_setup("prismrv", "/soc/gpu", "img,powervr-sgx544");
}
