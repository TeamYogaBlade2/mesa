/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * prismrv_context.c — pipe_context implementation.
 */
#include "prismrv_context.h"

#include <sys/mman.h>

#include "pipe/p_defines.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util/u_blitter.h"
#include "util/u_memory.h"
#include "util/u_upload_mgr.h"
#include "util/ralloc.h"
#include "util/u_debug.h"

#include "prismrv_batch.h"
#include "prismrv_fence.h"
#include "prismrv_drmif.h"
#include "prismrv_resource.h"
#include "prismrv_program.h"

/* ---- flush ---------------------------------------------------------- */

static void
prismrv_context_flush(struct pipe_context *pctx,
                      struct pipe_fence_handle **fence, unsigned flags)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);
   int out_fd = -1;

   if (ctx->batch.cmd_size) {
      /* user bos[0] = TA packet-stream BO (kernel publishes its GPU VA
       * in CCB data[2]; see REVIEW R1/R2 resolution) */
      uint32_t bos[1] = { ctx->batch.ta_handle };
      int ret = prismrv_batch_submit(pctx, PRISMRV_CMD_TA,
                           ctx->batch.cmd_handle, ctx->batch.cmd_size,
                           ctx->batch.ta_handle ? bos : NULL,
                           ctx->batch.ta_handle ? 1 : 0, &out_fd);
      ctx->batch.cmd_size = 0;
      if (ret) {
         /* submit failed: no fence will ever signal.  Report through
          * the debug channel and make sure the caller doesn't get a
          * valid-looking fd from a failed ioctl. */
         debug_printf("prismrv: submit failed (%d)\n", ret);
         out_fd = -1;
      }
   }

   if (fence) {
      *fence = out_fd >= 0 ? prismrv_fence_create(out_fd) : NULL;
      if (out_fd >= 0 && !*fence)
         close(out_fd);
   } else if (out_fd >= 0) {
      /* caller does not want the fence: close the fd so it does not
       * leak (REVIEW R3) */
      close(out_fd);
   }
}


static void
prismrv_set_debug_callback(struct pipe_context *pctx,
                           const struct util_debug_callback *cb)
{
}

static void
prismrv_invalidate_resource(struct pipe_context *pctx,
                            struct pipe_resource *pres)
{
}

/* ---- draw ------------------------------------------------------------ */

/* ---- command stream construction ------------------------------------- *
 * Two-layer format shared with the Python UMD (libprismrv.py):
 *
 *   layer 1 (cmd BO):  [u32 opcode][u32 words][payload...]
 *     0 NOP          -
 *     1 SET_RT       {w(u32), h(u32)}
 *     2 SET_PROG_VS  {usse text, NUL-padded}
 *     3 SET_PROG_FS  {usse text, NUL-padded}
 *     4 SET_UNIFORMS {f32 x 4 per uniform}
 *     5 DRAW         {ta_va(u64), ta_len(u32), first(u32)} — ta_va is the
 *                    GPU VA of a BO holding the layer-2 TA packets
 *     6 BARRIER      -
 *     7 SET_TEXTURE  {slot(u32), w(u32), h(u32), gem_handle(u32)}
 *
 *   layer 2 (TA packet BO, parsed by the binner / ta_stage.py):
 *     [u32 opcode][u32 words][payload...] with opcodes
 *     1 VGT_STATE{mode} 2 INDEX_RANGE{first,count}
 *     3 VERTEX_ARRAY{ncomp,reserved,stride,floats...inline} 4 END
 */

static uint32_t
prismrv_pack_ta_packets(uint32_t *buf, uint32_t max_words,
                        const float *verts, unsigned nverts,
                        unsigned ncomp, unsigned mode)
{
   uint32_t off = 0;
   uint32_t nw = nverts * ncomp;

   /* bounds: VGT(3) + INDEX_RANGE(4) + VERTEX_ARRAY hdr(5) + data + END(2) */
   if (3 + 4 + 5 + nw + 2 > max_words)
      return 0;

   /* VGT_STATE: mode 0=points 1=lines 2=triangles (ta_stage.py) */
   buf[off++] = 1; buf[off++] = 1; buf[off++] = mode;
   /* INDEX_RANGE */
   buf[off++] = 2; buf[off++] = 2; buf[off++] = 0; buf[off++] = nverts;
   /* VERTEX_ARRAY: inline float data after the 3-word header */
   buf[off++] = 3; buf[off++] = 3 + nw;
   buf[off++] = ncomp; buf[off++] = 0; buf[off++] = ncomp;
   memcpy(buf + off, verts, nw * 4); off += nw;
   /* END */
   buf[off++] = 4; buf[off++] = 0;

   return off * 4;   /* byte length */
}

