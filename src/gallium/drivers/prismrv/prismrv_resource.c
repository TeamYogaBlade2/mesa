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

/* placeholder: real allocation goes through prismrv_drm_gem_create and
 * is wired in the next iteration; this file currently only establishes
 * the transfer helper plumbing so the driver compiles against the
 * modern gallium resource model. */

static void
prismrv_resource_destroy(struct pipe_screen *pscreen,
                         struct pipe_resource *pres)
{
   FREE(pres);
}

static void *
prismrv_transfer_map(struct pipe_context *pctx,
                     struct pipe_resource *pres,
                     unsigned level,
                     unsigned usage,
                     const struct pipe_box *box,
                     struct pipe_transfer **ptransfer)
{
   return NULL; /* implemented with GEM mmap in a follow-up */
}

static void
prismrv_transfer_unmap(struct pipe_context *pctx,
                       struct pipe_transfer *ptransfer)
{
}

static const struct u_transfer_vtbl transfer_vtbl = {
   .resource_create       = prismrv_resource_create,
   .resource_destroy      = prismrv_resource_destroy,
   .transfer_map          = prismrv_transfer_map,
   .transfer_unmap        = prismrv_transfer_unmap,
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
