/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 */
#include "prismrv_vendor_res.h"

#include <errno.h>
#include <string.h>

static const struct { uint32_t heap, size; } scene_spec[SCENE_BO_COUNT] = {
   [SCENE_ZBUF] = { PRISMRV_HEAP_DEFAULT, 0x80004 },
   [SCENE_TA_F] = { PRISMRV_HEAP_TA_DATA, 0x200 },  [SCENE_TA_D] = { PRISMRV_HEAP_TA_DATA, 0x80 },
   [SCENE_TA_E] = { PRISMRV_HEAP_TA_DATA, 0x90 },
   [SCENE_TA_A0] = { PRISMRV_HEAP_TA_DATA, 0x400 }, [SCENE_TA_C0] = { PRISMRV_HEAP_TA_DATA, 0x6e8 },
   [SCENE_TA_A1] = { PRISMRV_HEAP_TA_DATA, 0x400 }, [SCENE_TA_C1] = { PRISMRV_HEAP_TA_DATA, 0x6e8 },
   [SCENE_HWPB] = { PRISMRV_HEAP_KERNEL_DATA, 0x40 }, [SCENE_PB_NEXT] = { PRISMRV_HEAP_KERNEL_DATA, 0x10 },
   [SCENE_RTDATASET] = { PRISMRV_HEAP_KERNEL_DATA, 0xb0 }, [SCENE_P0] = { PRISMRV_HEAP_KERNEL_DATA, 0x4 },
   [SCENE_S0] = { PRISMRV_HEAP_KERNEL_DATA, 0x10 }, [SCENE_S1] = { PRISMRV_HEAP_KERNEL_DATA, 0x10 },
   [SCENE_SYNCINFO] = { PRISMRV_HEAP_KERNEL_DATA, 0x38 }, [SCENE_RDETAILS] = { PRISMRV_HEAP_KERNEL_DATA, PRISMRV_VK_RENDER_DETAILS_BYTES },
   [SCENE_SYNC1] = { PRISMRV_HEAP_SYNC, 0x8 }, [SCENE_SYNC2] = { PRISMRV_HEAP_SYNC, 0x8 },
   [SCENE_PB] = { PRISMRV_HEAP_3DPARAM_CTX, 0x822000 }, [SCENE_PB_FREELIST] = { PRISMRV_HEAP_3DPARAM_CTX, 0x40000 },
   [SCENE_H10A] = { PRISMRV_HEAP_3DPARAM_CTX, 0x200 }, [SCENE_H10B] = { PRISMRV_HEAP_3DPARAM_CTX, 0x100 },
};

/* the heap windows the kernel must honour (mirrors prismrv_vendor_ctx.c) */
static bool
in_heap(uint32_t heap, uint32_t va, uint32_t size)
{
   static const struct { uint32_t heap, base, size; } r[] = {
      { PRISMRV_HEAP_DEFAULT, 0x10000000u, 0x30000000u }, { PRISMRV_HEAP_TA_DATA, 0xd0000000u, 0x0cfff000u },
      { PRISMRV_HEAP_KERNEL_DATA, 0xf0000000u, 0x02fff000u }, { PRISMRV_HEAP_SYNC, 0xe0000000u, 0x00fff000u },
      { PRISMRV_HEAP_3DPARAM_CTX, 0xc8000000u, 0x07fff000u },
   };

   for (unsigned i = 0; i < sizeof(r) / sizeof(r[0]); i++)
      if (r[i].heap == heap)
         return va >= r[i].base && size <= r[i].size && va - r[i].base <= r[i].size - size;
   return false;
}

