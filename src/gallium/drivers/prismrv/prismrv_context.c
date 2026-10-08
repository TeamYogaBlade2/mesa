/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * prismrv_context.c — pipe_context implementation.
 *
 * Command stream ABI (PRISMRV_CMD_ABI_STREAM_V1)
 * ==============================================
 * This driver produces the PrismRV command stream, which is consumed by
 * the PrismRV executor (emulator UMD/uKernel, libprismrv.py).  The stock
 * vendor uKernel does not understand it, and the Linux kernel does not
 * interpret it: the kernel only maps the BOs of the job and forwards
 * their GPU virtual addresses.  All addresses inside the stream are GPU
 * VAs taken from GEM_CREATE (never GEM handles).
 *
 *   The kernel validates layer 1 (see the linux prismrv_stream.c): every
 *   address range must lie inside a BO listed in the submit, unknown
 *   opcodes are rejected.  Keep the two in sync.
 *
 *   layer 1 (cmd BO):  [u32 opcode][u32 words][payload...]
 *     0  NOP
 *     1  SET_RT        {w, h, gpu_va, stride_bytes, PRISMRV_FMT_*}
 *                      (gpu_va 0 / stride 0 = no render target)
 *     2  SET_PROG_VS   {usse text, NUL-padded}
 *     3  SET_PROG_FS   {usse text, NUL-padded}
 *     4  SET_UNIFORMS  {stage (0=VS,1=FS), f32 x 4 per uniform}; sent once
 *                      per stage, each stage's uniform 0 is its own r16
 *     5  DRAW          {ta_va lo, ta_va hi, ta_len, first}
 *     6  BARRIER
 *     7  SET_TEXTURE   {slot, w, h, gpu_va, stride_bytes, PRISMRV_FMT_*,
 *                       wrap_s, wrap_t, min_filter, mag_filter, mip_filter}
 *                      (sampler state as Gallium PIPE_TEX_* values; the
 *                       executor may implement only a subset, see README)
 *     8  SET_VIEWPORT  {f32 scale[3], f32 translate[3]}
 *     9  SET_SCISSOR   {enable, minx, miny, maxx, maxy}
 *    10  SET_BLEND     {enable, rgb_func, rgb_src, rgb_dst,
 *                       a_func, a_src, a_dst, colormask}
 *    11  SET_DEPTH     {test_enable, write_enable, func}
 *    12  SET_RASTER    {cull_face, front_ccw}
 *   Opcodes 8-12 are forwarded so the executor can implement them; an
 *   executor that does not know an opcode skips it.  Until it does, the
 *   corresponding GL state has no effect (see README).
 *
 *   layer 2 (TA packet BO, parsed by the binner / ta_stage.py):
 *     [u32 opcode][u32 words][payload...] with opcodes
 *     1 VGT_STATE{mode} 2 INDEX_RANGE{first,count}
 *     3 VERTEX_ARRAY{ncomp,reserved,stride,floats...inline} 4 END
 *   One vertex = num_elements x vec4 floats; element i is the VS input
 *   with Gallium input index i and is loaded to registers r[4i..4i+3].
 */
#include "prismrv_context.h"

#include <sys/mman.h>

#include "pipe/p_defines.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>

#include "util/u_blitter.h"
#include "util/u_memory.h"
#include "util/u_transfer.h"
#include "util/u_transfer_helper.h"
#include "util/u_upload_mgr.h"
#include "util/ralloc.h"
#include "util/u_debug.h"
#include "util/format/u_format.h"
#include "util/u_math.h"
#include "util/u_pack_color.h"
#include "util/u_inlines.h"
#include "util/format/u_format.h"
#include "util/half_float.h"
#include "compiler/nir/nir.h"

#include "prismrv_batch.h"
#include "prismrv_fence.h"
#include "prismrv_drmif.h"
#include "prismrv_resource.h"
#include "prismrv_program.h"

enum {
   OP_NOP = 0, OP_SET_RT, OP_SET_PROG_VS, OP_SET_PROG_FS, OP_SET_UNIFORMS,
   OP_DRAW, OP_BARRIER, OP_SET_TEXTURE, OP_SET_VIEWPORT, OP_SET_SCISSOR,
   OP_SET_BLEND, OP_SET_DEPTH, OP_SET_RASTER,
};

/* vertices per DRAW packet; keeps one TA stream small and bounded */
#define PRISMRV_MAX_CHUNK_VERTS 240

/* Gallium format -> stream surface code; -1 if the executor cannot use it */
static int
prismrv_stream_format(enum pipe_format f)
{
   switch (f) {
   case PIPE_FORMAT_R8G8B8A8_UNORM:
   case PIPE_FORMAT_R8G8B8X8_UNORM:
      return PRISMRV_FMT_RGBA8_UNORM;
   case PIPE_FORMAT_B8G8R8A8_UNORM:
   case PIPE_FORMAT_B8G8R8X8_UNORM:
      return PRISMRV_FMT_BGRA8_UNORM;
   case PIPE_FORMAT_R32G32B32A32_FLOAT:
      return PRISMRV_FMT_RGBA32F;
   default:
      return -1;
   }
}

/* ---- batch bookkeeping ----------------------------------------------- */

/* wait for the previous job before the CPU rewrites the cmd/TA BOs */
static bool
prismrv_batch_wait_prev(struct prismrv_context *ctx)
{
   struct pollfd pfd;
   int pr;
   bool done;

   if (ctx->batch.prev_fence_fd < 0)
      return true;

   pfd.fd = ctx->batch.prev_fence_fd;
   pfd.events = POLLIN;
   pfd.revents = 0;
   pr = poll(&pfd, 1, 5000);
   done = pr > 0 && (pfd.revents & (POLLIN | POLLERR | POLLHUP));
   if (!done) {
      debug_printf("prismrv: GPU wait failed (poll=%d revents=%x)\n", pr,
                   pfd.revents);
      return false;
   }
   {
      int st = prismrv_sync_file_status(ctx->batch.prev_fence_fd);

      close(ctx->batch.prev_fence_fd);
      ctx->batch.prev_fence_fd = -1;
      if (st < 0 && st != -EAGAIN) {
         /* the job was killed (GPU reset): what it should have written
          * is not there */
         debug_printf("prismrv: job finished with error %d\n", st);
         ctx->screen->device_resets++;
         return false;
      }
   }
   return true;
}

