/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * See prismrv_vendor_emit.h.  Every constant below is carried over from the verified model; the
 * comments say what the model verified, not what was guessed.
 */
#include "prismrv_vendor_emit.h"

#include <string.h>

static uint32_t f2u(double v)
{
   float f = (float)v;
   uint32_t u;
   memcpy(&u, &f, sizeof(u));
   return u;
}

/* +0x84: func<<22, 0x200 base, bit20 = depth write DISABLED, bit25 = blend, bit8 = stencil test, low byte = ref */
static uint32_t isp_control_word(bool depth_test, unsigned func, bool depth_write, bool blend,
                                 bool stencil_test, uint8_t ref)
{
   bool wr_off;

   if (!depth_test) {
      func = PRISMRV_VK_ALWAYS;
      wr_off = true;
   } else {
      wr_off = !depth_write;
   }
   uint32_t w = (func << 22) | 0x200 | (wr_off ? 0x100000 : 0);
   w |= blend ? 0x02000000 : 0;
   if (stencil_test)
      w |= 0x100 | ref;
   return w;
}

/* +0x04: bit20 = depth writes cannot happen (no depth buffer, or masked), bit8 = stencil buffer attached */
static uint32_t isp_second_word(bool depth_write, bool stencil_buffer, bool depth_present)
{
   bool off = !depth_present || !depth_write;
   return 0x01c00200 | (off ? 0x100000 : 0) | (stencil_buffer ? 0x100 : 0);
}

static uint32_t stencil_state_word(const struct prismrv_vk_state *s)
{
   return ((uint32_t)s->st_zpass << 16) | ((uint32_t)s->st_zfail << 19) | ((uint32_t)s->st_sfail << 22) |
          ((uint32_t)s->st_func << 25) | ((uint32_t)s->st_compare_mask << 8) | s->st_write_mask;
}

