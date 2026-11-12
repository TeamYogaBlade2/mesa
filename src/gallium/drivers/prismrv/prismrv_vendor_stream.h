/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * TA control stream for the dominant draw shape: a clear quad, one indexed draw, the vertex program and the fixed
 * epilogue.  C port of mainline-tools/ta_emit.py stream_records() + the serialisation rules measured in
 * TA_KICK_CAPTURE_03.md (prismrv repository); the test replays 93 real captured kicks (10 distinct streams).
 *
 * NOT covered (the model does not reproduce them either): multi-draw and multi-pass shapes (scissored clears, extra
 * glDrawArrays).  The descriptor objects the state words point at (their contents) are the caller's and are not built here.
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
};

#define PRISMRV_VK_STREAM_MAX_DWORDS 29

/* writes the stream and returns its length in dwords (29), or 0 if a parameter cannot be represented */
unsigned prismrv_vk_stream_build(const struct prismrv_vk_stream_params *p, uint32_t *out, unsigned max);

#endif