static void
prismrv_batch_add_bo(struct prismrv_context *ctx, uint32_t handle)
{
   for (unsigned i = 0; i < ctx->batch.num_bos; i++)
      if (ctx->batch.bos[i] == handle)
         return;
   if (ctx->batch.num_bos < PRISMRV_BATCH_MAX_BOS)
      ctx->batch.bos[ctx->batch.num_bos++] = handle;
}

/* ---- flush ------------------------------------------------------------ */

static void
prismrv_context_flush(struct pipe_context *pctx,
                      struct pipe_fence_handle **fence, unsigned flags)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);
   int out_fd = -1;

   if (ctx->batch.cmd_size) {
      int ret = prismrv_batch_submit(pctx, PRISMRV_CMD_TA,
                                     ctx->batch.cmd_handle,
                                     ctx->batch.cmd_size,
                                     ctx->batch.bos, ctx->batch.num_bos,
                                     &out_fd);
      ctx->batch.cmd_size = 0;
      ctx->batch.ta_used_offset = 0;
      ctx->batch.num_bos = 0;
      ctx->dirty = PRISMRV_DIRTY_ALL;

      if (ret) {
         debug_printf("prismrv: submit failed (%d) — context lost\n", ret);
         ctx->context_lost = true;
         out_fd = -1;
      } else if (out_fd >= 0) {
         /* keep a dup'd copy for the next batch to wait on */
         int dup_fd = dup(out_fd);
         if (dup_fd < 0) {
            debug_printf("prismrv: dup(fence) failed — context lost\n");
            ctx->context_lost = true;
         } else {
            if (ctx->batch.prev_fence_fd >= 0)
               close(ctx->batch.prev_fence_fd);
            ctx->batch.prev_fence_fd = dup_fd;
         }
      }
   }

   if (fence) {
      *fence = (out_fd >= 0) ? prismrv_fence_create(out_fd) : NULL;
      if (out_fd >= 0 && !*fence)
         close(out_fd);
   } else if (out_fd >= 0) {
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

/* ---- vertex fetch (CPU gather into the TA stream) ------------------- */

static float
prismrv_fetch_component(const uint8_t *src, enum pipe_format fmt,
                        unsigned comp)
{
   switch (util_format_get_component_bits(fmt, UTIL_FORMAT_COLORSPACE_RGB,
                                          comp)) {
   default:
      break;
   }
   /* Dispatch by the format's channel type */
   const struct util_format_description *desc = util_format_description(fmt);
   if (comp >= desc->nr_channels)
      return comp == 3 ? 1.0f : 0.0f;

   const struct util_format_channel_description *ch = &desc->channel[comp];
   unsigned bits = ch->size;
   const uint8_t *p = src + ch->shift / 8;

   switch (ch->type) {
   case UTIL_FORMAT_TYPE_FLOAT:
      if (bits == 32) return *(const float *)p;
      if (bits == 16) return _mesa_half_to_float(*(const uint16_t *)p);
      break;
   case UTIL_FORMAT_TYPE_UNSIGNED:
      if (ch->normalized) {
         if (bits == 8)  return (float)(*p) / 255.0f;
         if (bits == 16) return (float)(*(const uint16_t *)p) / 65535.0f;
      } else {
         if (bits == 8)  return (float)(*p);
         if (bits == 16) return (float)(*(const uint16_t *)p);
      }
      break;
   case UTIL_FORMAT_TYPE_SIGNED:
      if (ch->normalized) {
         if (bits == 8)  return MAX2((float)(*(const int8_t *)p) / 127.0f, -1.0f);
         if (bits == 16) return MAX2((float)(*(const int16_t *)p) / 32767.0f, -1.0f);
      } else {
         if (bits == 8)  return (float)(*(const int8_t *)p);
         if (bits == 16) return (float)(*(const int16_t *)p);
      }
      break;
   default:
      break;
   }
   /* packed formats (RGB565, RGB10A2, etc.) use ch->shift that is not
    * byte-aligned; 'shift/8' above would read from the wrong byte and
    * extract garbage.  Return 0 rather than silently misread.
    * These formats should be rejected by the state tracker before
    * reaching here (is_format_supported returns false for them). */
   return 0.0f;
}

/*
 * Fetch a full vec4 (up to 4 components) from a vertex buffer for
 * vertex element @el.  Missing components default to (0,0,0,1).
 */
static void
prismrv_fetch_vertex_attrib(float out[4], const uint8_t *base,
                            const struct prismrv_vertex_element *el,
                            unsigned vertex_index)
{
   out[0] = 0.f; out[1] = 0.f; out[2] = 0.f; out[3] = 1.f;
   if (!base)
      return;

   const uint8_t *src = base + el->src_offset +
                        (size_t)vertex_index * el->src_stride;
   unsigned ncomp_fmt = util_format_get_nr_components(el->src_format);

   for (unsigned c = 0; c < ncomp_fmt && c < 4; c++)
      out[c] = prismrv_fetch_component(src, el->src_format, c);
}


static uint32_t
prismrv_pack_ta_packets(uint32_t *buf, uint32_t max_words,
                        const float *verts, unsigned nverts,
                        unsigned ncomp, unsigned mode)
{
   uint32_t off = 0;
   uint32_t nw = nverts * ncomp;

   /* VGT(3) + INDEX_RANGE(4) + VERTEX_ARRAY hdr(5) + data + END(2) */
   if (3 + 4 + 5 + nw + 2 > max_words)
      return 0;

   /* VGT_STATE: mode 0=points 1=lines 2=triangles (ta_stage.py) */
   buf[off++] = 1; buf[off++] = 1; buf[off++] = mode;
   buf[off++] = 2; buf[off++] = 2; buf[off++] = 0; buf[off++] = nverts;
   buf[off++] = 3; buf[off++] = 3 + nw;
   buf[off++] = ncomp; buf[off++] = 0; buf[off++] = ncomp;
   memcpy(buf + off, verts, nw * 4); off += nw;
   buf[off++] = 4; buf[off++] = 0;

   return off * 4;   /* byte length */
}

/* words needed by the state block for the current dirty set */
static size_t
prismrv_state_words(const struct prismrv_context *ctx)
{
   size_t n = 2 + 5;                    /* SET_RT */
   if (ctx->vs && ctx->vs->usse_text)
      n += 2 + (ctx->vs->usse_len + 4) / 4;
   if (ctx->fs && ctx->fs->usse_text)
      n += 2 + (ctx->fs->usse_len + 4) / 4;
   n += 2 * (2 + 1) + (ctx->num_vs_constants + ctx->num_fs_constants) * 4;
   n += 8 * (2 + 11);                   /* textures */
   n += (2 + 6) + (2 + 5) + (2 + 8) + (2 + 3) + (2 + 2);
   return n;
}

static void
prismrv_emit_program(uint32_t *out, unsigned *off, unsigned opcode,
                     const struct prismrv_shader_state *s)
{
   unsigned words = (s->usse_len + 4) / 4;

   out[(*off)++] = opcode;
   out[(*off)++] = words;
   out[*off + words - 1] = 0;           /* NUL padding */
   memcpy(out + *off, s->usse_text, s->usse_len);
   *off += words;
}

/* write all dirty state; returns words written */
static unsigned
prismrv_emit_state(struct prismrv_context *ctx, uint32_t *out)
{
   const struct pipe_framebuffer_state *fb = &ctx->framebuffer;
   unsigned off = 0;

   /* SET_RT: geometry plus the render target BO (written by the job) */
   out[off++] = OP_SET_RT; out[off++] = 5;
   out[off++] = fb->width; out[off++] = fb->height;
   /*
    * The attachment is a pipe_surface (a view: mip level, format), not the
    * bare resource.  Use the level's own address, size and stride.
    */
   const struct pipe_surface *cb = fb->nr_cbufs ? &fb->cbufs[0] : NULL;
   int rt_fmt = (cb && cb->texture) ? prismrv_stream_format(cb->format) : -1;

   if (rt_fmt >= 0) {
      struct prismrv_resource *rt = to_prismrv_resource(cb->texture);
      unsigned lw = prismrv_resource_level_dim(rt->base.width0, cb->level);

      out[off++] = prismrv_resource_level_va(rt, cb->level);
      out[off++] = lw * util_format_get_blocksize(cb->format);
      out[off++] = rt_fmt;
      prismrv_batch_add_bo(ctx, rt->gem_handle);
   } else {
      out[off++] = 0; out[off++] = 0; out[off++] = PRISMRV_FMT_RGBA8_UNORM;
   }

   if (ctx->vs && ctx->vs->usse_text)
      prismrv_emit_program(out, &off, OP_SET_PROG_VS, ctx->vs);
   if (ctx->fs && ctx->fs->usse_text)
      prismrv_emit_program(out, &off, OP_SET_PROG_FS, ctx->fs);

   if (ctx->num_vs_constants) {
      out[off++] = OP_SET_UNIFORMS;
      out[off++] = 1 + ctx->num_vs_constants * 4;
      out[off++] = 0;                       /* stage: vertex */
      memcpy(out + off, ctx->vs_constants, ctx->num_vs_constants * 16);
      off += ctx->num_vs_constants * 4;
   }
   if (ctx->num_fs_constants) {
      out[off++] = OP_SET_UNIFORMS;
      out[off++] = 1 + ctx->num_fs_constants * 4;
      out[off++] = 1;                       /* stage: fragment */
      memcpy(out + off, ctx->fs_constants, ctx->num_fs_constants * 16);
      off += ctx->num_fs_constants * 4;
   }

   /* textures carry the BO's GPU VA plus stride and format: the
    * executor needs both to decode the texels */
   for (unsigned t = 0; t < 8; t++) {
      struct prismrv_resource *tex = to_prismrv_resource(ctx->textures[t]);
      int fmt;

      if (!tex)
         continue;
      unsigned lvl = MIN2(ctx->tex_level[t], (unsigned)tex->base.last_level);
      unsigned tw = prismrv_resource_level_dim(tex->base.width0, lvl);
      unsigned th = prismrv_resource_level_dim(tex->base.height0, lvl);

      fmt = prismrv_stream_format(ctx->tex_format[t]);
      if (fmt < 0)
         continue;
      out[off++] = OP_SET_TEXTURE; out[off++] = 11;
      out[off++] = t;
      out[off++] = tw;
      out[off++] = th;
      out[off++] = prismrv_resource_level_va(tex, lvl);
      out[off++] = tw * util_format_get_blocksize(ctx->tex_format[t]);
      out[off++] = fmt;
      out[off++] = ctx->samplers[t].wrap_s;
      out[off++] = ctx->samplers[t].wrap_t;
      out[off++] = ctx->samplers[t].min_img_filter;
      out[off++] = ctx->samplers[t].mag_img_filter;
      out[off++] = ctx->samplers[t].min_mip_filter;
      prismrv_batch_add_bo(ctx, tex->gem_handle);
   }

   {
      float vp[6] = { 1.f, 1.f, 1.f, 0.f, 0.f, 0.f };
      unsigned k;

      if (ctx->viewport_valid) {
         memcpy(vp, ctx->viewport.scale, 12);
         memcpy(vp + 3, ctx->viewport.translate, 12);
      } else {
         vp[0] = fb->width * 0.5f;  vp[1] = fb->height * 0.5f;  vp[2] = 0.5f;
         vp[3] = fb->width * 0.5f;  vp[4] = fb->height * 0.5f;  vp[5] = 0.5f;
      }
      out[off++] = OP_SET_VIEWPORT; out[off++] = 6;
      for (k = 0; k < 6; k++)
         memcpy(&out[off++], &vp[k], 4);
   }

   {
      const struct pipe_scissor_state *sc = &ctx->scissors[0];
      bool en = ctx->raster.scissor_enable;

      out[off++] = OP_SET_SCISSOR; out[off++] = 5;
      out[off++] = en;
      out[off++] = en ? sc->minx : 0;
      out[off++] = en ? sc->miny : 0;
      out[off++] = en ? sc->maxx : fb->width;
      out[off++] = en ? sc->maxy : fb->height;
   }

   out[off++] = OP_SET_BLEND; out[off++] = 8;
   out[off++] = ctx->blend.blend_enable;
   out[off++] = ctx->blend.rgb_func;
   out[off++] = ctx->blend.rgb_src;
   out[off++] = ctx->blend.rgb_dst;
   out[off++] = ctx->blend.alpha_func;
   out[off++] = ctx->blend.alpha_src;
   out[off++] = ctx->blend.alpha_dst;
   out[off++] = ctx->blend.colormask;

   out[off++] = OP_SET_DEPTH; out[off++] = 3;
   out[off++] = ctx->depth.depth_enabled;
   out[off++] = ctx->depth.depth_writemask;
   out[off++] = ctx->depth.depth_func;

   out[off++] = OP_SET_RASTER; out[off++] = 2;
   out[off++] = ctx->raster.cull_face;
   out[off++] = ctx->raster.front_ccw;

   ctx->dirty = 0;
   return off;
}

/*
 * Make room for @words command words and @ta_bytes of TA stream.  If the
 * batch is full it is flushed and waited for, then retried once; only a
 * request that cannot fit an empty batch fails.
 */
static bool
prismrv_reserve(struct prismrv_context *ctx, size_t words, size_t ta_bytes)
{
   struct pipe_context *pctx = &ctx->base;
   size_t cmd_bytes = words * 4;

   if (cmd_bytes > ctx->batch.cmd_capacity ||
       ta_bytes > ctx->batch.ta_capacity)
      return false;

   if (ctx->batch.cmd_size + cmd_bytes > ctx->batch.cmd_capacity ||
       ctx->batch.ta_used_offset + ta_bytes > ctx->batch.ta_capacity) {
      prismrv_context_flush(pctx, NULL, 0);
      if (ctx->context_lost || !prismrv_batch_wait_prev(ctx))
         return false;
      /* the fresh batch must resend everything, so the caller has to
       * recompute the words it needs: signalled by returning true with
       * an empty batch (cmd_size == 0) */
   }
   return true;
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
   unsigned mode, min_verts, quantum;

   if (ctx->context_lost)
      return;

   /* none of these are advertised in the caps; refuse loudly rather
    * than render something different from what was asked */
   if (indirect || info->primitive_restart || info->instance_count > 1 ||
       info->start_instance) {
      debug_printf("prismrv: unsupported draw (indirect/restart/instancing)\n");
      return;
   }

   switch (info->mode) {
   case MESA_PRIM_POINTS:    mode = 0; min_verts = 1; quantum = 1; break;
   case MESA_PRIM_LINES:     mode = 1; min_verts = 2; quantum = 2; break;
   case MESA_PRIM_TRIANGLES: mode = 2; min_verts = 3; quantum = 3; break;
   default:
      debug_printf("prismrv: unsupported primitive %d\n", info->mode);
      return;
   }

   if (!ctx->num_vertex_elements || !ctx->vs || !ctx->vs->usse_text ||
       !ctx->fs || !ctx->fs->usse_text)
      return;   /* shader failed to compile, or nothing bound */

   /* the TA BO is created lazily */
   if (!ctx->batch.ta_handle) {
      ctx->batch.ta_capacity = 256 * 1024;
      ctx->batch.ta_handle = prismrv_drm_gem_create(screen->fd,
                                                    ctx->batch.ta_capacity,
                                                    &ctx->batch.ta_gpu_va);
      ctx->batch.ta_map = ctx->batch.ta_handle ?
         prismrv_drm_gem_map(screen->fd, ctx->batch.ta_handle,
                             ctx->batch.ta_capacity) : NULL;
      ctx->batch.ta_used_offset = 0;
   }
   if (!ctx->batch.ta_map || ctx->batch.ta_map == MAP_FAILED) {
      ctx->context_lost = true;
      return;
   }

   /* index buffer */
   const void *indices = NULL;
   unsigned index_size = info->index_size;

   if (index_size) {
      indices = info->has_user_indices ? info->index.user :
         (info->index.resource ?
          prismrv_resource_map(info->index.resource) : NULL);
      if (!indices || (index_size != 1 && index_size != 2 &&
                       index_size != 4))
         return;
   }

   /* per-element source pointers */
   const uint8_t *base[PRISMRV_MAX_VERTEX_ELEMENTS];
   unsigned nel = MIN2(ctx->num_vertex_elements, PRISMRV_MAX_VERTEX_ELEMENTS);
   for (unsigned e = 0; e < nel; e++) {
      unsigned vbi = ctx->vertex_elements[e].vertex_buffer_index;
      const struct pipe_vertex_buffer *vb;

      if (vbi >= ARRAY_SIZE(ctx->vertex_buffers))
         return;
      vb = &ctx->vertex_buffers[vbi];
      if (vb->is_user_buffer)
         base[e] = ctx->user_vertex_buffers[vbi];
      else if (vb->buffer.resource)
         base[e] = prismrv_resource_map(vb->buffer.resource);
      else
         base[e] = NULL;
      if (!base[e])
         return;
      base[e] += vb->buffer_offset;
   }
   /* one vertex = nel vec4; without a colour element the fixed-function
    * path of the executor reads v[4..6], so pad to two elements */
   unsigned nvec = MAX2(nel, 2u);
   unsigned ncomp = nvec * 4;

   if (!prismrv_batch_wait_prev(ctx) && ctx->batch.cmd_size == 0)
      return;

   for (unsigned d = 0; d < num_draws; d++) {
      unsigned start = draws[d].start, count = draws[d].count;
      int bias = index_size ? draws[d].index_bias : 0;

      /* draw in whole primitives, at most PRISMRV_MAX_CHUNK_VERTS each */
      count -= count % quantum;
      for (unsigned done = 0; done < count;) {
         unsigned n = MIN2(count - done, PRISMRV_MAX_CHUNK_VERTS);
         float *verts;
         uint32_t ta_len;

         n -= n % quantum;
         if (n < min_verts)
            break;

         verts = malloc((size_t)n * ncomp * sizeof(float));
         if (!verts)
            return;

         for (unsigned v = 0; v < n; v++) {
            unsigned idx = start + done + v;
            long src = idx;

            if (index_size == 4)
               src = ((const uint32_t *)indices)[idx];
            else if (index_size == 2)
               src = ((const uint16_t *)indices)[idx];
            else if (index_size == 1)
               src = ((const uint8_t *)indices)[idx];
            src += bias;
            if (src < 0)
               src = 0;

            for (unsigned e = 0; e < nvec; e++) {
               float *dst = verts + (size_t)v * ncomp + e * 4;

               if (e < nel)
                  prismrv_fetch_vertex_attrib(dst, base[e],
                                              &ctx->vertex_elements[e], src);
               else
                  dst[0] = dst[1] = dst[2] = dst[3] = 1.0f;
            }
         }

         /* make room; a flush inside resets the batch and the state */
         size_t ta_need = ((size_t)(3 + 4 + 5 + 2) + n * ncomp) * 4;
         size_t words = prismrv_state_words(ctx) + 2 + 4 + 2;
         if (!prismrv_reserve(ctx, words, ta_need)) {
            free(verts);
            debug_printf("prismrv: draw does not fit an empty batch\n");
            ctx->context_lost = true;
            return;
         }

         ta_len = prismrv_pack_ta_packets(
            (uint32_t *)(ctx->batch.ta_map + ctx->batch.ta_used_offset),
            (ctx->batch.ta_capacity - ctx->batch.ta_used_offset) / 4,
            verts, n, ncomp, mode);
         free(verts);
         if (!ta_len) {
            ctx->context_lost = true;
            return;
         }

         /* first packet of a job resets the BO list: TA BO first */
         if (ctx->batch.num_bos == 0)
            prismrv_batch_add_bo(ctx, ctx->batch.ta_handle);

         uint32_t *out = (uint32_t *)(ctx->batch.cmd_map + ctx->batch.cmd_size);
         unsigned off = 0;

         if (ctx->dirty || ctx->batch.cmd_size == 0)
            off += prismrv_emit_state(ctx, out);

         uint64_t ta_va = (uint64_t)ctx->batch.ta_gpu_va +
                          ctx->batch.ta_used_offset;
         out[off++] = OP_DRAW; out[off++] = 4;
         out[off++] = (uint32_t)ta_va;
         out[off++] = (uint32_t)(ta_va >> 32);
         out[off++] = ta_len;
         out[off++] = 0;
         out[off++] = OP_BARRIER; out[off++] = 0;

         ctx->batch.cmd_size += off * 4;
         ctx->batch.ta_used_offset =
            (ctx->batch.ta_used_offset + ta_len + 3) & ~3u;
         done += n;
      }
   }
}

/* ---- framebuffer / viewport / scissor ------------------------------- */

static void
prismrv_set_framebuffer_state(struct pipe_context *pctx,
                              const struct pipe_framebuffer_state *state)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);

   /* the job in progress renders into the old target: submit it first */
   if (ctx->batch.cmd_size)
      prismrv_context_flush(pctx, NULL, 0);
   util_copy_framebuffer_state(&ctx->framebuffer, state);
   ctx->dirty = PRISMRV_DIRTY_ALL;
}

