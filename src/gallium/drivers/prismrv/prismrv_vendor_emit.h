/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * Vendor-ABI kick emitter (the userspace half of DRM_PRISMRV_SUBMIT_VENDOR): the dwords of the
 * client-CCB header, the CMDTA TA block, the 3D register block and the two ISP state lists, as a
 * function of GL state.  This is a C port of mainline-tools/ta_emit.py in the prismrv repository, whose
 * rules were verified against the capture sets (analysis/TA_KICK_EMITTER_SPEC.md); the golden test in
 * tests/prismrv_vendor_emit_test.c checks this port against that model.
 *
 * NOT covered (open in the spec, do not invent): the TA control-stream serialisation, shader (USSE)
 * binding (spec section 8 item 9), allocation-dependent words (device VAs, 3DREGISTERS dw25 ...),
 * CMDTA_SHARED (kernel side).  Words the model leaves to the caller are written as 0.
 */
#ifndef PRISMRV_VENDOR_EMIT_H
#define PRISMRV_VENDOR_EMIT_H

#include <stdbool.h>
#include <stdint.h>

enum prismrv_vk_func { /* 3-bit compare code shared by depth and stencil */
   PRISMRV_VK_NEVER, PRISMRV_VK_LESS, PRISMRV_VK_EQUAL, PRISMRV_VK_LEQUAL,
   PRISMRV_VK_GREATER, PRISMRV_VK_NOTEQUAL, PRISMRV_VK_GEQUAL, PRISMRV_VK_ALWAYS,
};
enum prismrv_vk_stencil_op {
   PRISMRV_VK_SOP_KEEP, PRISMRV_VK_SOP_ZERO, PRISMRV_VK_SOP_REPLACE,
   PRISMRV_VK_SOP_INCR, PRISMRV_VK_SOP_DECR, PRISMRV_VK_SOP_INVERT,
};
enum prismrv_vk_cull { PRISMRV_VK_CULL_NONE, PRISMRV_VK_CULL_FRONT, PRISMRV_VK_CULL_BACK };

struct prismrv_vk_state {
   uint32_t width, height;            /* render target */
   bool depth, stencil;               /* attachments present */
   int32_t vx, vy, vw, vh;            /* viewport; (0,0,width,height) is the "full" form */
   double depth_near, depth_far;
   uint8_t depth_func;                /* enum prismrv_vk_func */
   bool depth_write;
   int8_t depth_test;                 /* -1: derive (func != ALWAYS || !depth_write), 0, 1 */
   bool blend, lines;
   uint8_t cull;                      /* enum prismrv_vk_cull */
   bool color_mask[4];                /* r, g, b, a write enables */
   bool stencil_test;                 /* only meaningful with a stencil attachment */
   uint8_t stencil_ref;
   uint8_t st_func, st_sfail, st_zfail, st_zpass; /* enum prismrv_vk_func / _stencil_op */
   uint8_t st_write_mask, st_compare_mask;
   uint32_t stream_va;                /* device VA of the TA control stream (caller's allocation) */
   uint32_t render_details_va;        /* device VA of HWRENDERDETAILS (caller's allocation) */
};

struct prismrv_vk_kick {
   uint32_t ccb[0x14];                /* client CCB header, 0x50 bytes (dwords the model does not set are 0) */
   uint32_t ta[18];                   /* CMDTA TA block, registers 0x204..0x250 */
   uint32_t three_d[59];              /* HWRENDERDETAILS 3DREGISTERS */
   uint32_t list_a[12];  unsigned n_a; /* ISP state list A: 11 dwords, 12 with a stencil attachment */
   uint32_t list_b[20];  unsigned n_b; /* ISP state list B: 17 / 19 dwords (+1 stencil record) */
};

void prismrv_vk_emit(const struct prismrv_vk_state *s, struct prismrv_vk_kick *out);

#endif