static void
prismrv_draw_vbo(struct pipe_context *pctx,
                 const struct pipe_draw_info *info,
                 unsigned drawid_offset,
                 const struct pipe_draw_indirect_info *indirect,
                 const struct pipe_draw_start_count_bias *draws,
                 unsigned num_draws)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);
   struct prismrv_screen *screen = ctx->screen;
   unsigned nverts, ncomp = 7;
   uint32_t ta_len;
   float *verts = NULL;

   bool is_tri = info->mode == MESA_PRIM_TRIANGLES;
   unsigned min_verts = is_tri ? 3 : (info->mode == MESA_PRIM_LINES ? 2 : 1);

   if ((info->mode != MESA_PRIM_TRIANGLES && info->mode != MESA_PRIM_LINES &&
        info->mode != MESA_PRIM_POINTS) || !ctx->num_vertex_elements)
      return;

   /* indexed draw: expand the index buffer into a vertex list first */
   const uint32_t *indices = NULL;
   unsigned index_size = 0;

   if (info->index_size) {
      if (info->has_user_indices)
         indices = (const uint32_t *)info->index.user;
      else if (info->index.resource)
         indices = (const uint32_t *)prismrv_resource_map(info->index.resource);
      if (!indices)
         return;
      index_size = info->index_size;
   }

   nverts = draws->count;
   if (nverts < min_verts)
      return;
   if (nverts > 256)
      nverts = 256;

   /* vertex data: positions and colors gathered from the bound vertex
    * buffer(s).  Element 0 feeds positions, element 1 (if bound) feeds
    * colours; both must be float3/float4 in the first vertex buffer. */
   {
      struct prismrv_vertex_element *pos_el = &ctx->vertex_elements[0];
      struct prismrv_vertex_element *col_el = ctx->num_vertex_elements > 1 ?
                                              &ctx->vertex_elements[1] : NULL;
      struct pipe_vertex_buffer *vb_pos =
         &ctx->vertex_buffers[pos_el->vertex_buffer_index];
      struct pipe_vertex_buffer *vb_col =
         col_el ? &ctx->vertex_buffers[col_el->vertex_buffer_index] : NULL;
      const uint8_t *base_pos = NULL, *base_col = NULL;

      if (vb_pos && vb_pos->buffer.resource)
         base_pos = prismrv_resource_map(vb_pos->buffer.resource);
      if (vb_col && vb_col->buffer.resource)
         base_col = prismrv_resource_map(vb_col->buffer.resource);
      if (!base_pos)
         return;
      if (col_el && !base_col)
         return;

      verts = calloc(nverts, ncomp * sizeof(float));
      if (!verts)
         return;

      for (unsigned v = 0; v < nverts; v++) {
         unsigned src = v;

         if (index_size == 4)
            src = indices[draws->start + v];
         else if (index_size == 2)
            src = ((const uint16_t *)indices)[draws->start + v];
         else if (index_size == 1)
            src = ((const uint8_t *)indices)[draws->start + v];

         const float *p = (const void *)(base_pos + vb_pos->buffer_offset +
                                         pos_el->src_offset +
                                         (size_t)src * pos_el->src_stride);
         const float *c = NULL;
         float cf[4] = { 0.f, 0.f, 0.f, 1.f };

         if (col_el && base_col) {
            c = (const void *)(base_col + vb_col->buffer_offset +
                               col_el->src_offset +
                               (size_t)src * col_el->src_stride);
            cf[0] = c[0]; cf[1] = c[1]; cf[2] = c[2]; cf[3] = c[3];
         }
         verts[v * ncomp + 0] = p[0];
         verts[v * ncomp + 1] = p[1];
         verts[v * ncomp + 2] = p[2];
         memcpy(&verts[v * ncomp + 3], cf, sizeof(cf));
      }
   }

   /* build the layer-2 TA packet stream into the TA BO */
   if (!ctx->batch.ta_handle) {
      ctx->batch.ta_capacity = 64 * 1024;
      ctx->batch.ta_handle =
         prismrv_drm_gem_create(screen->fd, ctx->batch.ta_capacity);
      ctx->batch.ta_map =
         prismrv_drm_gem_map(screen->fd, ctx->batch.ta_handle,
                             ctx->batch.ta_capacity);
   }
   if (!ctx->batch.ta_map || ctx->batch.ta_map == MAP_FAILED) {
      free(verts);
      return;
   }

   {
      unsigned mode = info->mode == MESA_PRIM_POINTS ? 0 :
                      info->mode == MESA_PRIM_LINES ? 1 : 2;

      ta_len = prismrv_pack_ta_packets(
         (uint32_t *)ctx->batch.ta_map, ctx->batch.ta_capacity / 4,
         verts, nverts, ncomp, mode);
      if (!ta_len) {
         free(verts);
         debug_printf("prismrv: TA packet stream does not fit\n");
         return;
      }
   }
   free(verts);
   /* clear anything past the new stream so a stale packet from an
    * earlier, longer draw can never be parsed */
   memset(ctx->batch.ta_map + ta_len, 0,
          ctx->batch.ta_capacity - ta_len);

      /* layer-1 stream in the cmd BO: SET_RT + SET_PROG_* + DRAW */
      {
         uint32_t *out = (uint32_t *)((uint8_t *)ctx->batch.cmd_map +
                                      ctx->batch.cmd_size);
         const struct pipe_framebuffer_state *fb = &ctx->framebuffer;
         uint32_t off = 0;
         unsigned prog_words;

         /* worst-case space check before writing: SET_RT + both shader
          * programs + uniforms + 8 textures + DRAW + BARRIER */
         {
            size_t need = 4 * 4   /* SET_RT is opcode+words+w+h */
               + (ctx->vs.usse_len ? 2 * 4 + ((ctx->vs.usse_len + 4) & ~3u) : 0)
               + (ctx->fs.usse_len ? 2 * 4 + ((ctx->fs.usse_len + 4) & ~3u) : 0)
               + ((ctx->num_vs_constants || ctx->num_fs_constants) ?
                 2 * 4 + (ctx->num_vs_constants + ctx->num_fs_constants) * 16 : 0)
               + 8 * 6 * 4
               + 5 * 4 + 2 * 4;
            if (ctx->batch.cmd_size + need > ctx->batch.cmd_capacity) {
               /* flush what we have and start a fresh command buffer */
               struct pipe_fence_handle *fence = NULL;
               prismrv_context_flush(pctx, &fence, 0);
               if (fence)
                  pctx->screen->fence_finish(pctx->screen, pctx, fence,
                                             UINT64_MAX);
               ctx->batch.cmd_size = 0;
               out = (uint32_t *)ctx->batch.cmd_map;
            }
         }

         /* SET_RT */
         out[off++] = 1; out[off++] = 2;
         out[off++] = fb->width; out[off++] = fb->height;

         /* bound vertex shader as USSE text (SET_PROG_VS) */
         if (ctx->vs.usse_text) {
            prog_words = (ctx->vs.usse_len + 4) / 4;
            out[off++] = 2; out[off++] = prog_words;
            memcpy(out + off, ctx->vs.usse_text, ctx->vs.usse_len + 1);
            memset((uint8_t *)out + off * 4 + ctx->vs.usse_len + 1,
                   0, prog_words * 4 - ctx->vs.usse_len - 1);
            off += prog_words;
         }

         /* bound fragment shader as USSE text (SET_PROG_FS) */
         if (ctx->fs.usse_text) {
            prog_words = (ctx->fs.usse_len + 4) / 4;
            out[off++] = 3; out[off++] = prog_words;
            memcpy(out + off, ctx->fs.usse_text, ctx->fs.usse_len + 1);
            memset((uint8_t *)out + off * 4 + ctx->fs.usse_len + 1,
                   0, prog_words * 4 - ctx->fs.usse_len - 1);
            off += prog_words;
         }

         /* uniforms: VS block first, then FS block (executor maps
          * them to r16.. per stage) */
         {
            unsigned nv = ctx->num_vs_constants;
            unsigned nf = ctx->num_fs_constants;
            if (nv || nf) {
               out[off++] = 4;
               out[off++] = nv * 4 + nf * 4;
               if (nv)
                  memcpy(out + off, ctx->vs_constants, nv * 16);
               off += nv * 4;
               if (nf)
                  memcpy(out + off, ctx->fs_constants, nf * 16);
               off += nf * 4;
            }
         }

         /* bound textures: {slot(u32), w, h, gem_handle} per view.
          * The executor maps the handle to pixels for `smp`. */
         for (unsigned t = 0; t < 8; t++) {
            struct prismrv_resource *tex = ctx->textures[t];

            if (!tex || !tex->cpu_map)
               continue;
            out[off++] = 7; out[off++] = 4;
            out[off++] = t;
            out[off++] = tex->base.width0;
            out[off++] = tex->base.height0;
            out[off++] = tex->gem_handle;
         }

         /* DRAW: userspace does not know GPU VAs (the kernel bump
          * allocator assigns them), so ta_va is left 0 here and the
          * kernel publishes the TA BO's GPU VA in CCB data[2]
          * (REVIEW R1/R2 resolution).  HookBackend-style executors
          * that own their VA space may patch the field instead. */
         out[off++] = 5; out[off++] = sizeof(uint64_t)/4 + 2;
         memset(out + off, 0, 8); off += 2;
         /* payload layout: ta_va(u64), ta_len(u32), first(u32).
          * ta_va=0 (kernel publishes the real VA in CCB data[2]);
          * ta_len MUST be the byte length of the TA stream — the
          * executor slices the TA BO with it.  The old code wrote
          * draws->start into this slot (and left 'first'
          * uninitialised), so every draw parsed as an empty stream. */
         out[off++] = ta_len;
         out[off++] = draws->start;
         /* BARRIER */
         out[off++] = 6; out[off++] = 0;

         ctx->batch.cmd_size += off * 4;
   }
}