static void
prismrv_set_viewport_states(struct pipe_context *pctx,
                            unsigned start_slot, unsigned num_viewports,
                            const struct pipe_viewport_state *states)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);

   /* max_viewports == 1: only slot 0 exists */
   if (start_slot == 0 && num_viewports >= 1 && states) {
      ctx->viewport = states[0];
      ctx->viewport_valid = true;
      ctx->dirty = PRISMRV_DIRTY_ALL;
   }
}

static void
prismrv_set_scissor_states(struct pipe_context *pctx,
                           unsigned start_slot, unsigned num_scissors,
                           const struct pipe_scissor_state *scissors)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);

   for (unsigned i = 0; i < num_scissors &&
                        start_slot + i < PRISMRV_MAX_VIEWPORTS; i++)
      ctx->scissors[start_slot + i] = scissors[i];
   ctx->dirty = PRISMRV_DIRTY_ALL;
}

/* ---- fixed-function CSOs --------------------------------------------- */

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
   s->alpha_func = tmpl->rt[0].alpha_func;
   s->alpha_src = tmpl->rt[0].alpha_src_factor;
   s->alpha_dst = tmpl->rt[0].alpha_dst_factor;
   s->colormask = tmpl->rt[0].colormask;
   return s;
}

static void
prismrv_bind_blend_state(struct pipe_context *pctx, void *state)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);

   if (state)
      memcpy(&ctx->blend, state, sizeof(ctx->blend));
   else
      memset(&ctx->blend, 0, sizeof(ctx->blend));
   ctx->dirty = PRISMRV_DIRTY_ALL;
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
   else
      memset(&ctx->raster, 0, sizeof(ctx->raster));
   ctx->dirty = PRISMRV_DIRTY_ALL;
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
   else
      memset(&ctx->depth, 0, sizeof(ctx->depth));
   ctx->dirty = PRISMRV_DIRTY_ALL;
}

