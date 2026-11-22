/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * Gallium <-> vendor frame glue, written against plain data so it is host-tested with a fake kernel (the Gallium state is flattened
 * by prismrv_context.c into struct prismrv_vk_gl_draw).  EXPERIMENTAL and exclusive: while it is enabled (PRISMRV_VENDOR_GL=1) draws
 * go to the vendor path only and a draw it cannot express is DROPPED with a message (the native command stream cannot run on the
 * stock uKernel, so there is nothing to fall back to).  Nothing here has run on hardware.
 *
 * Supported frame (the shape measured in the captures): clear, then ONE triangle-list draw (indexed u16 or not) into one RGBA8
 * render target, one or two float vertex attributes, no depth/stencil/blend/textures/uniforms.
 */
#ifndef PRISMRV_VENDOR_GL_H
#define PRISMRV_VENDOR_GL_H

#include <stdbool.h>
#include <stdint.h>

#include "prismrv_vendor_ctx.h"
#include "prismrv_vendor_res.h"

/* the HW render context: ui32Sizeof_HWRENDERCONTEXT and ui32OffsetToPDDevPAddr (analysis/agents/r0-owner.md) */
#define PRISMRV_VK_HWRC_SIZE      0x54
#define PRISMRV_VK_HWRC_PD_OFFSET 0x08

#define PRISMRV_VK_GL_MAX_ATTRS 2
#define PRISMRV_VK_GL_MAX_EXTRA_BOS 8

struct prismrv_vk_gl_draw {
   unsigned fb_w, fb_h;
   uint32_t rt_va, rt_handle;
   bool rt_rgba8;                   /* PIPE_FORMAT_R8G8B8A8_UNORM */
   bool has_zs, blend, textures, vs_uniforms, fs_uniforms, triangles;
   float vp_scale[2];
   const uint64_t *vs_code; unsigned vs_words;    /* native USSE, vertex stage */
   const uint64_t *fs_code; unsigned fs_words;    /* native USSE, fragment stage */
   unsigned n_attrs;
   struct { uint32_t va, handle; unsigned ncomp, stride; bool is_float; } attr[PRISMRV_VK_GL_MAX_ATTRS];
   unsigned count, first;           /* vertex / index count and start */
   bool indexed;
   uint32_t index_va, index_handle; /* u16 indices, already offset by the first index */
};

struct prismrv_vk_gl {
   struct prismrv_vk_ctx vk;
   struct prismrv_vk_scene scene;
   bool ready;
   uint32_t hwrc_va, hwrc_handle;   /* handle 0 = the caller-supplied address (PRISMRV_VENDOR_HWRC): no BO to map for it */
   /* recorded frame */
   bool have_draw, have_clear;
   struct prismrv_vk_gl_draw draw;
   uint32_t clear_colour;           /* packed R | G << 8 | B << 16 | A << 24 */
   unsigned dropped;
   uint32_t extra[PRISMRV_VK_GL_MAX_EXTRA_BOS];
   unsigned n_extra;
};

int prismrv_vk_gl_init(struct prismrv_vk_gl *g, const struct prismrv_vk_ops *ops, uint32_t hwrc_va, const char **why);

/* 0 = recorded; -ENOTSUP with *why = the reason (the draw is dropped) */
int prismrv_vk_gl_record_draw(struct prismrv_vk_gl *g, const struct prismrv_vk_gl_draw *d, const char **why);
void prismrv_vk_gl_record_clear(struct prismrv_vk_gl *g, float r, float gr, float b, float a);

/* submits the recorded frame (if any) and resets the recording; returns 0 on success or nothing to do */
int prismrv_vk_gl_flush(struct prismrv_vk_gl *g, const char **why);

#endif