void prismrv_vk_emit(const struct prismrv_vk_state *s, struct prismrv_vk_kick *out)
{
   const bool full = s->vx == 0 && s->vy == 0 && s->vw == (int32_t)s->width && s->vh == (int32_t)s->height;
   const bool stencil_rec = s->stencil_test && s->stencil;
   unsigned i;

   /* heap-6 references (UNVERIFIED on hardware; the encoding (va >> 4, raw for 3D dw30) is exact on 119 captured kicks) */
   const uint32_t ref_bg = (s->pds_bg_va ? s->pds_bg_va >> 4 : 0x0e401000) & 0x0fffffff;
   const uint32_t ref_bg2 = (s->pds_bg2_va ? s->pds_bg2_va >> 4 : 0x0e40180c) & 0x0fffffff;
   const uint32_t ref_fs = (s->pds_fs_va ? s->pds_fs_va >> 4 : 0x0e401002) & 0x0fffffff;
   const uint32_t arena2 = s->pds_arena2_va ? s->pds_arena2_va : 0xe4018000;
   const uint32_t rt_va = s->rt_va ? s->rt_va : 0x10128000, depth_va = s->depth_va ? s->depth_va : 0x10100000;
   const uint32_t ta8_va = s->ta8_va ? s->ta8_va : 0xd000c000, ta17_va = s->ta17_va ? s->ta17_va : 0xd000a000;
   const uint32_t ccb3_va = s->ccb3_va ? s->ccb3_va : 0x1001e000;
   /* list A dw4 / list B word: 0x0801e000 / 0x0803e000 in every capture = 0x08000000 | (ccb3 + 0 / 0x20000) & 0x07ffffff, i.e. two
    * addresses inside the same zero buffer.  The relation is seen at one ccb3 value only (UNVERIFIED for others). */
   const uint32_t zbuf_a = 0x08000000u | (ccb3_va & 0x07ffffffu), zbuf_b = 0x08000000u | ((ccb3_va + 0x20000u) & 0x07ffffffu);

   memset(out, 0, sizeof(*out));

   /* client CCB header: a template; only dword 7 (surfaces owned: 0x60 | depth 0x80 | stencil 0x100) varies */
   out->ccb[0x00 / 4] = 0x540;
   out->ccb[0x04 / 4] = 0x71;
   out->ccb[0x08 / 4] = ref_bg;
   out->ccb[0x0c / 4] = ccb3_va;
   out->ccb[0x10 / 4] = 0x16401808;
   out->ccb[0x14 / 4] = 0x0c000000;
   out->ccb[0x18 / 4] = 0x0c000000;
   out->ccb[0x1c / 4] = 0x60 | (s->depth ? 0x80 : 0) | (s->stencil ? 0x100 : 0);
   out->ccb[0x38 / 4] = s->render_details_va;

   /* CMDTA TA block: the 7 constants identical in every capture, plus the state-dependent ones */
   out->ta[1] = 0x01004004;
   out->ta[2] = 0x01004004;
   out->ta[3] = ((((s->height + 31) / 32) - 1) << 12) | (((s->width + 31) / 32) - 1);
   out->ta[4] = 0x00000010;
   out->ta[5] = s->depth ? 0x02000000 : 0;
   out->ta[6] = s->stream_va;
   out->ta[7] = 0x000000a2;
   out->ta[10] = 0x358637bd;
   out->ta[11] = 0x358637bd;
   out->ta[12] = ((s->height - 1) << 12) | (s->width - 1);
   out->ta[17] = 0x00000088;

   /* 3DREGISTERS: the dwords that never vary (dw25 is allocation-dependent and left to the caller) */
   out->three_d[0] = rt_va;
   out->three_d[1] = 0x40700000;
   out->three_d[2] = s->depth ? depth_va : 0;
   out->three_d[3] = 0x00000080;
   out->three_d[4] = 0x000f0000;
   out->three_d[6] = 0x000000e9;
   out->three_d[8] = ta8_va;
   out->three_d[9] = 0x00000400;
   out->three_d[10] = 0x00000010;
   out->three_d[11] = 0x01004004;
   out->three_d[12] = 0x01004004;
   out->three_d[14] = 0x00000001;
   out->three_d[15] = 0x1e3ce508;
   out->three_d[16] = 0x1e3ce508;
   out->three_d[17] = ta17_va;
   out->three_d[18] = (s->depth && s->stencil) ? 0x00171008 : (s->depth ? 0x00151008 : 0);
   /*
    * The words the Python model leaves out (CMDTA_REGISTER_BLOCKS.md; analysis/ZLS_AUX_WORDS.md):
    *  dw21 = dw22: 1 without a depth target, 0xeff00001 with depth, 0x0009b001 with depth + stencil - the same in every capture set of that kind;
    *  dw19 = dw20: 0 without a depth target; with one 0x97000 for a 64x64 target and 0xa3000 for 128x128 (59 captures, 3 sets: 55 / 3 / 1 with 0x98000).  The two
    *               measured sizes differ by exactly 4 KiB per 32x32 tile, so 0x93000 + tiles * 0x1000 is used - an extrapolation beyond those two sizes.
    *  dw25: caller's value, default the most common capture value.
    */
   if (s->depth) {
      const uint32_t tiles = ((s->width + 31) / 32) * ((s->height + 31) / 32);

      out->three_d[19] = out->three_d[20] = 0x00093000 + tiles * 0x1000;
      out->three_d[21] = out->three_d[22] = s->stencil ? 0x0009b001 : 0xeff00001;
   } else {
      out->three_d[21] = out->three_d[22] = 1;
   }
   out->three_d[25] = s->dw25 ? s->dw25 : 0x0280640e;
   out->three_d[23] = 0x3f800000;
   out->three_d[24] = 0x00000300;
   out->three_d[26] = 0x00000188;
   out->three_d[27] = 0x00000088;
   out->three_d[28] = 0x00000400;
   out->three_d[29] = 0x00000010;
   out->three_d[30] = arena2;
   out->three_d[31] = 0x00000003;
   out->three_d[32] = 0x00004000;

   uint32_t mask = 0;
   bool masked = false;
   for (i = 0; i < 4; i++) {
      mask |= (s->color_mask[3 - i] ? 1u : 0u) << (12 + i);
      if (!s->color_mask[i])
         masked = true;
   }

   /* state list A */
   uint32_t isp2 = isp_second_word(s->depth_write, s->stencil, s->depth);
   if (s->stencil) {
      const uint32_t a[12] = { 0x0000d4c7, isp2, 0x0000f000, 0x0e0200ff, ref_bg, zbuf_a, ref_bg2,
                               0x80000001, 0x00000001, 0x08001800, 0x00010000, 0 };
      memcpy(out->list_a, a, sizeof(a));
      out->n_a = 12;
   } else {
      const uint32_t a[11] = { 0x0000d4c3, isp2, 0x0000f000, ref_bg, zbuf_a, ref_bg2,
                               0x80000001, 0x00000001, 0x08001800, 0x00010000, 0 };
      memcpy(out->list_a, a, sizeof(a));
      out->n_a = 11;
   }

   /* state list B */
   bool depth_test = s->depth_test < 0 ? (s->depth_func != PRISMRV_VK_ALWAYS || !s->depth_write) : s->depth_test;
   uint32_t isp1 = isp_control_word(depth_test, s->depth_func, s->depth_write, s->blend, stencil_rec, s->stencil_ref);
   if (s->lines)
      isp1 |= 0x00008000; /* GL_LINES: bit 15 of the +0x84 word */

   uint32_t b[20];
   unsigned n = 0;
   b[n++] = 0; /* head word, fixed below */
   b[n++] = isp1;
   b[n++] = 0x0000f000;
   b[n++] = 0x16401812;
   b[n++] = zbuf_b;
   b[n++] = ref_fs;
   if (!full) {
      b[n++] = 0x80010001;
      b[n++] = 0x00000000;
   }
   /* viewport: offset = X + w/2, scale = w/2 per axis, as (offX, scaleX, offY, scaleY) */
   b[n++] = f2u(s->vx + s->vw / 2.0);
   b[n++] = f2u(s->vw / 2.0);
   b[n++] = f2u(s->vy + s->vh / 2.0);
   b[n++] = f2u(s->vh / 2.0);
   /* depth range: (mid, half) */
   b[n++] = f2u((s->depth_near + s->depth_far) / 2.0);
   b[n++] = f2u((s->depth_far - s->depth_near) / 2.0);
   b[n++] = 0;
   b[n++] = 0x04001000;
   b[n++] = 0x3727c5ac;
   b[n++] = 0x00088000 | s->cull;
   b[n++] = 0;
   if (masked) {
      b[2] = mask; /* per-channel write enables, bits 15:12, in both lists */
      out->list_a[2] = mask;
   }
   if (stencil_rec) {
      memmove(&b[4], &b[3], (n - 3) * sizeof(b[0]));
      b[3] = stencil_state_word(s);
      n++;
   }
   /* head word: 0xdf43; bit 2 with the stencil record, bit 7 once the two viewport words are in front */
   b[0] = 0x0000df43 | (stencil_rec ? 0x4 : 0) | (full ? 0 : 0x80);
   memcpy(out->list_b, b, n * sizeof(b[0]));
   out->n_b = n;
}