/* generic delete for the plain-old-data CSOs above */
static void
prismrv_delete_cso(struct pipe_context *pctx, void *state)
{
   FREE(state);
}

/* ---- constants ------------------------------------------------------- */

static void
prismrv_set_constant_buffer(struct pipe_context *pctx,
                            mesa_shader_stage shader, uint index,
                            const struct pipe_constant_buffer *cb)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);
   float *slot;
   unsigned *count;
   const uint8_t *data = NULL;
   unsigned nvec4;

   if (index != 0 || (shader != MESA_SHADER_VERTEX &&
                      shader != MESA_SHADER_FRAGMENT))
      return;

   slot = shader == MESA_SHADER_VERTEX ? ctx->vs_constants : ctx->fs_constants;
   count = shader == MESA_SHADER_VERTEX ? &ctx->num_vs_constants :
                                          &ctx->num_fs_constants;

   if (cb) {
      if (cb->user_buffer)
         data = cb->user_buffer;
      else if (cb->buffer)
         data = prismrv_resource_map(cb->buffer);
   }
   if (!data) {
      *count = 0;
      ctx->dirty = PRISMRV_DIRTY_ALL;
      return;
   }
   data += cb->buffer_offset;

   /* the backend can address only PRISMRV_MAX_UNIFORM_VEC4 vec4 */
   nvec4 = MIN2(cb->buffer_size / 16, (unsigned)PRISMRV_MAX_UNIFORM_VEC4);
   memcpy(slot, data, nvec4 * 16);
   *count = nvec4;
   ctx->dirty = PRISMRV_DIRTY_ALL;
}