static void
prismrv_set_framebuffer_state(struct pipe_context *pctx,
                              const struct pipe_framebuffer_state *state)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);

   util_copy_framebuffer_state(&ctx->framebuffer, state);
}

static void
prismrv_set_viewport_states(struct pipe_context *pctx,
                            unsigned start_slot,
                            unsigned num_viewports,
                            const struct pipe_viewport_state *states)
{
   (void)start_slot; (void)states;
}

static void
prismrv_set_scissor_states(struct pipe_context *pctx,
                           unsigned start_slot,
                           unsigned num_scissors,
                           const struct pipe_scissor_state *scissors)
{
}

/* ---- fixed-function state -------------------------------------------- */

#define BLEND_FACTOR(f) ((uint32_t)(f) & 0xf)
static void *
prismrv_create_blend_state(struct pipe_context *pctx,
                           const struct pipe_blend_state *tmpl)
{
   struct prismrv_blend_state *s = CALLOC_STRUCT(prismrv_blend_state);
   if (!s)
      return NULL;
   s->blend_enable = tmpl->rt[0].blend_enable;
   s->rgb_func = tmpl->rt[0].rgb_func;
   s->rgb_src = tmpl->rt[0].rgb_src_factor;
   s->rgb_dst = tmpl->rt[0].rgb_dst_factor;
   return s;
}

