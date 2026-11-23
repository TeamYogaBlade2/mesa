/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 */
#include "prismrv_vendor_stream.h"

#define STREAM_END 0xc0000000u

static unsigned
put_state(uint32_t *o, unsigned n, unsigned cls, uint32_t desc_va, uint32_t task)
{
   o[n++] = (cls << 28) | ((desc_va >> 4) & 0x0fffffff);
   o[n++] = task;
   return n;
}

static unsigned
put_draw(uint32_t *o, unsigned n, bool lines, unsigned count, uint32_t index_va)
{
   o[n++] = (lines ? 0x85c00000u : 0x81c00000u) | count;
   o[n++] = index_va;
   o[n++] = 0x70000000;          /* the constant pair that follows every draw */
   o[n++] = 0x003fffff;
   return n;
}

unsigned
prismrv_vk_stream_build(const struct prismrv_vk_stream_params *p, uint32_t *out, unsigned max)
{
   const uint32_t first_task = p->stencil ? 0x1a022204 : 0x1a022203;
   const uint32_t b_task = p->full ? 0x1a022205 : 0x1a022207;
   const uint32_t vprog_task = ((p->varying ? 0x05u : 0x03u) << 24) | (0x80u << 16) | ((2u * p->n_attrs) << 8) | (p->n_attrs + 1u);
   static const uint32_t epilogue_task[5] = { 0x0800e100, 0x1a024201, 0x1a026201, 0x1a028201, 0x1a022201 };
   unsigned n = 0, i;

   bool prev_lines = p->lines;
   unsigned need = 29 + 6 * p->n_extra;

   if (p->n_attrs < 1 || p->n_attrs > 127 || p->index_count < 1 || p->index_count > 63 || p->n_extra > PRISMRV_VK_MAX_EXTRA_DRAWS)
      return 0;
   for (i = 0; i < PRISMRV_VK_DESC_COUNT; i++)
      if (p->desc_va[i] & 0xf)
         return 0;
   for (i = 0; i < p->n_extra; i++) {
      const struct prismrv_vk_extra_draw *e = &p->extra[i];
      bool change = e->lines != prev_lines;

      if (e->index_count < 1 || e->index_count > 63 || (change && (!e->class_a_va || (e->class_a_va & 0xf))))
         return 0;
      need += change ? 2 : 0;
      prev_lines = e->lines;
   }

   if (max < need)
      return 0;

   n = put_state(out, n, 4, p->desc_va[PRISMRV_VK_DESC_STATE_A], first_task);
   n = put_draw(out, n, false, 3, p->clear_quad_va);                           /* the clear quad */
   n = put_state(out, n, 7, p->desc_va[PRISMRV_VK_DESC_SCISSOR], 0x05800202);
   n = put_state(out, n, 4, p->desc_va[PRISMRV_VK_DESC_STATE_B], b_task);
   n = put_state(out, n, 4, p->desc_va[PRISMRV_VK_DESC_ACCESS], 0x1000e103);
   n = put_draw(out, n, p->lines, p->index_count, p->index_va);
   n = put_state(out, n, 0xf, p->desc_va[PRISMRV_VK_DESC_VPROG], vprog_task);
   prev_lines = p->lines;
   for (i = 0; i < p->n_extra; i++) {
      const struct prismrv_vk_extra_draw *e = &p->extra[i];

      if (e->lines != prev_lines)       /* primitive class changed: state list A is re-emitted in its 3-dword form */
         n = put_state(out, n, 4, e->class_a_va, first_task);
      n = put_draw(out, n, e->lines, e->index_count, e->index_va);
      n = put_state(out, n, 0xf, p->desc_va[PRISMRV_VK_DESC_VPROG], vprog_task);
      prev_lines = e->lines;
   }
   for (i = 0; i < 5; i++)
      n = put_state(out, n, i == 4 ? 6 : 4, p->desc_va[PRISMRV_VK_DESC_EPILOGUE0 + i], epilogue_task[i]);
   out[n++] = STREAM_END;
   return n;
}