/* ---- samplers -------------------------------------------------------- */

static void
prismrv_sampler_view_destroy(struct pipe_context *pctx,
                             struct pipe_sampler_view *view)
{
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
   pipe_reference_init(&so->base.reference, 1);
   so->base.context = pctx;
   return &so->base;
}

static void
prismrv_bind_sampler_states(struct pipe_context *pctx,
                            mesa_shader_stage shader,
                            unsigned start_slot, unsigned num_samplers,
                            void **samplers)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);

   if (shader != MESA_SHADER_FRAGMENT)
      return;   /* the vertex stage has no samplers (caps) */
   for (unsigned i = 0; i < num_samplers; i++) {
      unsigned slot = start_slot + i;

      if (slot >= ARRAY_SIZE(ctx->samplers))
         break;
      if (samplers && samplers[i])
         ctx->samplers[slot] = *(struct prismrv_sampler_state *)samplers[i];
      else
         memset(&ctx->samplers[slot], 0, sizeof(ctx->samplers[slot]));
   }
   ctx->dirty = PRISMRV_DIRTY_ALL;
}

static void *
prismrv_create_sampler_state(struct pipe_context *pctx,
                             const struct pipe_sampler_state *tmpl)
{
   struct prismrv_sampler_state *s = CALLOC_STRUCT(prismrv_sampler_state);

   if (!s)
      return NULL;
   s->wrap_s = tmpl->wrap_s;
   s->wrap_t = tmpl->wrap_t;
   s->min_img_filter = tmpl->min_img_filter;
   s->mag_img_filter = tmpl->mag_img_filter;
   s->min_mip_filter = tmpl->min_mip_filter;
   return s;
}

