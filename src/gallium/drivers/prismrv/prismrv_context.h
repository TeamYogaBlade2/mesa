/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 */
#ifndef PRISMRV_CONTEXT_H_
#define PRISMRV_CONTEXT_H_

#include "prismrv_device.h"

struct blitter_context;
struct u_upload_mgr;

#define PRISMRV_BATCH_MAX_BOS 16

struct prismrv_batch {
   uint32_t cmd_handle;
   uint8_t *cmd_map;
   uint32_t cmd_size;
   uint32_t cmd_capacity;

   /* layer-2 TA packet stream BO */
   uint32_t ta_handle;
   uint8_t *ta_map;
   uint32_t ta_capacity;
   /*
    * Current write cursor into the TA BO.  Each draw appends its TA
    * packet stream at ta_used_offset and advances the cursor.  Without
    * this, every draw would overwrite offset 0 and all DRAW commands
    * in the batch would reference the same (last-written) geometry.
    * Reset to 0 on flush.
    */
   uint32_t ta_used_offset;

   /*
    * Fence fd from the previous submit, kept open until the submit
    * after next so the CPU can wait for GPU completion before
    * re-writing the cmd/TA BO.  -1 when no previous submit exists.
    */
   int prev_fence_fd;

   /* GPU virtual address of the TA BO (from GEM_CREATE) */
   uint32_t ta_gpu_va;
   uint32_t cmd_gpu_va;

   /*
    * BOs referenced by the command stream through their GPU VA
    * (TA data, textures, render target).  Submitted as the BO list so
    * the kernel maps them and orders the job against other users
    * (implicit sync).  bos[0] is always the TA BO when present.
    */
   uint32_t bos[PRISMRV_BATCH_MAX_BOS];
   unsigned num_bos;
};

#define PRISMRV_MAX_VERTEX_ELEMENTS 4   /* VS inputs live in r0..r15 */

struct prismrv_vertex_element {
   unsigned src_offset;
   enum pipe_format src_format;
   unsigned vertex_buffer_index;
   uint32_t src_stride;             /* stride to the same attrib in the next vertex */
};

/* vertex element CSO — one per VAO, allocated in create, freed in delete */
struct prismrv_vertex_element_state {
   unsigned num_elements;
   struct prismrv_vertex_element elements[PRISMRV_MAX_VERTEX_ELEMENTS];
};

/* bound shader state */
struct prismrv_shader_state {
   char *usse_text;         /* compiled USSE text (ralloc), NULL if the
                             * shader could not be compiled */
   unsigned usse_len;
};

struct prismrv_sampler_view {
   struct pipe_sampler_view base;
};

/* fixed-function state (shipped to the executor via SET_* packets) */
/* sampler state as sent in SET_TEXTURE (Gallium PIPE_TEX_* values) */
struct prismrv_sampler_state {
   uint32_t wrap_s, wrap_t, min_img_filter, mag_img_filter, min_mip_filter;
};

struct prismrv_blend_state {
   bool blend_enable;
   unsigned rgb_func, rgb_src, rgb_dst;
   unsigned alpha_func, alpha_src, alpha_dst;
   unsigned colormask;
};

struct prismrv_rasterizer_state {
   bool scissor_enable;
   unsigned cull_face;          /* PIPE_FACE_* */
   bool front_ccw;
};

struct prismrv_depth_stencil_alpha_state {
   bool depth_enabled;
   bool depth_writemask;
   unsigned depth_func;         /* PIPE_FUNC_* */
};

/* dirty bits: state that must be re-sent in the command stream */
#define PRISMRV_DIRTY_ALL 0xffffffffu

#define PRISMRV_MAX_VIEWPORTS 16

struct prismrv_context {
   struct pipe_context base;
   struct prismrv_screen *screen;

   struct prismrv_batch batch;
   struct blitter_context *blitter;
   struct u_upload_mgr *uploader;

   struct pipe_framebuffer_state framebuffer;
   struct pipe_scissor_state scissors[PRISMRV_MAX_VIEWPORTS];

   /* bound shaders (Gallium guarantees a state is unbound before it
    * is deleted, so plain pointers are safe) */
   struct prismrv_shader_state *vs;
   struct prismrv_shader_state *fs;

   /* state that must be re-emitted at the start of the next draw;
    * reset to ALL at every flush because the executor state is per job */
   uint32_t dirty;

   struct pipe_viewport_state viewport;
   bool viewport_valid;

   /* fixed-function state */
   struct prismrv_blend_state blend;
   struct prismrv_rasterizer_state raster;
   struct prismrv_depth_stencil_alpha_state depth;

   /* bound texture views (slot -> resource), consumed by SET_TEXTURE */
   /* pipe_resource refs held for the lifetime of the sampler binding.
    * Released on unbind and on context destroy. */
   struct pipe_resource *textures[8];   /* fragment stage only */
   struct prismrv_sampler_state samplers[8];
   uint8_t tex_level[8];                /* sampler view first_level */
   enum pipe_format tex_format[8];      /* sampler view format */

   /* set to true when a submit fails; draw_vbo returns immediately until
    * the context is destroyed and re-created */
   bool context_lost;
   unsigned resets_seen;     /* screen->device_resets already reported */

   /* vertex elements */
   struct prismrv_vertex_element vertex_elements[PRISMRV_MAX_VERTEX_ELEMENTS];
   unsigned num_vertex_elements;

   /* bound vertex buffers (set_vertex_buffers) */
   struct pipe_vertex_buffer vertex_buffers[8];
   const void *user_vertex_buffers[8];
   unsigned num_vertex_buffers;

   /* constant buffer data (one slot per stage; shipped as
    * VS-block-then-FS-block inside SET_UNIFORMS) */
   float vs_constants[4 * 64];   /* up to 64 vec4 uniforms per stage */
   float fs_constants[4 * 64];
   unsigned num_vs_constants;
   unsigned num_fs_constants;
};

static inline const struct pipe_framebuffer_state *
prismrv_framebuffer(struct prismrv_context *ctx)
{
   return &ctx->framebuffer;
}

bool prismrv_context_sync(struct prismrv_context *ctx);
struct prismrv_resource;
bool prismrv_resource_sync(struct prismrv_context *ctx, struct prismrv_resource *res);

struct pipe_context *
prismrv_context_create(struct pipe_screen *pscreen, void *priv,
                       unsigned flags);

void prismrv_batch_init_context(struct prismrv_context *ctx);
void prismrv_context_init(struct prismrv_context *ctx);

#endif /* PRISMRV_CONTEXT_H_ */
