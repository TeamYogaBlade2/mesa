/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 */
#include "prismrv_vendor_gl.h"

#include <errno.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

static uint32_t
pack_unorm8(float f)
{
   if (!(f > 0.0f)) return 0;
   if (f >= 1.0f) return 255;
   return (uint32_t)(f * 255.0f + 0.5f);
}

/*
 * Third dword of the FS binding's PDS data slot (analysis/FS_BINDING_DW3.md).  Captured: with the colour-varying form (dw1 = 6) it is never 0 (13 of 13:
 * 0x2fc0000f, once 0x2f80000f); with the no-varying form it is 0 in all 63 kicks that carry the dw25 pair Mesa emits (family dw25 0x0e/0x0f), and 0 or
 * 0x2fc0000f in the other family.  Default: varying ? 0x2fc0000f : 0.  Its meaning is unknown and nothing else in the dumps flips together with it.
 * PRISMRV_VENDOR_FS_DW3=<hex> overrides it for both forms.
 */
static uint32_t
fs_bind_dw3(bool varying)
{
   const char *e = getenv("PRISMRV_VENDOR_FS_DW3");

   if (e && *e)
      return (uint32_t)strtoul(e, NULL, 16);
   return varying ? 0x2fc0000fu : 0;
}

int
prismrv_vk_gl_init(struct prismrv_vk_gl *g, const struct prismrv_vk_ops *ops, uint32_t hwrc_va, const char **why)
{
   int r;

   memset(g, 0, sizeof(*g));
   if (!hwrc_va && !ops->hwrc_create) { *why = "no HW render context address (PRISMRV_VENDOR_HWRC) and the kernel cannot create one (no HWRC_CREATE ioctl)"; return -EINVAL; }
   r = prismrv_vk_ctx_init(&g->vk, ops, why);
   if (r) return r;
   r = prismrv_vk_scene_create(&g->vk, &g->scene, why);
   if (r) return r;
   if (!hwrc_va) {
      /* vendor: the KM allocates the 0x54-byte context and patches the PD address into the user image at +0x08 */
      g->hwrc_handle = ops->hwrc_create(ops->user, PRISMRV_VK_HWRC_SIZE, PRISMRV_VK_HWRC_PD_OFFSET, &hwrc_va);
      if (!g->hwrc_handle) { *why = "DRM_PRISMRV_HWRC_CREATE failed (needs CAP_SYS_RAWIO and the kernel's vendor_abi)"; return -EIO; }
   }
   g->hwrc_va = hwrc_va;
   g->ready = true;
   return 0;
}

/*
 * A frame is one TA stream: a state block followed by [draw][pair][vertex PDS record] per draw.  Only the index range may differ between its draws
 * (measured: variants2/cfg7, multi-draw-pass.md), so a further draw is accepted only when every other field equals the first draw's.  Draws that
 * upload user-pointer data get a temporary BO each (different VAs), so they cannot be merged and are refused as "different state".
 */
static bool
same_frame_state(const struct prismrv_vk_gl_draw *a, const struct prismrv_vk_gl_draw *b)
{
   struct prismrv_vk_gl_draw x = *b;

   x.first = a->first; x.count = a->count; x.indexed = a->indexed; x.index_va = a->index_va; x.index_handle = a->index_handle;
   return memcmp(a, &x, sizeof(x)) == 0;
}

/* GL_TRIANGLES / GL_LINES index counts as the vendor UMD emits them (multi-draw-pass.md 3b): 3*floor(N/3), 2*floor(N/2) */
static unsigned
round_count(const struct prismrv_vk_gl_draw *d)
{
   return d->lines ? d->count - d->count % 2 : d->count - d->count % 3;
}

