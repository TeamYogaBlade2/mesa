/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * Host test of the Gallium glue against a FAKE kernel: eligibility (each unsupported feature is refused with its reason), index
 * generation for a non-indexed draw, clear colour packing, and a full flush that submits once with the scene + resource BOs.
 * Address plumbing only; nothing here says the hardware accepts the result.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../prismrv_vendor_gl.h"

struct fake_bo { uint32_t handle, va, size; uint8_t *mem; };
struct fake { struct fake_bo bo[96]; unsigned n; uint32_t next[11]; unsigned submits; struct drm_prismrv_submit_vendor last; uint32_t last_bos[128]; };
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

static struct { uint32_t size, pd_offset; int calls; int fail; } hw;
static uint32_t
f_hwrc(void *user, uint32_t size, uint32_t pd_offset, uint32_t *gpu_va)
{
   (void)user; hw.size = size; hw.pd_offset = pd_offset; hw.calls++;
   if (hw.fail) return 0;
   *gpu_va = 0xf0010000; return 99;
}

int
main(void)
{
   int fails = 0;
   struct fake f; memset(&f, 0, sizeof f);
   const struct prismrv_vk_ops ops = { .user = &f, .bo_create = f_create, .bo_map = f_map, .submit = f_submit };
   static struct prismrv_vk_gl g;
   const char *why = "";
   static const uint64_t vs[2] = { 0xfa44070000000000ull, 0xfb275000a0200000ull }, fs[2] = { 0xfa44070000000000ull, 0x50850009e0000500ull };
   struct prismrv_vk_gl_draw d;

   CHECK(prismrv_vk_gl_init(&g, &ops, 0, &why) == -EINVAL, "init without HWRC accepted");
   CHECK(prismrv_vk_gl_init(&g, &ops, 0x00108368, &why) == 0, "init: %s", why);

   {  /* kernel-created HWRC (vendor SGXRegisterHWRenderContextKM): size 0x54, PD patched in at +0x08 */
      struct fake f2; static struct prismrv_vk_gl g2; struct prismrv_vk_ops ops2 = ops;
      memset(&f2, 0, sizeof f2); ops2.user = &f2; ops2.hwrc_create = f_hwrc;
      CHECK(prismrv_vk_gl_init(&g2, &ops2, 0, &why) == 0, "init with kernel HWRC: %s", why);
      CHECK(hw.calls == 1 && hw.size == 0x54 && hw.pd_offset == 0x08, "hwrc_create args %u/%u", hw.size, hw.pd_offset);
      CHECK(g2.hwrc_va == 0xf0010000 && g2.hwrc_handle == 99, "hwrc va/handle");
      ops2.user = &f2; hw.fail = 1;
      CHECK(prismrv_vk_gl_init(&g2, &ops2, 0, &why) == -EIO, "failing HWRC_CREATE accepted");
      hw.fail = 0;
      CHECK(prismrv_vk_gl_init(&g2, &ops2, 0x00108368, &why) == 0 && g2.hwrc_handle == 0, "explicit HWRC init: %s", why);
      CHECK(hw.calls == 2, "explicit HWRC must not call the kernel (calls=%d)", hw.calls);
   }

   memset(&d, 0, sizeof d);
   d.fb_w = d.fb_h = 64; d.rt_va = 0x10100000; d.rt_handle = 77; d.rt_rgba8 = true; d.triangles = true;
   d.vw = d.vh = 64; d.depth_far = 1.0; d.color_mask[0] = d.color_mask[1] = d.color_mask[2] = d.color_mask[3] = true;

   d.vp_scale[0] = d.vp_scale[1] = 32.0f;
   d.vs_code = vs; d.vs_words = 2; d.fs_code = fs; d.fs_words = 2;
   d.n_attrs = 1; d.attr[0].va = 0x10197000; d.attr[0].handle = 78; d.attr[0].ncomp = 3; d.attr[0].stride = 12; d.attr[0].is_float = true;

   {  /* fixed-function state: what is carried, what is refused (own context so the frame counters stay independent) */
      struct fake f4; static struct prismrv_vk_gl g4; struct prismrv_vk_ops ops4 = ops; struct prismrv_vk_gl_draw x;
      memset(&f4, 0, sizeof f4); ops4.user = &f4;
      CHECK(prismrv_vk_gl_init(&g4, &ops4, 0x00108368, &why) == 0, "init g4: %s", why);
#define STATE_REFUSED(setup, text) do { x = d; setup; \
         CHECK(prismrv_vk_gl_record_draw(&g4, &x, &why) == -ENOTSUP && strstr(why, text), "'%s' not refused (%s)", text, why); } while (0)
      STATE_REFUSED(x.has_zs = true, "Z16");
      STATE_REFUSED(x.has_depth = true, "no GPU address");               /* depth surface without VA/handle */
      STATE_REFUSED(x.stencil_test = true, "stencil test");
      STATE_REFUSED(x.alpha_test = true, "alpha test");
      STATE_REFUSED(x.scissor_partial = true, "scissor");
      STATE_REFUSED(x.unmapped_state = true, "not measured");
      STATE_REFUSED(x.vw = 0, "empty viewport");
      STATE_REFUSED(x.triangles = false, "triangle and line");
      /* accepted: depth + cull + colour mask + a viewport offset + lines */
      d.count = 3;
      x = d; x.has_depth = true; x.zs_va = 0x10120000; x.zs_handle = 88; x.depth_test = x.depth_write = true; x.depth_func = 1;
      x.cull = 2; x.color_mask[1] = false; x.vx = 8; x.vy = 8; x.vw = x.vh = 32;
      CHECK(prismrv_vk_gl_record_draw(&g4, &x, &why) == 0, "depth/cull/mask draw refused: %s", why);
      CHECK(prismrv_vk_gl_flush(&g4, &why) == 0, "depth flush: %s", why);
      CHECK(f4.submits == 1, "depth frame submits %u", f4.submits);
      {
         unsigned k; bool zs_bo = false;
         for (k = 0; k < f4.last.num_bos; k++) zs_bo |= f4.last_bos[k] == 88;
         CHECK(zs_bo, "depth surface BO missing from the submit");
      }
      {  /* the HWRENDERDETAILS block (3DREGISTERS dw1 = 0x40700000) must carry the depth surface at dw2 and the depth format word at dw18 */
         unsigned b, i; bool found = false;
         for (b = 0; b < f4.n && !found; b++)
            for (i = 0; f4.bo[b].mem && i + 80 <= f4.bo[b].size; i += 4) {
               const uint32_t *w = (const uint32_t *)(f4.bo[b].mem + i);
               if (w[1] == 0x40700000 && w[0] == d.rt_va && w[2] == 0x10120000 && w[18] == 0x00151008) { found = true; break; }
            }
         CHECK(found, "3DREGISTERS with depth_va 0x10120000 / dw18 0x00151008 not found in any BO");
      }
      x = d; x.triangles = false; x.lines = true; x.count = 3;
      CHECK(prismrv_vk_gl_record_draw(&g4, &x, &why) == 0, "line draw refused: %s", why);
      CHECK(prismrv_vk_gl_flush(&g4, &why) == 0, "line flush: %s", why);
   }

   d.count = 3;

   /* each unsupported feature is refused with a reason and counted */
   {
      struct prismrv_vk_gl_draw x;
#define REFUSE(field, val, text) do { x = d; x.field = val; \
         CHECK(prismrv_vk_gl_record_draw(&g, &x, &why) == -ENOTSUP && strstr(why, text), "'%s' not refused (%s)", text, why); } while (0)
      REFUSE(has_zs, true, "depth");
      REFUSE(blend, true, "blending");
      REFUSE(textures, true, "textures");
      REFUSE(fs_uniforms, true, "uniforms");
      REFUSE(triangles, false, "triangle");
      REFUSE(rt_rgba8, false, "RGBA8");
      REFUSE(n_attrs, 3, "attributes");
      REFUSE(attr[0].is_float, false, "floats");
      REFUSE(vs_code, NULL, "native");
      REFUSE(count, 2, "empty");
      x = d; x.vs_varyings = x.fs_varyings = 1;       /* a varying with a single attribute: unmeasured */
      CHECK(prismrv_vk_gl_record_draw(&g, &x, &why) == -ENOTSUP && strstr(why, "measured"), "1 varying/1 attr not refused (%s)", why);
      REFUSE(fs_varyings, 1, "differ");               /* VS writes none, FS reads one */
      x = d; x.n_attrs = 2; x.attr[1] = x.attr[0]; x.vs_varyings = x.fs_varyings = 2;
      CHECK(prismrv_vk_gl_record_draw(&g, &x, &why) == -ENOTSUP && strstr(why, "more than one varying"), "2 varyings not refused (%s)", why);
      x = d; x.n_attrs = 2; x.attr[1] = x.attr[0];    /* two attributes but no varying: unmeasured */
      CHECK(prismrv_vk_gl_record_draw(&g, &x, &why) == -ENOTSUP && strstr(why, "measured"), "2 attrs/0 varyings not refused (%s)", why);
      CHECK(g.dropped == 14, "dropped %u", g.dropped);
   }

   /* a clear then a non-indexed draw: one submit, scene BOs + the three resources */
   prismrv_vk_gl_record_clear(&g, 1.0f, 0.5f, 0.0f, 1.0f);
   CHECK(g.clear_colour == 0xff0080ffu, "clear colour 0x%08x", g.clear_colour);
   CHECK(prismrv_vk_gl_record_draw(&g, &d, &why) == 0, "draw refused: %s", why);
   CHECK(prismrv_vk_gl_record_draw(&g, &d, &why) == -ENOTSUP && strstr(why, "more than one"), "second draw not refused");
   CHECK(prismrv_vk_gl_flush(&g, &why) == 0, "flush: %s", why);
   CHECK(f.submits == 1, "submits %u", f.submits);
   {
      unsigned i, k, h77 = 0, h78 = 0;
      for (k = 0; k < f.last.num_bos; k++) { h77 |= f.last_bos[k] == 77; h78 |= f.last_bos[k] == 78; }
      CHECK(h77 && h78, "resource BOs missing from the submit");
      for (i = 0; i < SCENE_BO_COUNT; i++) {
         unsigned found = 0;
         for (k = 0; k < f.last.num_bos; k++) found |= f.last_bos[k] == g.scene.handles[i];
         CHECK(found, "scene BO %u missing", i);
      }
   }
   /* generated indices 0,1,2 live in the data arena */
   {
      const uint16_t *ix = (const uint16_t *)(g.vk.arena[PRISMRV_VK_ARENA_DATA].map + 0);
      unsigned found = 0, i;
      for (i = 0; i + 2 < g.vk.arena[PRISMRV_VK_ARENA_DATA].used / 2; i++) found |= ix[i] == 0 && ix[i + 1] == 1 && ix[i + 2] == 2;
      CHECK(found, "generated indices 0,1,2 not in the data arena");
   }
   /* a flush with nothing recorded submits nothing */
   CHECK(prismrv_vk_gl_flush(&g, &why) == 0 && f.submits == 1, "empty flush submitted");
   /* an indexed draw uses the caller's index VA and BO */
   d.indexed = true; d.index_va = 0x1019d000; d.index_handle = 79;
   CHECK(prismrv_vk_gl_record_draw(&g, &d, &why) == 0 && prismrv_vk_gl_flush(&g, &why) == 0 && f.submits == 2, "indexed frame: %s", why);
   {
      unsigned k, h79 = 0;
      for (k = 0; k < f.last.num_bos; k++) h79 |= f.last_bos[k] == 79;
      CHECK(h79, "index BO missing from the submit");
   }
   {  /* the measured 'one colour varying + two attributes' shape is accepted, on its own context */
      struct fake f3; static struct prismrv_vk_gl g3; struct prismrv_vk_ops ops3 = ops; struct prismrv_vk_gl_draw x = d;
      memset(&f3, 0, sizeof f3); ops3.user = &f3;
      CHECK(prismrv_vk_gl_init(&g3, &ops3, 0x00108368, &why) == 0, "init g3: %s", why);
      x.n_attrs = 2; x.attr[1] = x.attr[0]; x.attr[1].va = 0x10198000; x.attr[1].handle = 79; x.vs_varyings = x.fs_varyings = 1;
      CHECK(prismrv_vk_gl_record_draw(&g3, &x, &why) == 0, "varying draw refused: %s", why);
      CHECK(prismrv_vk_gl_flush(&g3, &why) == 0, "varying flush: %s", why);
      CHECK(f3.submits == 1, "varying frame submits %u", f3.submits);
   }

   printf("gl glue: %d failures\n", fails);
   return fails ? 1 : 0;
}
