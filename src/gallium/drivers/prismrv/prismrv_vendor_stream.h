/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * TA control stream for the dominant draw shape: a clear quad, one indexed draw, the vertex program and the fixed
 * epilogue.  C port of mainline-tools/ta_emit.py stream_records() + the serialisation rules measured in
 * TA_KICK_CAPTURE_03.md (prismrv repository); the test replays 93 real captured kicks (10 distinct streams).
 *
 * Multi-draw (measured on variants2/cfg7 and on vendor-UMD probes, analysis/agents/multi-draw-pass.md): each extra draw
 * appends [draw][constant pair][vertex PDS record] = 6 dwords; when the primitive CLASS changes (triangles <-> lines) a
 * 3-dword re-emit of state list A precedes it (+8 dwords).  A multi-PASS frame is several KICKS, never one stream.
 *
 * NOT covered (the model does not reproduce them either): scissored clears (extra clear quads), GL_POINTS (the vendor
 * UMD emits no draw for it), more than three extra draws.  The descriptor objects the state words point at (their contents) are the caller's and are not built here.
 */
#ifndef PRISMRV_VENDOR_STREAM_H
#define PRISMRV_VENDOR_STREAM_H

#include <stdbool.h>
#include <stdint.h>

enum prismrv_vk_desc {
   PRISMRV_VK_DESC_STATE_A,    /* state list A (11 dwords, 12 with a stencil attachment) */
   PRISMRV_VK_DESC_SCISSOR,    /* scissor / viewport block */
   PRISMRV_VK_DESC_STATE_B,    /* state list B */
   PRISMRV_VK_DESC_ACCESS,
   PRISMRV_VK_DESC_VPROG,      /* vertex program (PDS) */
   PRISMRV_VK_DESC_EPILOGUE0,  /* five fixed epilogue objects */
   PRISMRV_VK_DESC_COUNT = PRISMRV_VK_DESC_EPILOGUE0 + 5,
};

#define PRISMRV_VK_MAX_EXTRA_DRAWS 3

struct prismrv_vk_extra_draw {
   bool lines;
   uint8_t index_count;        /* 1..63 */
   uint32_t index_va;
   uint32_t class_a_va;        /* device VA of the 3-dword state-list-A object; REQUIRED when `lines` differs from the previous draw */
};

struct prismrv_vk_stream_params {
   bool stencil;               /* stencil attachment: first task word 1a022204 instead of 1a022203 */
   bool varying;               /* vertex program writes a varying */
   uint8_t n_attrs;            /* vertex attributes, 1..127 */
   uint8_t index_count;        /* 1..63 */
   uint32_t clear_quad_va;     /* device VA of the clear quad's index data */
   uint32_t index_va;          /* device VA of the draw's index data */
   bool lines;                 /* GL_LINES instead of triangles */
   bool full;                  /* viewport covers the whole target (state list B is the 17-dword form) */
   uint32_t desc_va[PRISMRV_VK_DESC_COUNT];  /* device VAs of the descriptor objects, 16-byte aligned */
   unsigned n_extra;                          /* additional user draws, 0..PRISMRV_VK_MAX_EXTRA_DRAWS */
   struct prismrv_vk_extra_draw extra[PRISMRV_VK_MAX_EXTRA_DRAWS];
};

/* 29 dwords for one draw, +6 per extra draw, +2 per primitive-class change */
#define PRISMRV_VK_STREAM_MAX_DWORDS (29 + 3 * 8)

/* writes the stream and returns its length in dwords, or 0 if a parameter cannot be represented or `max` is too small */
unsigned prismrv_vk_stream_build(const struct prismrv_vk_stream_params *p, uint32_t *out, unsigned max);

#endif