static void
prismrv_bind_blend_state(struct pipe_context *pctx, void *state)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);

   if (state)
      memcpy(&ctx->blend, state, sizeof(ctx->blend));
}

static void
prismrv_delete_blend_state(struct pipe_context *pctx, void *state)
{
   FREE(state);
}

static void *
prismrv_create_rasterizer_state(struct pipe_context *pctx,
                                const struct pipe_rasterizer_state *tmpl)
{
   struct prismrv_rasterizer_state *s =
      CALLOC_STRUCT(prismrv_rasterizer_state);
   if (!s)
      return NULL;
   s->scissor_enable = tmpl->scissor;
   s->cull_face = tmpl->cull_face;
   s->front_ccw = tmpl->front_ccw;
   return s;
}

static void
prismrv_bind_rasterizer_state(struct pipe_context *pctx, void *state)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);

   if (state)
      memcpy(&ctx->raster, state, sizeof(ctx->raster));
}

static void
prismrv_delete_rasterizer_state(struct pipe_context *pctx, void *state)
{
   FREE(state);
}

static void *
prismrv_create_depth_stencil_alpha_state(
   struct pipe_context *pctx,
   const struct pipe_depth_stencil_alpha_state *tmpl)
{
   struct prismrv_depth_stencil_alpha_state *s =
      CALLOC_STRUCT(prismrv_depth_stencil_alpha_state);
   if (!s)
      return NULL;
   s->depth_enabled = tmpl->depth_enabled;
   s->depth_writemask = tmpl->depth_writemask;
   s->depth_func = tmpl->depth_func;
   return s;
}

