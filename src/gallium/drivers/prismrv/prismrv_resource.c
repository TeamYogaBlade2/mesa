/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * prismrv_resource.c — GEM-backed pipe resources.
 */
#include "util/u_inlines.h"
#include "prismrv_resource.h"
#include "prismrv_context.h"

#include <sys/mman.h>
#include <unistd.h>
#include <poll.h>

#include "util/u_math.h"
#include "util/u_memory.h"
#include "util/u_transfer.h"
#include "util/u_transfer_helper.h"
#include "util/format/u_format.h"

#include "prismrv_drmif.h"
#include "util/u_math.h"

/*
 * Compute the packed mip layout.  Each level is tightly packed
 * (stride = width * bpp) and 64-byte aligned; returns the total size
 * (not yet page aligned).  Level 0 is always at offset 0.
 */
static uint64_t
prismrv_compute_layout(struct prismrv_resource *res)
{
   const struct pipe_resource *b = &res->base;
   unsigned bpp = util_format_get_blocksize(b->format);
   uint64_t off = 0;

   for (unsigned l = 0; l <= b->last_level && l < PIPE_MAX_TEXTURE_LEVELS; l++) {
      res->level_offset[l] = (uint32_t)off;
      off += (uint64_t)prismrv_resource_level_dim(b->width0, l) *
             prismrv_resource_level_dim(b->height0, l) * bpp;
      off = align64(off, 64);
   }
   return off;
}

/* GEM allocation: create a BO of the given size and cache the mapping. */
void
prismrv_resource_allocate_gpu(struct prismrv_screen *screen,
                              struct prismrv_resource *res)
{
   if (res->gem_handle)
      return;
   if (res->base.target == PIPE_BUFFER) {
      res->size = align64(res->base.width0, 4096);
   } else {
      /*
       * Use the actual bytes-per-block from the format description.
       * The previous code hardcoded * 4 which was correct for 32-bit
       * formats (B8G8R8A8, R8G8B8A8, A8R8G8B8) but wrong for any
       * format with a different block size, e.g. R8G8B8 (3 bytes/px)
       * would under-allocate by 25 %, causing out-of-bounds writes.
       */
      res->size = align64(prismrv_compute_layout(res), 4096);
   }
   res->fd = screen->fd;
   res->gem_handle = prismrv_drm_gem_create(screen->fd, res->size,
                                            &res->gpu_va);
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
      unsigned bpp = (tmpl->target == PIPE_BUFFER)
         ? 1 : util_format_get_blocksize(tmpl->format);
      uint64_t bytes = (tmpl->target == PIPE_BUFFER)
         ? align64(tmpl->width0, 4096)
         : align64((uint64_t)tmpl->width0 * tmpl->height0 * bpp * 2, 4096);
      /* (x2 bounds a full mip chain, which is < 4/3 of level 0) */
      if (bytes > (uint64_t)UINT32_MAX) {
         FREE(res);
         return NULL;
      }
   }
   if (tmpl->depth0 > 1 || tmpl->array_size > 1 ||
       tmpl->last_level >= PIPE_MAX_TEXTURE_LEVELS) {
      FREE(res);
      return NULL;
   }

   res->base = *tmpl;
   pipe_reference_init(&res->base.reference, 1);
   res->base.screen = pscreen;
   res->base.nr_samples = 0;
   res->base.nr_storage_samples = 0;
   res->fd = -1;   /* set by prismrv_resource_allocate_gpu() */

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
   /*
    * Close the GEM handle on the fd that created it.  res->fd is set
    * at allocation time by prismrv_resource_allocate_gpu().
    *
    * The old code wrote  prismrv_drm_gem_close(pscreen ? 0 : 0, ...)
    * which always passed fd=0 (the conditional result is trivially 0
    * either way), so every BO leaked until the DRM fd was closed.
    */
   if (res->gem_handle && res->fd >= 0)
      prismrv_drm_gem_close(res->fd, res->gem_handle);
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

   if (!res->gem_handle || level > pres->last_level)
      return NULL;

   /*
    * CPU access hazard.  Any CPU read OR write of a resource the GPU may
    * have written (render target, readback) or may still read (texture,
    * vertex data being rewritten) needs the GPU to be done: submit what
    * is still queued in this context, then wait for it.  (The kernel
    * also does cache maintenance when a job retires, but that only
    * makes the data visible once the job has finished - it does not
    * wait for it.)  The wait is per context, not per resource: no
    * per-resource tracking exists yet, and multi-context sharing has no
    * export path.  PIPE_MAP_UNSYNCHRONIZED skips it for callers that
    * know better.
    */
   if (!(usage & PIPE_MAP_UNSYNCHRONIZED) && !prismrv_resource_sync(ctx, res))
      return NULL;       /* GPU not provably done: no CPU pointer */

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
   {
      unsigned bpp = util_format_get_blocksize(pres->format);
      pt->stride = prismrv_resource_level_dim(pres->width0, level) * bpp;
   }
   pt->layer_stride = pt->stride * prismrv_resource_level_dim(pres->height0, level);

   *ptransfer = pt;

   uint64_t offset = (pres->target == PIPE_BUFFER ? 0 : res->level_offset[level]) +
                     (uint64_t)box->y * pt->stride +
                     (uint64_t)box->x * util_format_get_blocksize(pres->format);
   if (pres->target == PIPE_BUFFER)
      offset = box->x;
   return (uint8_t *)res->cpu_map + offset;
}

static void
prismrv_transfer_unmap(struct pipe_context *pctx,
                       struct pipe_transfer *ptransfer)
{
   /*
    * NOTE: no explicit cache maintenance yet.  BOs are shmem pages
    * mapped write-combined only when PRISMRV_BO_UNCACHED is set; the
    * default cached mapping relies on the emulator / a coherent
    * interconnect.  GPU-side cache control (CCB cache_control, PTE
    * cache-consistent bit) is not wired up - see the kernel
    * documentation of the CCB before using this on real hardware.
    */
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

static bool
prismrv_can_create_resource(struct pipe_screen *pscreen,
                            const struct pipe_resource *tmpl)
{
   uint64_t bytes;

   if (tmpl->target != PIPE_BUFFER && tmpl->target != PIPE_TEXTURE_2D &&
       tmpl->target != PIPE_TEXTURE_RECT)
      return false;
   if (tmpl->depth0 > 1 || tmpl->array_size > 1 ||
       tmpl->last_level >= PIPE_MAX_TEXTURE_LEVELS || tmpl->nr_samples > 1)
      return false;
   if (tmpl->target == PIPE_BUFFER)
      bytes = tmpl->width0;
   else
      bytes = (uint64_t)tmpl->width0 * tmpl->height0 *
              util_format_get_blocksize(tmpl->format) * 2;
   return bytes <= UINT32_MAX - 4096;
}

void
prismrv_resource_screen_init(struct prismrv_screen *screen)
{
   /*
    * u_transfer_helper only supplies the transfer_* entry points; the
    * screen's own resource_create/destroy must be installed here.
    */
   screen->base.resource_create = prismrv_resource_create;
   screen->base.resource_destroy = prismrv_resource_destroy;
   screen->base.can_create_resource = prismrv_can_create_resource;
   screen->base.transfer_helper =
      u_transfer_helper_create(&transfer_vtbl, 0);
}

void
prismrv_resource_screen_fini(struct prismrv_screen *screen)
{
   if (screen->base.transfer_helper)
      u_transfer_helper_destroy(screen->base.transfer_helper);
}