int
prismrv_vk_gl_record_draw(struct prismrv_vk_gl *g, const struct prismrv_vk_gl_draw *d, const char **why)
{
#define REJECT(m) do { *why = m; g->dropped++; return -ENOTSUP; } while (0)
   if (!g->ready) REJECT("vendor path not initialised");
   if (!d->triangles && !d->lines) REJECT("only triangle and line lists are supported");
   if (!d->rt_rgba8 || !d->rt_va) REJECT("render target must be an RGBA8 resource");
   if (d->has_zs) REJECT("only a Z16 depth surface is wired (no stencil surface: its address is in no captured kick)");
   if (d->has_depth && (!d->zs_va || !d->zs_handle)) REJECT("depth surface has no GPU address");
   if (d->stencil_test) REJECT("stencil test is not wired");
   if (d->alpha_test) REJECT("alpha test is not wired");
   if (d->scissor_partial) REJECT("a scissor rectangle that does not cover the target is not wired");
   if (d->unmapped_state) REJECT("culling with a clockwise front face / a viewport outside the target / a depth range outside [0,1] is not measured");
   if (d->vw <= 0 || d->vh <= 0) REJECT("empty viewport");
   if (d->blend) REJECT("blending is not wired");
   if (d->textures) REJECT("textures are not wired");
   if (d->vs_uniforms || d->fs_uniforms) REJECT("shader uniforms are not wired (VS constants go into the access block, FS ones into an unmeasured place)");
   if (!d->vs_code || !d->vs_words || !d->fs_code || !d->fs_words) REJECT("shader has no native USSE binary");
   if (d->n_attrs < 1 || d->n_attrs > PRISMRV_VK_GL_MAX_ATTRS) REJECT("1 or 2 vertex attributes only");
   for (unsigned i = 0; i < d->n_attrs; i++)
      if (!d->attr[i].is_float || d->attr[i].ncomp < 1 || d->attr[i].ncomp > 4 || !d->attr[i].va || !d->attr[i].stride)
         REJECT("vertex attributes must be 1..4 floats in a resource");
   /* the only measured shapes: no varying + one attribute, or one colour varying + two attributes (analysis/VS_FS_BINDING_SPLIT.md).
    * "does the VS write a varying" is derived from the shader interface; any shape that does not fit is refused, not approximated. */
   if (d->vs_varyings != d->fs_varyings) REJECT("the VS varying outputs and the FS varying inputs differ (unmeasured)");
   if (d->vs_varyings > 1) REJECT("more than one varying is not measured");
   if ((d->vs_varyings == 1) != (d->n_attrs == 2)) REJECT("only 'one varying + two attributes' or 'no varying + one attribute' are measured");
   {
      /* the vendor UMD drops a draw that cannot form one whole primitive; a partial trailing primitive is cut off */
      const unsigned n = round_count(d);

      if (n < (d->lines ? 2u : 3u)) REJECT("fewer indices than one primitive (the vendor UMD emits no draw record for it)");
      if (n > PRISMRV_VK_MAX_INDEX_COUNT) REJECT("more than 300 indices per draw (the draw word was only measured up to 300)");
   }
   if (d->indexed && !d->index_va) REJECT("index buffer must be a resource");
   if (d->count > 0xffff) REJECT("more than 65535 vertices/indices");
   if (g->have_draw) {
      if (g->n_more >= PRISMRV_VK_MAX_EXTRA_DRAWS) REJECT("more than four draws per frame (the emitter models three extra draws)");
      if (d->lines != g->draw.lines) REJECT("a primitive class change inside a frame needs the state-list-A re-emit object (not wired)");
      if (!same_frame_state(&g->draw, d)) REJECT("a further draw with different state: one kick carries one state block");
      g->more[g->n_more++] = *d;
      return 0;
   }
   g->draw = *d;
   g->n_more = 0;
   g->have_draw = true;
   return 0;
}

void
prismrv_vk_gl_record_clear(struct prismrv_vk_gl *g, float r, float gr, float b, float a)
{
   g->clear_colour = pack_unorm8(r) | pack_unorm8(gr) << 8 | pack_unorm8(b) << 16 | pack_unorm8(a) << 24;
   g->have_clear = true;
}