static void
prismrv_bind_depth_stencil_alpha_state(struct pipe_context *pctx, void *state)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);

   if (state)
      memcpy(&ctx->depth, state, sizeof(ctx->depth));
}

static void
prismrv_delete_depth_stencil_alpha_state(struct pipe_context *pctx, void *state)
{
   FREE(state);
}

static void
prismrv_set_constant_buffer(struct pipe_context *pctx,
                            mesa_shader_stage shader, uint index,
                            const struct pipe_constant_buffer *cb)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);

   /* one uniform slot per shader stage; both ship in SET_UNIFORMS
    * (VS block first, then FS block) with a word count covering both */
   if (index != 0 || !cb || !cb->buffer)
      return;

   {
      float *slot = (shader == MESA_SHADER_VERTEX) ?
                    ctx->vs_constants : ctx->fs_constants;
      unsigned *count = (shader == MESA_SHADER_VERTEX) ?
                        &ctx->num_vs_constants : &ctx->num_fs_constants;
      const float *data = prismrv_resource_map(cb->buffer);
      unsigned nvec4 = cb->buffer_size / 16;

      if (!data)
         return;
      if (nvec4 > ARRAY_SIZE(ctx->vs_constants) / 4)
         nvec4 = ARRAY_SIZE(ctx->vs_constants) / 4;
      memcpy(slot, data + cb->buffer_offset / 4, nvec4 * 16);
      *count = nvec4;
   }
}

static void
prismrv_sampler_view_destroy(struct pipe_context *pctx,
                             struct pipe_sampler_view *view)
{
   if (view->texture)
      pipe_resource_reference(&view->texture, NULL);
   FREE(view);
}

static struct pipe_sampler_view *
prismrv_create_sampler_view(struct pipe_context *pctx,
                            struct pipe_resource *pres,
                            const struct pipe_sampler_view *tmpl)
{
   struct prismrv_sampler_view *so = CALLOC_STRUCT(prismrv_sampler_view);
   if (!so)
      return NULL;

   so->base = *tmpl;
   so->base.texture = NULL;
   pipe_resource_reference(&so->base.texture, pres);
   so->base.reference.count = 1;
   so->base.context = pctx;
   return &so->base;
}

static void
prismrv_bind_sampler_states(struct pipe_context *pctx,
                            mesa_shader_stage shader,
                            unsigned start_slot, unsigned num_samplers,
                            void **samplers)
{
   /* sampler objects (filters/wrap) have no emulator equivalent:
    * smp is nearest+clamp only.  Accepted and ignored. */
}

static void *
prismrv_create_sampler_state(struct pipe_context *pctx,
                             const struct pipe_sampler_state *tmpl)
{
   return CALLOC_STRUCT(prismrv_blend_state); /* opaque cookie */
}

static void
prismrv_delete_sampler_state(struct pipe_context *pctx, void *state)
{
   FREE(state);
}

static void
prismrv_set_sampler_views(struct pipe_context *pctx,
                          mesa_shader_stage shader,
                          unsigned start_slot, unsigned num_views,
                          unsigned unbind_num_trailing_slots,
                          struct pipe_sampler_view **views)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);

   for (unsigned i = 0; i < num_views && start_slot + i < 8; i++) {
      struct prismrv_sampler_view *sv =
         views ? (struct prismrv_sampler_view *)views[i] : NULL;

      if (!sv || !sv->base.texture) {
         ctx->textures[start_slot + i] = NULL;
         continue;
      }
      ctx->textures[start_slot + i] =
         to_prismrv_resource(sv->base.texture);
      /* nearest sampling needs the CPU mapping of the texture BO */
      if (!ctx->textures[start_slot + i]->cpu_map)
         prismrv_resource_map(sv->base.texture);
   }
}

/* ---- lifecycle ------------------------------------------------------- */

