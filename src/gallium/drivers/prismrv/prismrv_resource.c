/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * prismrv_resource.c — GEM-backed pipe resources.
 *
 * Uses u_transfer_helper like the other modern gallium drivers; the
 * vtbl below provides allocation, destruction and CPU mapping.
 */
#include "prismrv_resource.h"

#include <sys/mman.h>
#include <unistd.h>

#include "util/u_memory.h"
#include "util/u_transfer.h"
#include "util/u_transfer_helper.h"

#include "prismrv_context.h"
#include "prismrv_drmif.h"

static struct pipe_resource *
prismrv_resource_create(struct pipe_screen *pscreen,
                        const struct pipe_resource *tmpl)
{
   struct prismrv_resource *res = CALLOC_STRUCT(prismrv_resource);
   if (!res)
      return NULL;

   if (tmpl->target != PIPE_TEXTURE_2D && tmpl->target != PIPE_TEXTURE_RECT) {
      FREE(res);
      return NULL;
   }
   if (tmpl->depth0 > 1 || tmpl->array_size > 1) {
      FREE(res);
      return NULL;
   }

   /* single-sample, linear, no mipmaps for the initial bring-up */
   res->base = *tmpl;
   res->base.last_level = 0;
   res->base.nr_samples = 0;
   res->base.nr_storage_samples = 0;

   return &res->base;
}

void
prismrv_resource_allocate_gpu(struct prismrv_screen *screen,
                              struct prismrv_resource *res)
{
   if (res->gem_handle)
      return;
   res->size = res->base.width0 * res->base.height0 * 4; /* BGRA8 */
   res->gem_handle = prismrv_drm_gem_create(screen->fd, res->size);
}

static void
prismrv_resource_destroy(struct pipe_screen *pscreen,
                         struct pipe_resource *pres)
{
   struct prismrv_resource *res = to_prismrv_resource(pres);
   struct prismrv_screen *screen = to_prismrv_screen(pscreen);

   if (res->cpu_map && res->cpu_map != MAP_FAILED)
      munmap(res->cpu_map, res->size);
   /* GEM handle is freed when the fd is closed or explicitly via
    * DRM_IOCTL_GEM_CLOSE (not yet in the uAPI) */
   (void)screen;
   FREE(res);
}

static void *
prismrv_transfer_map(struct pipe_context *pctx,
                     struct pipe_resource *pres,
                     unsigned level,
                     unsigned usage,
                     const struct pipe_box *box,
                     struct pipe_transfer **ptransfer)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);
   struct prismrv_resource *res = to_prismrv_resource(pres);
   struct prismrv_screen *screen = ctx->screen;

   if (!res->gem_handle)
      prismrv_resource_allocate_gpu(screen, res);

   if (!res->cpu_map || res->cpu_map == MAP_FAILED)
      res->cpu_map = prismrv_drm_gem_map(screen->fd, res->gem_handle,
                                         res->size);

   if (!res->cpu_map || res->cpu_map == MAP_FAILED)
      return NULL;

   /* return pointer to the requested box origin */
   unsigned offset = box->y * pres->width0 * 4 + box->x * 4;
   return res->cpu_map + offset;
}

static void
prismrv_transfer_unmap(struct pipe_context *pctx,
                       struct pipe_transfer *ptransfer)
{
   /* CPU writes are coherent through the mmap; GPU coherency is
    * maintained by the kernel CCB cache-control field. */
}

static void
prismrv_transfer_flush_region(struct pipe_context *pctx,
                              struct pipe_transfer *ptransfer,
                              const struct pipe_box *box)
{
}

static const struct u_transfer_vtbl transfer_vtbl = {
   .resource_create       = prismrv_resource_create,
   .resource_destroy      = prismrv_resource_destroy,
   .transfer_map          = prismrv_transfer_map,
   .transfer_unmap        = prismrv_transfer_unmap,
   .transfer_flush_region = prismrv_transfer_flush_region,
};

void
prismrv_resource_screen_init(struct prismrv_screen *screen)
{
   screen->base.resource_destroy = prismrv_resource_destroy;
   screen->base.transfer_helper =
      u_transfer_helper_create(&transfer_vtbl, 0);
}

void
prismrv_resource_screen_fini(struct prismrv_screen *screen)
{
   if (screen->base.transfer_helper)
      u_transfer_helper_destroy(screen->base.transfer_helper);
}
