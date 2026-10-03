/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * prismrv_fence.c — sync_file fd backed pipe fences.
 *
 * The kernel submit ioctl returns a sync_file fd signalling render
 * completion; we wrap it in a pipe_fence_handle so st/mesa can wait on
 * it (REVIEW R3 resolution).
 */
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include "util/os_file.h"
#include "util/u_memory.h"
#include "util/u_inlines.h"

#include "prismrv_screen.h"
#include "prismrv_context.h"
#include "prismrv_fence.h"

struct pipe_fence_handle {
   struct pipe_reference reference;
   int fd;
};

struct pipe_fence_handle *
prismrv_fence_create(int fd)
{
   struct pipe_fence_handle *fence = CALLOC_STRUCT(pipe_fence_handle);
   if (!fence)
      return NULL;
   pipe_reference_init(&fence->reference, 1);
   fence->fd = fd;
   return fence;
}

static void
prismrv_fence_destroy(struct pipe_fence_handle *fence)
{
   if (fence->fd >= 0)
      close(fence->fd);
   FREE(fence);
}

static void
prismrv_fence_reference(struct pipe_screen *pscreen,
                        struct pipe_fence_handle **ptr,
                        struct pipe_fence_handle *fence)
{
   /*
    * pipe_reference_described() accepts NULL for both dst and src, so
    * pipe_reference() is safe even when *ptr is NULL (first assignment)
    * or fence is NULL (release).  The old code wrote
    *   pipe_reference(&(*ptr)->reference, &fence->reference)
    * which dereferenced *ptr before checking it — crashing whenever
    * fence_reference was called to initialise a NULL slot.
    */
   struct pipe_reference *old_ref = *ptr ? &(*ptr)->reference : NULL;
   struct pipe_reference *new_ref = fence  ? &fence->reference  : NULL;

   if (pipe_reference(old_ref, new_ref))
      prismrv_fence_destroy(*ptr);
   *ptr = fence;
}

static bool
prismrv_fence_finish(struct pipe_screen *pscreen, struct pipe_context *pctx,
                     struct pipe_fence_handle *fence, uint64_t timeout)
{
   struct pollfd pfd = { .fd = fence->fd, .events = POLLIN };
   int timeout_ms = timeout > (uint64_t)INT32_MAX ? -1
                                                  : (int)(timeout / 1000000);
   return poll(&pfd, 1, timeout_ms) > 0;
}

static int
prismrv_fence_get_fd(struct pipe_screen *pscreen,
                     struct pipe_fence_handle *fence)
{
   return os_dupfd_cloexec(fence->fd);
}

void
prismrv_fence_screen_init(struct prismrv_screen *screen)
{
   screen->base.fence_reference = prismrv_fence_reference;
   screen->base.fence_finish = prismrv_fence_finish;
   screen->base.fence_get_fd = prismrv_fence_get_fd;
}

void
prismrv_fence_context_init(struct prismrv_context *ctx)
{
}