struct pipe_context *
prismrv_context_create(struct pipe_screen *pscreen, void *priv,
                       unsigned flags)
{
   struct prismrv_screen *screen = to_prismrv_screen(pscreen);
   struct prismrv_context *ctx;

   ctx = rzalloc(NULL, struct prismrv_context);
   if (!ctx)
      return NULL;

   ctx->screen = screen;
   ctx->base.screen = pscreen;
   ctx->base.priv = priv;

   prismrv_context_init(ctx);

   /* command buffer for the layer-1 stream (SET_RT/DRAW/BARRIER) */
   prismrv_batch_init_context(ctx);
   if (!ctx->batch.cmd_handle || !ctx->batch.cmd_map ||
       ctx->batch.cmd_map == MAP_FAILED) {
      debug_printf("prismrv: failed to allocate the command buffer\n");
      ctx->base.destroy(&ctx->base);
      return NULL;
   }

   return &ctx->base;
}

static void
prismrv_context_destroy(struct pipe_context *pctx)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);

   /* blitter/uploader can be NULL when context creation failed early */
   if (ctx->blitter)
      util_blitter_destroy(ctx->blitter);
   if (ctx->uploader)
      u_upload_destroy(ctx->uploader);
   if (ctx->batch.cmd_map && ctx->batch.cmd_map != MAP_FAILED)
      munmap(ctx->batch.cmd_map, ctx->batch.cmd_capacity);
   if (ctx->batch.ta_map && ctx->batch.ta_map != MAP_FAILED)
      munmap(ctx->batch.ta_map, ctx->batch.ta_capacity);
   if (ctx->batch.cmd_handle) {
      /* GEM handles are per-context resources: close them so the
       * kernel can reclaim the BOs (drm close does this too, but be
       * explicit in case the fd is shared) */
      struct prismrv_screen *screen = ctx->screen;
      if (ctx->batch.ta_handle)
         prismrv_drm_gem_close(screen->fd, ctx->batch.ta_handle);
      prismrv_drm_gem_close(screen->fd, ctx->batch.cmd_handle);
   }

   /* the context itself is ralloc'd (see prismrv_context_create);
    * FREE() here would corrupt the heap */
   ralloc_free(ctx);
}

static void *
prismrv_create_vs_state(struct pipe_context *pctx,
                        const struct pipe_shader_state *tmpl)
{
   /* NIR is provided by st/mesa; store it for later USSE compilation */
   struct prismrv_shader_state *state = CALLOC_STRUCT(prismrv_shader_state);
   if (!state) return NULL;
   state->nir = tmpl->ir.nir;
   return state;
}

static void
prismrv_bind_vs_state(struct pipe_context *pctx, void *state)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);
   struct prismrv_shader_state *s = state;

   /* compile NIR → USSE first so the copy below carries the text
    * (the old order left ctx->vs.usse_text NULL until the next bind,
    * making the first draw run with the previous shader) */
   if (s && s->nir && !s->usse_text) {
      s->usse_text =
         prismrv_nir_to_usse(s, (nir_shader *)s->nir);
      s->usse_len = strlen(s->usse_text);
   }

   memcpy(&ctx->vs, state, sizeof(ctx->vs));
}

static void
prismrv_delete_vs_state(struct pipe_context *pctx, void *state)
{
   FREE(state);
}

static void *
prismrv_create_fs_state(struct pipe_context *pctx,
                        const struct pipe_shader_state *tmpl)
{
   struct prismrv_shader_state *state = CALLOC_STRUCT(prismrv_shader_state);
   if (!state) return NULL;
   state->nir = tmpl->ir.nir;
   return state;
}

static void
prismrv_bind_fs_state(struct pipe_context *pctx, void *state)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);
   struct prismrv_shader_state *s = state;

   if (s && s->nir && !s->usse_text) {
      s->usse_text =
         prismrv_nir_to_usse(s, (nir_shader *)s->nir);
      s->usse_len = strlen(s->usse_text);
   }

   memcpy(&ctx->fs, state, sizeof(ctx->fs));
}

static void
prismrv_delete_fs_state(struct pipe_context *pctx, void *state)
{
   FREE(state);
}

