/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * Host test of the scene (frame resource plan) against a FAKE kernel: placement, free list, cross-references between the HW objects
 * and the BOs, and a complete frame assembled from the scene (every dword the frame refers to must be inside a BO of the submit).
 * Address plumbing only; nothing here says the hardware accepts the result.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../prismrv_vendor_ctx.h"
#include "../prismrv_vendor_res.h"

struct fake_bo { uint32_t handle, va, size; uint8_t *mem; };
struct fake { struct fake_bo bo[64]; unsigned n; uint32_t next[11]; unsigned submits; struct drm_prismrv_submit_vendor last; uint32_t last_bos[128]; };
static const uint32_t heap_base[11] = { 0x10000000, 0xd0000000, 0, 0xf0000000, 0xf4000000, 0xfc000000, 0xe4000000, 0xec000000, 0xe0000000, 0, 0xc8000000 };

static uint32_t
f_create(void *u, uint64_t size, uint32_t heap, uint32_t *va)
{
   struct fake *f = u;
   struct fake_bo *b = &f->bo[f->n];

   if (!f->next[heap]) f->next[heap] = heap_base[heap] + (heap == 3 ? 0x51000 : 0);
   b->handle = ++f->n; b->va = f->next[heap]; b->size = size; b->mem = calloc(1, size);
   f->next[heap] += (size + 0xfff) & ~0xfffu;
   *va = b->va;
   return b->handle;
}
static void *f_map(void *u, uint32_t h, uint64_t s) { (void)s; return ((struct fake *)u)->bo[h - 1].mem; }
static int
f_submit(void *u, const struct drm_prismrv_submit_vendor *a)
{
   struct fake *f = u;
   f->submits++; f->last = *a; memcpy(f->last_bos, (const void *)(uintptr_t)a->bos, a->num_bos * 4);
   return 0;
}

#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fprintf(stderr, "\n"); fails++; } } while (0)

static bool
inside(const struct fake *f, uint32_t va)
{
   for (unsigned i = 0; i < f->n; i++)
      if (va >= f->bo[i].va && va < f->bo[i].va + f->bo[i].size) return true;
   return false;
}

int
main(void)
{
   int fails = 0;
   struct fake f; memset(&f, 0, sizeof f);
   const struct prismrv_vk_ops ops = { .user = &f, .bo_create = f_create, .bo_map = f_map, .submit = f_submit };
   struct prismrv_vk_ctx c; struct prismrv_vk_scene s; const char *why = "";

   CHECK(prismrv_vk_ctx_init(&c, &ops, &why) == 0, "ctx init: %s", why);
   CHECK(prismrv_vk_scene_create(&c, &s, &why) == 0, "scene: %s", why);

   /* the PB free list: 34 entries, first/last as in every capture */
   {
      const uint32_t *fl = (const uint32_t *)s.bo[SCENE_PB_FREELIST].map;
      CHECK(fl[0] == 0xffff0001 && fl[1] == 0x00000002 && fl[2] == 0x00010003 && fl[33] == 0x0020ffff && fl[34] == 0, "free list %08x %08x %08x %08x %08x", fl[0], fl[1], fl[2], fl[33], fl[34]);
   }
   /* every address the HW objects name is a scene BO */
   {
      const struct prismrv_vk_hwobj *h = &s.hw;
      const uint32_t v[] = { h->pb_heap10, h->pb_next, h->sync_obj, h->sync_obj2, h->rtdataset_va, h->p0,
                             h->blk[0].s, h->blk[0].h1a, h->blk[0].h1b, h->blk[0].h10a, h->blk[0].h1c, h->blk[0].h10b, h->blk[0].h1d,
                             h->blk[0].h1e, h->blk[0].h1f, h->blk[1].s, h->blk[1].h1a, h->blk[1].h1b, h->blk[1].h1c, s.ta8_va, s.ta17_va, s.ccb3_va };
      for (unsigned i = 0; i < sizeof(v) / 4; i++)
         CHECK(inside(&f, v[i]), "HW object address %u (0x%08x) is not in a scene BO", i, v[i]);
   }

   /* a frame assembled from the scene */
   {
      static const float verts[9] = { -1, -1, 0, 3, -1, 0, -1, 3, 0 };
      static const uint32_t code[4] = { 0, 0xfa440700, 0xa0000000, 0x50850009 };
      struct prismrv_vk_frame_in in; struct prismrv_vk_frame_result r; unsigned i;
      uint8_t *vb; uint32_t vb_va, ib_va;
      uint8_t *ib;

      memset(&in, 0, sizeof in);
      vb = prismrv_vk_arena_alloc(&c.arena[PRISMRV_VK_ARENA_DATA], 64, 16, &vb_va);
      ib = prismrv_vk_arena_alloc(&c.arena[PRISMRV_VK_ARENA_DATA], 16, 16, &ib_va);
      memcpy(vb, verts, sizeof verts);
      ((uint16_t *)ib)[0] = 0; ((uint16_t *)ib)[1] = 1; ((uint16_t *)ib)[2] = 2;
      in.gl.width = in.gl.height = in.gl.vw = in.gl.vh = 64; in.gl.depth_far = 1.0; in.gl.depth_func = PRISMRV_VK_ALWAYS;
      in.gl.depth_test = -1; in.gl.color_mask[0] = in.gl.color_mask[1] = in.gl.color_mask[2] = in.gl.color_mask[3] = true;
      in.gl.rt_va = 0x10100000; /* the render target would be the caller's resource */
      in.code = code; in.code_size = sizeof code;
      in.n_attrs = 1; in.attr_va = vb_va; in.attr_dwords_m1 = 2; in.attr_stride = 12; in.index_va = ib_va; in.index_count = 3;
      in.hwrc_va = 0x00108368; in.clear_w = in.clear_h = 64.0f; in.clear_colour = 0xff000000;
      prismrv_vk_scene_fill_frame(&s, &in);
      CHECK(prismrv_vk_ctx_frame(&c, &in, &r, &why) == 0, "frame from scene: %s", why);
      CHECK(f.submits == 1 && f.last.num_bos >= SCENE_BO_COUNT, "submit bos %u", f.last.num_bos);
      for (i = 0; i < SCENE_BO_COUNT; i++) {
         unsigned k, found = 0;
         for (k = 0; k < f.last.num_bos; k++) found |= f.last_bos[k] == s.handles[i];
         CHECK(found, "scene BO %u missing from the submit", i);
      }
      /* the emitted 3DREGISTERS carry the scene addresses */
      {
         const uint32_t *rd = (const uint32_t *)s.bo[SCENE_RDETAILS].map;
         CHECK(rd[0] == 0x10100000 && rd[8] == s.ta8_va && rd[17] == s.ta17_va, "render details 0x%08x 0x%08x 0x%08x", rd[0], rd[8], rd[17]);
         CHECK(rd[0xf4 / 4] == s.hw.rtdataset_va && rd[0xec / 4] == s.hw.sync_obj2, "render details tail");
      }
      CHECK(!(r.unwired & PRISMRV_VK_UNWIRED_HW_OBJECT_CONTENTS), "HW object contents still unwired (0x%x)", r.unwired);
   }
   printf("scene: %d failures\n", fails);
   return fails ? 1 : 0;
}
