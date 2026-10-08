/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * Native SGX544 USSE encoder - plain VMAD.
 *
 * Every field below was determined with the vendor disassembler as the
 * oracle (tools/usse_oracle in the prismrv repository: single-bit sweeps,
 * a per-bit influence map over random words, exhaustive swizzle/bank
 * tables), then checked on 20000 random words and 5000 operand sets with
 * zero mismatches.  The fields are INDEPENDENT: no bit means two things in
 * the verified scope.  (The register/swizzle overlap in gpu_emu/isa.py is
 * an artefact of that decoder, not of the hardware.)
 *
 *   src3  reg [5:0]    bank b28=o b29=pa   neg b35 abs b36  swz mode b47 sel b45,b46
 *   src2  reg [11:6]   bank b30=o b31=pa   neg b37 abs b38  swz mode b44 sel b20,b21
 *   src1  reg [17:12]  bank b34=pa                  abs b50  swz mode b53 sel b18,b19
 *   dst   reg [27:22]  bank b32,b33,b51    mask b39=x b40=y
 *   flags nosched b43 syncs b52 skipinv b55 ; predicate b56=p0 b57=!p0
 *   register number = 2 * field; field 60..63 of bank r are i0..i3
 *   opcode bits 63:58 = 0
 * Bits 41, 42, 54 have no visible effect and are required to be 0; bits
 * 48/49 select indexed sources (not modelled).
 */
#include "prismrv_usse_encode.h"

#include <string.h>

static const char *const swz_presets[3][8] = {
   { "xxxx", "yyyy", "zzzz", "wwww", "xyzw", "yzxw", "xyww", "zwxy" },
   { "xxxx", "yyyy", "zzzz", "wwww", "xyzw", "xyyz", "yyww", "wyzw" },
   { "xxxx", "yyyy", "zzzz", "wwww", "xyzw", "xzww", "xxyz", "xyzz" },
};

/* per source slot 1..3: mode bit, select bit 0, select bit 1 */
static const uint8_t swz_bits[3][3] = { { 53, 18, 19 }, { 44, 20, 21 }, { 47, 45, 46 } };
static const uint8_t reg_shift[3] = { 12, 6, 0 };

#define BIT64(n) (UINT64_C(1) << (n))
#define GET(w, n) (unsigned)(((w) >> (n)) & 1)

int
prismrv_usse_swizzle_preset(unsigned slot, const char swz[4])
{
   if (slot < 1 || slot > 3)
      return -1;
   for (int i = 0; i < 8; i++)
      if (!memcmp(swz_presets[slot - 1][i], swz, 4))
         return i;
   return -1;
}

/* register -> 6-bit field; false if the bank cannot address it */
static bool
reg_to_field(unsigned bank, unsigned reg, unsigned *field, unsigned *hw_bank)
{
   if (bank == PRISMRV_USSE_BANK_I) {
      if (reg > 3)
         return false;
      *field = 60 + reg;
      *hw_bank = PRISMRV_USSE_BANK_R;
      return true;
   }
   if (reg & 1)
      return false;
   if (bank == PRISMRV_USSE_BANK_R ? reg >= 120 : reg >= 128)
      return false;
   *field = reg / 2;
   *hw_bank = bank;
   return true;
}

static void
field_to_reg(unsigned hw_bank, unsigned field, uint8_t *bank, uint8_t *reg)
{
   if (hw_bank == PRISMRV_USSE_BANK_R && field >= 60) {
      *bank = PRISMRV_USSE_BANK_I;
      *reg = field - 60;
   } else {
      *bank = hw_bank;
      *reg = 2 * field;
   }
}