int
prismrv_vk_gl_flush(struct prismrv_vk_gl *g, const char **why)
{
   struct prismrv_vk_frame_in in;
   struct prismrv_vk_frame_result res;
   const struct prismrv_vk_gl_draw *d = &g->draw;
   unsigned i;
   int r = 0;

   if (!g->have_draw) {
      g->have_clear = false;
      return 0;
   }
   memset(&in, 0, sizeof(in));
   in.gl.width = in.gl.height = 0;
   in.gl.width = d->fb_w;
   in.gl.height = d->fb_h;
   in.gl.vx = d->vx;
   in.gl.vy = d->vy;
   in.gl.vw = d->vw;
   in.gl.vh = d->vh;
   in.gl.depth_near = d->depth_near;
   in.gl.depth_far = d->depth_far;
   in.gl.depth = d->has_depth;
   in.gl.depth_va = d->has_depth ? d->zs_va : 0;
   in.gl.depth_func = d->has_depth && d->depth_test ? d->depth_func : PRISMRV_VK_ALWAYS;
   in.gl.depth_write = d->has_depth && d->depth_write;
   in.gl.depth_test = d->has_depth && d->depth_test ? 1 : (d->has_depth ? 0 : -1);
   in.gl.cull = d->cull;
   in.gl.lines = d->lines;
   for (i = 0; i < 4; i++) in.gl.color_mask[i] = d->color_mask[i];
   in.gl.rt_va = d->rt_va;
   {  /* 3DREGISTERS dw25: session-dependent (0x0280640e / 0f / 1b / 1c / 1e in the captures); PRISMRV_VENDOR_3D_DW25=<hex> overrides the default */
      const char *e = getenv("PRISMRV_VENDOR_3D_DW25");

      in.gl.dw25 = e && *e ? (uint32_t)strtoul(e, NULL, 16) : 0;
   }
   in.lines = d->lines;

   in.code = d->fs_code;
   in.code_size = d->fs_words * 8;
   in.vs_code = d->vs_code;
   in.vs_code_size = d->vs_words * 8;
   /* varying: derived from the shader interface (record_draw only lets through the measured shapes).  UNVERIFIED: the FS record's
    * colour-varying form is the observed correlate of "the VS writes a varying" (it was never seen with a texture-coordinate varying);
    * its dw3 is the placeholder value of the kv10 captures */
   in.varying = d->vs_varyings == 1;
   in.colour_varying = in.varying;
   in.fs_bind_dw3 = fs_bind_dw3(in.varying);

   in.n_attrs = d->n_attrs;
   in.attr_va = d->attr[0].va;
   in.attr_dwords_m1 = d->attr[0].ncomp - 1;
   in.attr_stride = d->attr[0].stride;
   if (d->n_attrs == 2) {
      in.attr1_va = d->attr[1].va;
      in.attr1_dwords_m1 = d->attr[1].ncomp - 1;
      in.attr1_stride = d->attr[1].stride;
   }

   /* index data: the caller's u16 buffer, or 0,1,2,... generated for a non-indexed draw */
   for (i = 0; i <= g->n_more; i++) {
      const struct prismrv_vk_gl_draw *x = i ? &g->more[i - 1] : d;
      const unsigned n = round_count(x);
      uint32_t va;

      if (x->indexed) {
         va = x->index_va;
      } else {
         uint16_t *ix = (uint16_t *)prismrv_vk_arena_alloc(&g->vk.arena[PRISMRV_VK_ARENA_DATA], (n * 2 + 15) & ~15u, 16, &va);
         unsigned k;

         if (!ix) { *why = "data arena full"; r = -ENOSPC; goto out; }
         for (k = 0; k < n; k++) ix[k] = x->first + k;
      }
      if (i == 0) {
         in.index_va = va;
      } else {
         in.extra_draw[i - 1].index_va = va;
         in.extra_draw[i - 1].index_count = n;
      }
   }
   in.index_count = round_count(d);
   in.n_extra_draws = g->n_more;
   in.clear_w = (float)d->fb_w;
   in.clear_h = (float)d->fb_h;
   in.clear_colour = g->have_clear ? g->clear_colour : 0xff000000;
   in.access_auto = true;
   in.hwrc_va = g->hwrc_va;
   /* the epilogue / access words the caller owns carry userspace-driver values: zero */

   prismrv_vk_scene_fill_frame(&g->scene, &in);
   /* scene BOs + the resources this frame reads/writes */
   {
      uint32_t bos[SCENE_BO_COUNT + PRISMRV_VK_GL_MAX_EXTRA_BOS];
      unsigned n = 0;

      for (i = 0; i < SCENE_BO_COUNT; i++) bos[n++] = g->scene.handles[i];
      bos[n++] = d->rt_handle;
      if (d->has_depth && d->zs_handle) bos[n++] = d->zs_handle;
      if (g->hwrc_handle) bos[n++] = g->hwrc_handle;
      for (i = 0; i < d->n_attrs && n < SCENE_BO_COUNT + PRISMRV_VK_GL_MAX_EXTRA_BOS; i++) bos[n++] = d->attr[i].handle;
      for (i = 0; i <= g->n_more; i++) {
         const struct prismrv_vk_gl_draw *x = i ? &g->more[i - 1] : d;

         unsigned k; bool dup = false;

         for (k = SCENE_BO_COUNT; k < n; k++) dup |= bos[k] == x->index_handle;     /* the same index buffer used twice */
         if (x->indexed && !dup && n < SCENE_BO_COUNT + PRISMRV_VK_GL_MAX_EXTRA_BOS) bos[n++] = x->index_handle;
      }
      memcpy(g->extra, bos + SCENE_BO_COUNT, (n - SCENE_BO_COUNT) * 4);
      g->n_extra = n - SCENE_BO_COUNT;
      /* the scene handles lead; extra_bos must hold all of them in one array */
      static uint32_t all[SCENE_BO_COUNT + PRISMRV_VK_GL_MAX_EXTRA_BOS];
      memcpy(all, bos, n * 4);
      in.extra_bos = all;
      in.n_extra_bos = n;
   }

   r = prismrv_vk_ctx_frame(&g->vk, &in, &res, why);
out:
   g->have_draw = g->have_clear = false;
   g->n_more = 0;
   return r;
}