static void
prismrv_set_vertex_buffers(struct pipe_context *pctx,
                           unsigned count,
                           const struct pipe_vertex_buffer *buffers)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);
   unsigned i;

   /* count is the number of buffers in the array; buffers == NULL
    * unbinds everything */
   memset(ctx->vertex_buffers, 0, sizeof(ctx->vertex_buffers));
   ctx->num_vertex_buffers = 0;
   if (!buffers)
      return;
   for (i = 0; i < count && i < ARRAY_SIZE(ctx->vertex_buffers); i++) {
      if (!buffers[i].buffer.resource)
         continue;
      ctx->vertex_buffers[i] = buffers[i];
      ctx->num_vertex_buffers = i + 1;
   }
}

static void *
prismrv_create_vertex_elements(struct pipe_context *pctx,
                               unsigned num_elems,
                               const struct pipe_vertex_element *elems)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);
   ctx->num_vertex_elements = num_elems;
   for (unsigned i = 0; i < num_elems && i < 8; i++) {
      ctx->vertex_elements[i].src_offset = elems[i].src_offset;
      ctx->vertex_elements[i].src_format = elems[i].src_format;
      ctx->vertex_elements[i].vertex_buffer_index = elems[i].vertex_buffer_index;
      ctx->vertex_elements[i].src_stride = elems[i].src_stride;
   }
   return (void *)(uintptr_t)(num_elems | 1);  /* non-NULL cookie */
}

static void
prismrv_bind_vertex_elements(struct pipe_context *pctx, void *state)
{
}

static void
prismrv_delete_vertex_elements(struct pipe_context *pctx, void *state)
{
}

void
prismrv_context_init(struct prismrv_context *ctx)
{
   struct pipe_context *pctx = &ctx->base;

   pctx->destroy = prismrv_context_destroy;
   pctx->flush = prismrv_context_flush;
   pctx->set_debug_callback = prismrv_set_debug_callback;
   pctx->invalidate_resource = prismrv_invalidate_resource;
   pctx->draw_vbo = prismrv_draw_vbo;
   pctx->set_framebuffer_state = prismrv_set_framebuffer_state;
   pctx->set_viewport_states = prismrv_set_viewport_states;
   pctx->set_scissor_states = prismrv_set_scissor_states;
   pctx->set_constant_buffer = prismrv_set_constant_buffer;
   pctx->create_sampler_view = prismrv_create_sampler_view;
   pctx->sampler_view_destroy = prismrv_sampler_view_destroy;
   pctx->create_vs_state = prismrv_create_vs_state;
   pctx->bind_vs_state = prismrv_bind_vs_state;
   pctx->delete_vs_state = prismrv_delete_vs_state;
   pctx->create_fs_state = prismrv_create_fs_state;
   pctx->bind_fs_state = prismrv_bind_fs_state;
   pctx->delete_fs_state = prismrv_delete_fs_state;
   pctx->set_vertex_buffers = prismrv_set_vertex_buffers;
   pctx->create_vertex_elements_state = prismrv_create_vertex_elements;
   pctx->bind_vertex_elements_state = prismrv_bind_vertex_elements;
   pctx->delete_vertex_elements_state = prismrv_delete_vertex_elements;
   pctx->create_blend_state = prismrv_create_blend_state;
   pctx->bind_blend_state = prismrv_bind_blend_state;
   pctx->delete_blend_state = prismrv_delete_blend_state;
   pctx->create_rasterizer_state = prismrv_create_rasterizer_state;
   pctx->bind_rasterizer_state = prismrv_bind_rasterizer_state;
   pctx->delete_rasterizer_state = prismrv_delete_rasterizer_state;
   pctx->create_depth_stencil_alpha_state =
      prismrv_create_depth_stencil_alpha_state;
   pctx->bind_depth_stencil_alpha_state =
      prismrv_bind_depth_stencil_alpha_state;
   pctx->delete_depth_stencil_alpha_state =
      prismrv_delete_depth_stencil_alpha_state;
   pctx->create_sampler_state = prismrv_create_sampler_state;
   pctx->bind_sampler_states = prismrv_bind_sampler_states;
   pctx->delete_sampler_state = prismrv_delete_sampler_state;
   pctx->set_sampler_views = prismrv_set_sampler_views;

   ctx->uploader = u_upload_create_default(pctx);
   if (!ctx->uploader)
      return;
   pctx->stream_uploader = ctx->uploader;
   pctx->const_uploader = ctx->uploader;

   prismrv_fence_context_init(ctx);
   ctx->blitter = util_blitter_create(pctx);
}