bool
prismrv_usse_encode_vmad(const struct prismrv_usse_vmad *in, uint64_t *out)
{
   uint64_t w = 0;
   unsigned field, hw;

   if (in->flags & ~7u || in->pred > PRISMRV_USSE_PRED_NOT_P0 ||
       in->dst.mask > 3)
      return false;
   if (in->flags & PRISMRV_USSE_FLAG_NOSCHED) w |= BIT64(43);
   if (in->flags & PRISMRV_USSE_FLAG_SYNCS)   w |= BIT64(52);
   if (in->flags & PRISMRV_USSE_FLAG_SKIPINV) w |= BIT64(55);
   if (in->pred == PRISMRV_USSE_PRED_P0)      w |= BIT64(56);
   if (in->pred == PRISMRV_USSE_PRED_NOT_P0)  w |= BIT64(57);

   /* destination */
   if (!reg_to_field(in->dst.bank, in->dst.reg, &field, &hw))
      return false;
   switch (hw) {
   case PRISMRV_USSE_BANK_R:                         break;
   case PRISMRV_USSE_BANK_SA: w |= BIT64(51);        break;
   case PRISMRV_USSE_BANK_PA: w |= BIT64(33);        break;
   case PRISMRV_USSE_BANK_O:  w |= BIT64(32);        break;
   default: return false;
   }
   w |= (uint64_t)field << 22;
   if (in->dst.mask & PRISMRV_USSE_MASK_X) w |= BIT64(39);
   if (in->dst.mask & PRISMRV_USSE_MASK_Y) w |= BIT64(40);

   for (unsigned n = 1; n <= 3; n++) {
      const struct prismrv_usse_src *s = &in->src[n - 1];
      int preset = prismrv_usse_swizzle_preset(n, s->swz);

      if (preset < 0 || s->swz[4] != '\0')
         return false;
      if (!reg_to_field(s->bank, s->reg, &field, &hw))
         return false;
      w |= (uint64_t)field << reg_shift[n - 1];

      if (n == 1) {
         if (s->neg)
            return false;                 /* no negate on source 1 */
         if (hw == PRISMRV_USSE_BANK_PA)
            w |= BIT64(34);
         else if (hw != PRISMRV_USSE_BANK_R)
            return false;                 /* source 1: r or pa only */
         if (s->abs) w |= BIT64(50);
      } else {
         unsigned ob = n == 2 ? 30 : 28, pb = ob + 1;
         unsigned nb = n == 2 ? 37 : 35, ab = nb + 1;

         if (hw == PRISMRV_USSE_BANK_O || hw == PRISMRV_USSE_BANK_SA)
            w |= BIT64(ob);
         if (hw == PRISMRV_USSE_BANK_PA || hw == PRISMRV_USSE_BANK_SA)
            w |= BIT64(pb);
         if (s->neg) w |= BIT64(nb);
         if (s->abs) w |= BIT64(ab);
      }
      w |= (uint64_t)(preset >> 2) << swz_bits[n - 1][0];
      w |= (uint64_t)(preset & 1) << swz_bits[n - 1][1];
      w |= (uint64_t)((preset >> 1) & 1) << swz_bits[n - 1][2];
   }
   *out = w;
   return true;
}

bool
prismrv_usse_decode_vmad(uint64_t w, struct prismrv_usse_vmad *out)
{
   struct prismrv_usse_vmad d;
   unsigned hw;

   if ((w >> 58) || GET(w, 48) || GET(w, 49) || GET(w, 41) || GET(w, 42) ||
       GET(w, 54))
      return false;
   if (GET(w, 56) && GET(w, 57))
      return false;                          /* "Pn" predicate: not modelled */
   memset(&d, 0, sizeof(d));
   d.flags = (GET(w, 43) ? PRISMRV_USSE_FLAG_NOSCHED : 0) |
             (GET(w, 52) ? PRISMRV_USSE_FLAG_SYNCS : 0) |
             (GET(w, 55) ? PRISMRV_USSE_FLAG_SKIPINV : 0);
   d.pred = GET(w, 56) ? PRISMRV_USSE_PRED_P0 :
            GET(w, 57) ? PRISMRV_USSE_PRED_NOT_P0 : PRISMRV_USSE_PRED_NONE;

   switch ((GET(w, 32) << 2) | (GET(w, 33) << 1) | GET(w, 51)) {
   case 0: hw = PRISMRV_USSE_BANK_R;  break;
   case 1: hw = PRISMRV_USSE_BANK_SA; break;
   case 2: hw = PRISMRV_USSE_BANK_PA; break;
   case 4: hw = PRISMRV_USSE_BANK_O;  break;
   default: return false;                 /* i / c / indexed destinations */
   }
   field_to_reg(hw, (w >> 22) & 63, &d.dst.bank, &d.dst.reg);
   d.dst.mask = (GET(w, 39) ? PRISMRV_USSE_MASK_X : 0) |
                (GET(w, 40) ? PRISMRV_USSE_MASK_Y : 0);

   for (unsigned n = 1; n <= 3; n++) {
      struct prismrv_usse_src *s = &d.src[n - 1];
      unsigned preset;

      if (n == 1) {
         hw = GET(w, 34) ? PRISMRV_USSE_BANK_PA : PRISMRV_USSE_BANK_R;
         s->abs = GET(w, 50);
      } else {
         unsigned ob = n == 2 ? 30 : 28, nb = n == 2 ? 37 : 35;
         unsigned o = GET(w, ob), pa = GET(w, ob + 1);

         hw = o ? (pa ? PRISMRV_USSE_BANK_SA : PRISMRV_USSE_BANK_O) :
                  (pa ? PRISMRV_USSE_BANK_PA : PRISMRV_USSE_BANK_R);
         s->neg = GET(w, nb);
         s->abs = GET(w, nb + 1);
      }
      field_to_reg(hw, (w >> reg_shift[n - 1]) & 63, &s->bank, &s->reg);
      preset = (GET(w, swz_bits[n - 1][0]) << 2) |
               (GET(w, swz_bits[n - 1][2]) << 1) | GET(w, swz_bits[n - 1][1]);
      memcpy(s->swz, swz_presets[n - 1][preset], 5);
   }
   *out = d;
   return true;
}