static void
prismrv_set_sampler_views(struct pipe_context *pctx,
                          mesa_shader_stage shader,
                          unsigned start_slot, unsigned num_views,
                          unsigned unbind_num_trailing_slots,
                          struct pipe_sampler_view **views)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);

   /*
    * Textures are only supported in the fragment stage (the VS caps
    * advertise zero samplers); views for other stages are released.
    * A single shared array with per-stage semantics used to let a VS
    * binding overwrite the FS one.
    */
   for (unsigned i = 0; i < num_views; i++) {
      struct pipe_sampler_view *sv = views ? views[i] : NULL;
      unsigned slot = start_slot + i;

      if (shader != MESA_SHADER_FRAGMENT || slot >= ARRAY_SIZE(ctx->textures))
         continue;
      pipe_resource_reference(&ctx->textures[slot],
                              sv ? sv->texture : NULL);
      if (sv) {
         /* a texture view = (level, format); both are honoured in the
          * stream.  Swizzles are lowered into the shader by the state
          * tracker (caps.texture_swizzle is off); layers do not exist. */
         ctx->tex_level[slot] = sv->u.tex.first_level;
         ctx->tex_format[slot] = sv->format;
      }
   }

   if (shader == MESA_SHADER_FRAGMENT) {
      for (unsigned i = 0; i < unbind_num_trailing_slots; i++) {
         unsigned slot = start_slot + num_views + i;

         if (slot < ARRAY_SIZE(ctx->textures))
            pipe_resource_reference(&ctx->textures[slot], NULL);
      }
   }
   ctx->dirty = PRISMRV_DIRTY_ALL;
}

/* ---- shaders --------------------------------------------------------- */

static struct prismrv_shader_state *
prismrv_create_shader_state(const struct pipe_shader_state *tmpl)
{
   struct prismrv_shader_state *state = CALLOC_STRUCT(prismrv_shader_state);
   nir_shader *nir;

   if (!state)
      return NULL;

   if (tmpl->type != PIPE_SHADER_IR_NIR || !tmpl->ir.nir) {
      debug_printf("prismrv: only NIR shaders are supported\n");
      return state;
   }
   /* the driver owns the NIR passed to create_*_state */
   nir = tmpl->ir.nir;
   state->usse_text = prismrv_nir_to_usse(NULL, nir);
   if (state->usse_text)
      state->usse_len = strlen(state->usse_text);
   ralloc_free(nir);
   return state;
}

static void
prismrv_delete_shader_state(struct pipe_context *pctx, void *state)
{
   struct prismrv_shader_state *s = state;

   if (s && s->usse_text)
      ralloc_free(s->usse_text);
   FREE(s);
}

static void *
prismrv_create_vs_state(struct pipe_context *pctx,
                        const struct pipe_shader_state *tmpl)
{
   return prismrv_create_shader_state(tmpl);
}

static void
prismrv_bind_vs_state(struct pipe_context *pctx, void *state)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);

   ctx->vs = state;
   ctx->dirty = PRISMRV_DIRTY_ALL;
}

static void *
prismrv_create_fs_state(struct pipe_context *pctx,
                        const struct pipe_shader_state *tmpl)
{
   return prismrv_create_shader_state(tmpl);
}

static void
prismrv_bind_fs_state(struct pipe_context *pctx, void *state)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);

   ctx->fs = state;
   ctx->dirty = PRISMRV_DIRTY_ALL;
}

/* ---- vertex buffers / elements --------------------------------------- */

static void
prismrv_set_vertex_buffers(struct pipe_context *pctx,
                           unsigned count,
                           const struct pipe_vertex_buffer *buffers)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);
   unsigned i;

   for (i = 0; i < ARRAY_SIZE(ctx->vertex_buffers); i++) {
      pipe_resource_reference(&ctx->vertex_buffers[i].buffer.resource, NULL);
      ctx->vertex_buffers[i].is_user_buffer = false;
      ctx->vertex_buffers[i].buffer_offset = 0;
      ctx->user_vertex_buffers[i] = NULL;
   }
   ctx->num_vertex_buffers = 0;

   if (!buffers || !count)
      return;

   for (i = 0; i < count && i < ARRAY_SIZE(ctx->vertex_buffers); i++) {
      ctx->vertex_buffers[i].buffer_offset = buffers[i].buffer_offset;
      ctx->vertex_buffers[i].is_user_buffer = buffers[i].is_user_buffer;
      if (buffers[i].is_user_buffer) {
         ctx->user_vertex_buffers[i] = buffers[i].buffer.user;
      } else if (buffers[i].buffer.resource) {
         pipe_resource_reference(&ctx->vertex_buffers[i].buffer.resource,
                                 buffers[i].buffer.resource);
      } else {
         continue;
      }
      ctx->num_vertex_buffers = i + 1;
   }
}

static void *
prismrv_create_vertex_elements(struct pipe_context *pctx,
                               unsigned num_elems,
                               const struct pipe_vertex_element *elems)
{
   struct prismrv_vertex_element_state *cso;

   /* VS inputs live in r0..r15: more elements cannot be delivered */
   if (num_elems > PRISMRV_MAX_VERTEX_ELEMENTS) {
      debug_printf("prismrv: %u vertex elements (max %u)\n", num_elems,
                   PRISMRV_MAX_VERTEX_ELEMENTS);
      return NULL;
   }
   cso = CALLOC_STRUCT(prismrv_vertex_element_state);
   if (!cso)
      return NULL;
   cso->num_elements = num_elems;
   for (unsigned i = 0; i < num_elems; i++) {
      cso->elements[i].src_offset = elems[i].src_offset;
      cso->elements[i].src_format = elems[i].src_format;
      cso->elements[i].vertex_buffer_index = elems[i].vertex_buffer_index;
      cso->elements[i].src_stride = elems[i].src_stride;
   }
   return cso;
}

static void
prismrv_bind_vertex_elements(struct pipe_context *pctx, void *state)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);
   struct prismrv_vertex_element_state *cso = state;

   if (cso) {
      ctx->num_vertex_elements = cso->num_elements;
      memcpy(ctx->vertex_elements, cso->elements,
             cso->num_elements * sizeof(ctx->vertex_elements[0]));
   } else {
      ctx->num_vertex_elements = 0;
   }
}

