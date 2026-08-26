/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * prismrv_resource.c — GEM-backed pipe resources.
 */
#include "prismrv_resource.h"
#include "prismrv_context.h"

#include <sys/mman.h>
#include <unistd.h>

#include "util/u_math.h"
#include "util/u_memory.h"
#include "util/u_transfer.h"
#include "util/u_transfer_helper.h"

#include "prismrv_drmif.h"

/* GEM allocation: create a BO of the given size and cache the mapping. */
void
prismrv_resource_allocate_gpu(struct prismrv_screen *screen,
                              struct prismrv_resource *res)
{
   if (res->gem_handle)
      return;
   res->size = (res->base.target == PIPE_BUFFER)
      ? align64(res->base.width0, 4096)
      : align64((uint64_t)res->base.width0 * res->base.height0 * 4, 4096);
   res->fd = screen->fd;
   res->gem_handle = prismrv_drm_gem_create(screen->fd, res->size);
}

void *
prismrv_resource_map(struct pipe_resource *pres)
{
   struct prismrv_resource *res = to_prismrv_resource(pres);
   struct prismrv_screen *screen = to_prismrv_screen(pres->screen);

   if (!res->gem_handle)
      prismrv_resource_allocate_gpu(screen, res);
   if (!res->cpu_map || res->cpu_map == MAP_FAILED) {
      res->cpu_map = prismrv_drm_gem_map(screen->fd, res->gem_handle,
                                         res->size);
      if (!res->cpu_map || res->cpu_map == MAP_FAILED)
         return NULL;
   }
   return res->cpu_map;
}

static struct pipe_resource *
prismrv_resource_create(struct pipe_screen *pscreen,
                        const struct pipe_resource *tmpl)
{
   struct prismrv_screen *screen = to_prismrv_screen(pscreen);
   struct prismrv_resource *res = CALLOC_STRUCT(prismrv_resource);
   if (!res)
      return NULL;

   /*
    * Buffers are legal resources (VBO/UBO/constant data all come in as
    * PIPE_BUFFER via pipe_buffer_create).  Rejecting them made every
    * glBufferData fail and left draws without a VBO.
    */
   if (tmpl->target != PIPE_BUFFER &&
       tmpl->target != PIPE_TEXTURE_2D &&
       tmpl->target != PIPE_TEXTURE_RECT) {
      FREE(res);
      return NULL;
   }
   {
      /* buffer size comes from width0; textures use w*h*bpp */
      uint64_t bytes = (tmpl->target == PIPE_BUFFER)
         ? align64(tmpl->width0, 4096)
         : align64((uint64_t)tmpl->width0 * tmpl->height0 * 4, 4096);
      if (bytes > (uint64_t)UINT32_MAX) {
         FREE(res);
         return NULL;
      }
   }
   if (tmpl->depth0 > 1 || tmpl->array_size > 1) {
      FREE(res);
      return NULL;
   }

   res->base = *tmpl;
   res->base.last_level = 0;
   res->base.nr_samples = 0;
   res->base.nr_storage_samples = 0;

   /* allocate the GEM BO eagerly so transfers work immediately */
   prismrv_resource_allocate_gpu(screen, res);
   if (!res->gem_handle) {
      FREE(res);
      return NULL;
   }

   return &res->base;
}

static void
prismrv_resource_destroy(struct pipe_screen *pscreen,
                         struct pipe_resource *pres)
{
   struct prismrv_resource *res = to_prismrv_resource(pres);

   if (res->cpu_map && res->cpu_map != MAP_FAILED)
      munmap(res->cpu_map, res->size);
   /* close the GEM handle: without this every texture/RT leaks a BO
    * until the fd is closed */
   if (res->gem_handle)
      prismrv_drm_gem_close(pscreen ? 0 : 0, res->gem_handle);
   FREE(res);
}

static unsigned
prismrv_get_offset(const struct pipe_resource *pres,
                   const struct pipe_box *box)
{
   return box->y * pres->width0 * 4 + box->x * 4;
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
      return NULL;

   if (!res->cpu_map || res->cpu_map == MAP_FAILED) {
      res->cpu_map = prismrv_drm_gem_map(screen->fd, res->gem_handle,
                                         res->size);
      if (!res->cpu_map || res->cpu_map == MAP_FAILED)
         return NULL;
   }

   struct pipe_transfer *pt = CALLOC_STRUCT(pipe_transfer);
   if (!pt)
      return NULL;

   pt->resource = pres;
   pt->level = level;
   pt->usage = usage;
   pt->box = *box;
   pt->stride = pres->width0 * 4;
   pt->layer_stride = pt->stride;

   *ptransfer = pt;

   unsigned offset = prismrv_get_offset(pres, box);
   return res->cpu_map + offset;
}

static void
prismrv_transfer_unmap(struct pipe_context *pctx,
                       struct pipe_transfer *ptransfer)
{
   /* CPU writes are coherent through the mmap; the kernel CCB
    * cache-control field handles GPU-side coherency on submit. */
   FREE(ptransfer);
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