int
prismrv_vk_scene_create(struct prismrv_vk_ctx *c, struct prismrv_vk_scene *s, const char **why)
{
#define FAIL(e, m) do { *why = m; return e; } while (0)
   memset(s, 0, sizeof(*s));
   for (unsigned i = 0; i < SCENE_BO_COUNT; i++) {
      struct prismrv_vk_scene_bo_info *b = &s->bo[i];

      b->size = scene_spec[i].size;
      b->handle = c->ops.bo_create(c->ops.user, b->size, scene_spec[i].heap, &b->va);
      if (!b->handle)
         FAIL(-ENOMEM, "scene BO creation failed");
      if (!in_heap(scene_spec[i].heap, b->va, b->size))
         FAIL(-EFAULT, "kernel placed a scene BO outside the requested heap");
      b->map = c->ops.bo_map(c->ops.user, b->handle, b->size);
      if (!b->map)
         FAIL(-ENOMEM, "scene BO mapping failed");
      memset(b->map, 0, b->size);
      s->handles[i] = b->handle;
   }

   prismrv_vk_build_pb_freelist(PRISMRV_VK_PB_PAGES, (uint32_t *)s->bo[SCENE_PB_FREELIST].map);

   {
      struct prismrv_vk_hwobj *h = &s->hw;
      const uint32_t a0 = s->bo[SCENE_TA_A0].va, a1 = s->bo[SCENE_TA_A1].va;

      /* the parameter buffer range the HWPBDESC names: 0x80200000..0x80217800 in every capture (not a device VA of any
       * heap; UNVERIFIED how it relates to the PB BO) */
      h->pb_end = 0x80217800;
      h->pb_base = 0x80200000;
      h->pb_heap10 = s->bo[SCENE_PB_FREELIST].va;
      h->pb_next = s->bo[SCENE_PB_NEXT].va;
      h->sync_obj = s->bo[SCENE_SYNC2].va;
      h->sync_obj2 = s->bo[SCENE_SYNC1].va;
      h->rtdataset_va = s->bo[SCENE_RTDATASET].va;
      h->p0 = s->bo[SCENE_P0].va;
      h->blk[0] = (struct prismrv_vk_rtblock){ .s = s->bo[SCENE_S0].va, .h1a = a0, .h1b = a0 + 0x3c, .h10a = s->bo[SCENE_H10A].va,
                                               .h1c = s->bo[SCENE_TA_C0].va, .h10b = s->bo[SCENE_H10B].va,
                                               .h1d = s->bo[SCENE_TA_D].va, .h1e = s->bo[SCENE_TA_E].va, .h1f = s->bo[SCENE_TA_F].va };
      h->blk[1] = (struct prismrv_vk_rtblock){ .s = s->bo[SCENE_S1].va, .h1a = a1, .h1b = a1 + 0x3c, .h10a = s->bo[SCENE_H10A].va,
                                               .h1c = s->bo[SCENE_TA_C1].va, .h10b = s->bo[SCENE_H10B].va,
                                               .h1d = s->bo[SCENE_TA_D].va, .h1e = s->bo[SCENE_TA_E].va, .h1f = s->bo[SCENE_TA_F].va };
      s->ta17_va = a0;                 /* 3DREGISTERS dw17 = the dataset's first TA block */
      s->ta8_va = a1;                  /* dw8 = the render target data's first TA block */
      s->ccb3_va = s->bo[SCENE_ZBUF].va + 0x19000;   /* captures: +0x19000 / +0x1a000 into the 0x80004-byte buffer */
   }
   return 0;
}

void
prismrv_vk_scene_fill_frame(const struct prismrv_vk_scene *s, struct prismrv_vk_frame_in *in)
{
   in->hwobj = &s->hw;
   in->hw.hwpbdesc = s->bo[SCENE_HWPB].va;
   in->hw.syncinfo = s->bo[SCENE_SYNCINFO].va;
   in->hw.rtdataset = s->bo[SCENE_RTDATASET].va;
   in->hw.rtdata = s->bo[SCENE_RTDATASET].va + PRISMRV_VK_RTDATA_OFFSET;
   in->render_details_va = s->bo[SCENE_RDETAILS].va;
   in->hw_pb_map = (uint32_t *)s->bo[SCENE_HWPB].map;
   in->hw_sync_map = (uint32_t *)s->bo[SCENE_SYNCINFO].map;
   in->hw_rt_map = (uint32_t *)s->bo[SCENE_RTDATASET].map;
   in->hw_render_details_map = (uint32_t *)s->bo[SCENE_RDETAILS].map;
   in->hw.ta8 = s->ta8_va;                       /* the stream's TA words: heap 1 block, the F block, and the captured 0x40700000 */
   in->hw.ta9 = s->bo[SCENE_TA_F].va;
   in->hw.ta15 = 0x40700000;
   in->gl.ta8_va = s->ta8_va;
   in->gl.ta17_va = s->ta17_va;
   in->gl.ccb3_va = s->ccb3_va;
   in->extra_bos = s->handles;
   in->n_extra_bos = SCENE_BO_COUNT;
}