/* ---- state the executor cannot use: accepted and ignored -------------- */

/*
 * The state tracker calls these unconditionally when it validates state
 * (a NULL callback crashes the first draw).  The corresponding GL
 * features are either not advertised in the caps or have no executor
 * equivalent yet; see the "Supported subset" section of the README.
 */
static void
prismrv_set_blend_color(struct pipe_context *pctx,
                        const struct pipe_blend_color *color)
{
}

static void
prismrv_set_stencil_ref(struct pipe_context *pctx,
                        const struct pipe_stencil_ref ref)
{
}

static void
prismrv_set_sample_mask(struct pipe_context *pctx, unsigned sample_mask)
{
}

static void
prismrv_set_min_samples(struct pipe_context *pctx, unsigned min_samples)
{
}

static void
prismrv_set_clip_state(struct pipe_context *pctx,
                       const struct pipe_clip_state *state)
{
}

static void
prismrv_set_polygon_stipple(struct pipe_context *pctx,
                            const struct pipe_poly_stipple *stipple)
{
}

static void
prismrv_flush_resource(struct pipe_context *pctx,
                       struct pipe_resource *resource)
{
   /* make pending rendering reach the GPU; completion is observed
    * through fences / prismrv_context_sync() */
   prismrv_context_flush(pctx, NULL, 0);
}

/*
 * Submit anything pending and wait until the GPU has finished all of this
 * context's work.  Used before the CPU touches a resource the GPU may have
 * written or may still read: "GPU cache" coherence (kernel cache
 * maintenance) says nothing about the job having completed.
 */
/*
 * Make a CPU access to @res safe: submit what this context still has
 * queued, then ask the kernel to wait for every job on the BO's
 * reservation object - from any context or process.  Waiting for this
 * context's last fence alone says nothing about another context's job.
 */
bool
prismrv_resource_sync(struct prismrv_context *ctx, struct prismrv_resource *res)
{
   int err;

   if (ctx->context_lost)
      return false;
   if (ctx->batch.cmd_size)
      prismrv_context_flush(&ctx->base, NULL, 0);
   if (ctx->context_lost)
      return false;
   err = prismrv_drm_gem_wait(ctx->screen->fd, res->gem_handle,
                              5ull * 1000 * 1000 * 1000);
   if (err) {
      debug_printf("prismrv: GEM_WAIT failed (%d)\n", err);
      ctx->context_lost = true;
      return false;
   }
   return true;
}

bool
prismrv_context_sync(struct prismrv_context *ctx)
{
   if (ctx->context_lost)
      return false;
   if (ctx->batch.cmd_size)
      prismrv_context_flush(&ctx->base, NULL, 0);
   if (ctx->context_lost)
      return false;
   if (!prismrv_batch_wait_prev(ctx)) {
      /* the GPU did not finish (timeout / poll error): memory the CPU is
       * about to touch may still be in use, so give up on the context */
      ctx->context_lost = true;
      return false;
   }
   return true;
}

static void
prismrv_texture_barrier(struct pipe_context *pctx, unsigned flags)
{
   /* every draw is followed by BARRIER in the stream already */
}

static void
prismrv_memory_barrier(struct pipe_context *pctx, unsigned flags)
{
}

static enum pipe_reset_status
prismrv_get_device_reset_status(struct pipe_context *pctx)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);

   if (ctx->context_lost)
      return PIPE_GUILTY_CONTEXT_RESET;
   /* a GPU reset killed jobs (ours or, with several contexts, someone
    * else's): report it once per reset */
   if (ctx->resets_seen != ctx->screen->device_resets) {
      ctx->resets_seen = ctx->screen->device_resets;
      return PIPE_UNKNOWN_CONTEXT_RESET;
   }
   return PIPE_NO_RESET;
}

/* ---- clears ------------------------------------------------------------ */

/*
 * CPU clear of a colour buffer.  The executor has no clear packet, so the
 * pending job is submitted and waited for (write-after-GPU ordering) and
 * the pixels are written through the BO mapping.
 */
static void
prismrv_clear_color_rect(struct prismrv_context *ctx,
                         const struct pipe_surface *surf,
                         const union pipe_color_union *color,
                         int x, int y, unsigned w, unsigned h)
{
   struct pipe_resource *pres = surf->texture;
   struct prismrv_resource *res = to_prismrv_resource(pres);
   unsigned level = surf->level;
   unsigned lw = prismrv_resource_level_dim(pres->width0, level);
   unsigned lh = prismrv_resource_level_dim(pres->height0, level);
   unsigned bpp = util_format_get_blocksize(surf->format);
   union util_color uc;
   uint8_t *map;

   if (!res || pres->target != PIPE_TEXTURE_2D || bpp == 0 || bpp > 4 ||
       level > pres->last_level)
      return;
   /*
    * Clip the rectangle to the level.  Do it in signed 64-bit arithmetic:
    * w/h are unsigned, so "w += x" with a negative x wrapped to a huge
    * value and a rectangle entirely off to the left/top cleared whole
    * rows.
    */
   {
      int64_t x0 = MAX2((int64_t)x, (int64_t)0);
      int64_t y0 = MAX2((int64_t)y, (int64_t)0);
      int64_t x1 = MIN2((int64_t)x + (int64_t)w, (int64_t)lw);
      int64_t y1 = MIN2((int64_t)y + (int64_t)h, (int64_t)lh);

      if (x1 <= x0 || y1 <= y0)
         return;           /* nothing of it is inside the surface */
      x = (int)x0;
      y = (int)y0;
      w = (unsigned)(x1 - x0);
      h = (unsigned)(y1 - y0);
   }

   if (!prismrv_resource_sync(ctx, res))
      return;
   map = prismrv_resource_map(pres);
   if (!map)
      return;
   map += res->level_offset[level];

   util_pack_color_union(surf->format, &uc, color);
   for (unsigned row = 0; row < h; row++) {
      uint8_t *p = map + ((size_t)(y + row) * lw + x) * bpp;

      for (unsigned col = 0; col < w; col++, p += bpp)
         memcpy(p, &uc, bpp);
   }
}

static void
prismrv_clear(struct pipe_context *pctx, unsigned buffers,
              uint32_t color_clear_mask, uint8_t stencil_clear_mask,
              const struct pipe_scissor_state *scissor_state,
              const union pipe_color_union *color, double depth,
              unsigned stencil)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);
   const struct pipe_framebuffer_state *fb = &ctx->framebuffer;

   if (buffers & PIPE_CLEAR_DEPTHSTENCIL)
      debug_printf("prismrv: depth/stencil clear ignored (no Z buffer)\n");
   if (!(buffers & PIPE_CLEAR_COLOR0) || !fb->nr_cbufs ||
       !fb->cbufs[0].texture)
      return;

   if (scissor_state)
      prismrv_clear_color_rect(ctx, &fb->cbufs[0], color,
                               scissor_state->minx, scissor_state->miny,
                               scissor_state->maxx - scissor_state->minx,
                               scissor_state->maxy - scissor_state->miny);
   else
      prismrv_clear_color_rect(ctx, &fb->cbufs[0], color, 0, 0,
                               fb->width, fb->height);
}

/* ---- lifecycle ------------------------------------------------------- */

static void
prismrv_context_destroy(struct pipe_context *pctx)
{
   struct prismrv_context *ctx = to_prismrv_context(pctx);

   /* Let the GPU finish with the BOs before they go away */
   if (ctx->batch.cmd_size)
      prismrv_context_flush(pctx, NULL, 0);
   prismrv_batch_wait_prev(ctx);

   for (unsigned i = 0; i < ARRAY_SIZE(ctx->textures); i++)
      pipe_resource_reference(&ctx->textures[i], NULL);
   for (unsigned i = 0; i < ARRAY_SIZE(ctx->vertex_buffers); i++)
      pipe_resource_reference(&ctx->vertex_buffers[i].buffer.resource, NULL);
   util_unreference_framebuffer_state(&ctx->framebuffer);

   if (ctx->batch.prev_fence_fd >= 0) {
      close(ctx->batch.prev_fence_fd);
      ctx->batch.prev_fence_fd = -1;
   }

   if (ctx->blitter)
      util_blitter_destroy(ctx->blitter);
   if (ctx->uploader)
      u_upload_destroy(ctx->uploader);
   if (ctx->batch.cmd_map && ctx->batch.cmd_map != MAP_FAILED)
      munmap(ctx->batch.cmd_map, ctx->batch.cmd_capacity);
   if (ctx->batch.ta_map && ctx->batch.ta_map != MAP_FAILED)
      munmap(ctx->batch.ta_map, ctx->batch.ta_capacity);
   if (ctx->batch.cmd_handle) {
      struct prismrv_screen *screen = ctx->screen;
      if (ctx->batch.ta_handle)
         prismrv_drm_gem_close(screen->fd, ctx->batch.ta_handle);
      prismrv_drm_gem_close(screen->fd, ctx->batch.cmd_handle);
   }

   ralloc_free(ctx);
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
   pctx->sampler_view_release = u_default_sampler_view_release;
   pctx->create_vs_state = prismrv_create_vs_state;
   pctx->bind_vs_state = prismrv_bind_vs_state;
   pctx->delete_vs_state = prismrv_delete_shader_state;
   pctx->create_fs_state = prismrv_create_fs_state;
   pctx->bind_fs_state = prismrv_bind_fs_state;
   pctx->delete_fs_state = prismrv_delete_shader_state;
   pctx->set_vertex_buffers = prismrv_set_vertex_buffers;
   pctx->create_vertex_elements_state = prismrv_create_vertex_elements;
   pctx->bind_vertex_elements_state = prismrv_bind_vertex_elements;
   pctx->delete_vertex_elements_state = prismrv_delete_cso;
   pctx->create_blend_state = prismrv_create_blend_state;
   pctx->bind_blend_state = prismrv_bind_blend_state;
   pctx->delete_blend_state = prismrv_delete_cso;
   pctx->create_rasterizer_state = prismrv_create_rasterizer_state;
   pctx->bind_rasterizer_state = prismrv_bind_rasterizer_state;
   pctx->delete_rasterizer_state = prismrv_delete_cso;
   pctx->create_depth_stencil_alpha_state =
      prismrv_create_depth_stencil_alpha_state;
   pctx->bind_depth_stencil_alpha_state =
      prismrv_bind_depth_stencil_alpha_state;
   pctx->delete_depth_stencil_alpha_state = prismrv_delete_cso;
   pctx->create_sampler_state = prismrv_create_sampler_state;
   pctx->bind_sampler_states = prismrv_bind_sampler_states;
   pctx->delete_sampler_state = prismrv_delete_cso;
   pctx->set_sampler_views = prismrv_set_sampler_views;

   /* resource access: transfers go through the screen's transfer helper */
   pctx->buffer_map = u_transfer_helper_transfer_map;
   pctx->texture_map = u_transfer_helper_transfer_map;
   pctx->buffer_unmap = u_transfer_helper_transfer_unmap;
   pctx->texture_unmap = u_transfer_helper_transfer_unmap;
   pctx->transfer_flush_region = u_transfer_helper_transfer_flush_region;
   pctx->buffer_subdata = u_default_buffer_subdata;
   pctx->texture_subdata = u_default_texture_subdata;
   pctx->clear_buffer = u_default_clear_buffer;
   pctx->clear = prismrv_clear;
   pctx->set_blend_color = prismrv_set_blend_color;
   pctx->set_stencil_ref = prismrv_set_stencil_ref;
   pctx->set_sample_mask = prismrv_set_sample_mask;
   pctx->set_min_samples = prismrv_set_min_samples;
   pctx->set_clip_state = prismrv_set_clip_state;
   pctx->set_polygon_stipple = prismrv_set_polygon_stipple;
   pctx->flush_resource = prismrv_flush_resource;
   pctx->texture_barrier = prismrv_texture_barrier;
   pctx->memory_barrier = prismrv_memory_barrier;
   pctx->get_device_reset_status = prismrv_get_device_reset_status;

   ctx->dirty = PRISMRV_DIRTY_ALL;
   ctx->batch.prev_fence_fd = -1;

   ctx->uploader = u_upload_create_default(pctx);
   if (!ctx->uploader)
      return;
   pctx->stream_uploader = ctx->uploader;
   pctx->const_uploader = ctx->uploader;

   prismrv_fence_context_init(ctx);
   ctx->blitter = util_blitter_create(pctx);
}

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

   prismrv_batch_init_context(ctx);
   if (!ctx->batch.cmd_handle || !ctx->batch.cmd_map ||
       ctx->batch.cmd_map == MAP_FAILED || !ctx->uploader) {
      debug_printf("prismrv: failed to create the context\n");
      ctx->base.destroy(&ctx->base);
      return NULL;
   }

   return &ctx->base;
}
