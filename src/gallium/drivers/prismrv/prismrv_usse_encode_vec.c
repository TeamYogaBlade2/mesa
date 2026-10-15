/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * Native SGX544 USSE encoder - two-source vector group (vmul/vadd/vmin/vmax)
 * and vmov.  Field layout (all verified against the vendor disassembler,
 * see tools/usse_oracle/vec_model.py and test_vec_model.py in the prismrv
 * repository: 5000 random words each, 0 mismatches, exact round trip):
 *
 * vec2  opcode [63:59]=00001, sub-opcode [14:12] 0 vmul 1 vadd 5 vmin 6 vmax
 *   src2  reg [5:0]   bank b28=o b29=pa (both sa)  abs b36   swz [47:44] preset
 *   src1  reg [11:6]  bank b30=o b31=pa (both sa)  neg b37 abs b38
 *         swz per character, 3 bits each, value 0..7 = x y z w 0 1 2 h:
 *         c0 b15,b16,b17  c1 b18,b19,b20  c2 b21,b34,b35  c3 b50,b53,b54
 *   dst   reg [27:22] bank b32,b33,b51  mask b39..b42 = x y z w
 *   flags nosched b43 syncs b52 skipinv b55; predicate [58:56]
 *   register number = 2 * field; bits 48/49 = indexed sources (rejected)
 *
 * vmov  opcode [63:59]=00111; register numbers are 1:1
 *   src   reg [11:6]  bank b30,b31  swz [38:35] preset
 *   dst   reg [23:18] bank b32,b33,b51  mask [27:24]
 *   type [42:40] i8 i16 i32; repeat [45:44] = count-1
 *   flags nosched b43 end b50 syncs b52 skipinv b55; predicate [58:56]
 *   bits 0-5,12-17,28,29,34,39,46-49,53,54 must be zero
 */
#include "prismrv_usse_encode.h"

#include <string.h>

#define BIT64(n) (UINT64_C(1) << (n))
#define GET(w, n) (unsigned)(((w) >> (n)) & 1)
#define BITS(w, hi, lo) (unsigned)(((w) >> (lo)) & ((UINT64_C(1) << ((hi) - (lo) + 1)) - 1))

static const char swz1_chars[8] = { 'x', 'y', 'z', 'w', '0', '1', '2', 'h' };
static const uint8_t swz1_bits[4][3] = { { 15, 16, 17 }, { 18, 19, 20 },
                                         { 21, 34, 35 }, { 50, 53, 54 } };
static const char *const swz16[16] = {
   "xxxx", "yyyy", "zzzz", "wwww", "xyzw", "yzww", "xyzz", "xxyz",
   "xyxy", "xywz", "zxyw", "zwzw", "yzxz", "xxyy", "xzww", "xyz1",
};

static int
swz16_index(const char *s)
{
   for (int i = 0; i < 16; i++)
      if (!memcmp(swz16[i], s, 4) && s[4] == '\0')
         return i;
   return -1;
}

/* register -> field; mul = 2 for the doubled groups, 1 for vmov */
static bool
reg_field(unsigned bank, unsigned reg, unsigned mul, unsigned *field,
          unsigned *hw_bank)
{
   if (bank == PRISMRV_USSE_BANK_I) {
      if (reg > 3)
         return false;
      *field = 60 + reg;
      *hw_bank = PRISMRV_USSE_BANK_R;
      return true;
   }
   if (reg % mul)
      return false;
   if (reg / mul >= (bank == PRISMRV_USSE_BANK_R ? 60u : 64u))
      return false;
   *field = reg / mul;
   *hw_bank = bank;
   return true;
}

static void
field_reg(unsigned hw_bank, unsigned field, unsigned mul, uint8_t *bank,
          uint8_t *reg)
{
   if (hw_bank == PRISMRV_USSE_BANK_R && field >= 60) {
      *bank = PRISMRV_USSE_BANK_I;
      *reg = field - 60;
   } else {
      *bank = hw_bank;
      *reg = mul * field;
   }
}

/* source bank: (o bit, pa bit) */
static bool
src_bank_bits(unsigned hw, unsigned *o, unsigned *pa)
{
   switch (hw) {
   case PRISMRV_USSE_BANK_R:  *o = 0; *pa = 0; return true;
   case PRISMRV_USSE_BANK_O:  *o = 1; *pa = 0; return true;
   case PRISMRV_USSE_BANK_PA: *o = 0; *pa = 1; return true;
   case PRISMRV_USSE_BANK_SA: *o = 1; *pa = 1; return true;
   default: return false;
   }
}

static unsigned
src_bank_from_bits(unsigned o, unsigned pa)
{
   return o ? (pa ? PRISMRV_USSE_BANK_SA : PRISMRV_USSE_BANK_O)
            : (pa ? PRISMRV_USSE_BANK_PA : PRISMRV_USSE_BANK_R);
}

static bool
dst_bank_bits(unsigned hw, uint64_t *w)
{
   switch (hw) {
   case PRISMRV_USSE_BANK_R:  return true;
   case PRISMRV_USSE_BANK_SA: *w |= BIT64(51); return true;
   case PRISMRV_USSE_BANK_PA: *w |= BIT64(33); return true;
   case PRISMRV_USSE_BANK_O:  *w |= BIT64(32); return true;
   default: return false;
   }
}

static bool
dst_bank_from_bits(uint64_t w, unsigned *hw)
{
   switch ((GET(w, 32) << 2) | (GET(w, 33) << 1) | GET(w, 51)) {
   case 0: *hw = PRISMRV_USSE_BANK_R;  return true;
   case 1: *hw = PRISMRV_USSE_BANK_SA; return true;
   case 2: *hw = PRISMRV_USSE_BANK_PA; return true;
   case 4: *hw = PRISMRV_USSE_BANK_O;  return true;
   default: return false;
   }
}

/* predicate encodings differ between the two groups (verified) */
static bool
pred_to_bits(unsigned pred, bool is_vmov, unsigned *bits)
{
   switch (pred) {
   case PRISMRV_USSE_PRED_NONE:   *bits = 0; return true;
   case PRISMRV_USSE_PRED_P0:     *bits = 1; return true;
   case PRISMRV_USSE_PRED_NOT_P0: *bits = is_vmov ? 5 : 4; return true;
   default: return false;
   }
}

static bool
pred_from_bits(unsigned bits, bool is_vmov, uint8_t *pred)
{
   if (bits == 0) { *pred = PRISMRV_USSE_PRED_NONE; return true; }
   if (bits == 1) { *pred = PRISMRV_USSE_PRED_P0; return true; }
   if (bits == (is_vmov ? 5u : 4u)) { *pred = PRISMRV_USSE_PRED_NOT_P0; return true; }
   return false;           /* p1/p2/p3/!p1/!p2/Pn: not modelled in the IR */
}

bool
prismrv_usse_encode_vec2(const struct prismrv_usse_vec2 *in, uint64_t *out)
{
   static const uint8_t sub[4] = { 0, 1, 5, 6 };
   uint64_t w = BIT64(59);
   unsigned field, hw, o, pa, pb;

   if (in->op > PRISMRV_USSE_VMAX || (in->flags & ~7u) ||
       !pred_to_bits(in->pred, false, &pb))
      return false;
   w |= (uint64_t)sub[in->op] << 12;
   if (in->flags & PRISMRV_USSE_FLAG_NOSCHED) w |= BIT64(43);
   if (in->flags & PRISMRV_USSE_FLAG_SYNCS)   w |= BIT64(52);
   if (in->flags & PRISMRV_USSE_FLAG_SKIPINV) w |= BIT64(55);
   w |= (uint64_t)pb << 56;

   /* an empty mask prints as "r0." and is not known to mean anything */
   if (!in->dst.mask || in->dst.mask > PRISMRV_USSE_MASK_XYZW)
      return false;
   if (!reg_field(in->dst.bank, in->dst.reg, 2, &field, &hw) ||
       !dst_bank_bits(hw, &w))
      return false;
   w |= (uint64_t)field << 22;
   w |= (uint64_t)in->dst.mask << 39;

   /* src1: free per-character swizzle, neg, abs */
   {
      const struct prismrv_usse_src *s = &in->src[0];

      if (!reg_field(s->bank, s->reg, 2, &field, &hw) ||
          !src_bank_bits(hw, &o, &pa) || s->swz[4])
         return false;
      w |= (uint64_t)field << 6 | (uint64_t)o << 30 | (uint64_t)pa << 31;
      if (s->neg) w |= BIT64(37);
      if (s->abs) w |= BIT64(38);
      for (unsigned c = 0; c < 4; c++) {
         const char *p = memchr(swz1_chars, s->swz[c], 8);

         if (!p || !s->swz[c])
            return false;
         for (unsigned k = 0; k < 3; k++)
            w |= (uint64_t)(((unsigned)(p - swz1_chars) >> k) & 1)
                 << swz1_bits[c][k];
      }
   }
   /* src2: 16 presets, abs, no negate */
   {
      const struct prismrv_usse_src *s = &in->src[1];
      int preset = swz16_index(s->swz);

      if (s->neg || preset < 0 || !reg_field(s->bank, s->reg, 2, &field, &hw) ||
          !src_bank_bits(hw, &o, &pa))
         return false;
      w |= (uint64_t)field | (uint64_t)o << 28 | (uint64_t)pa << 29 |
           (uint64_t)preset << 44;
      if (s->abs) w |= BIT64(36);
   }
   *out = w;
   return true;
}

bool
prismrv_usse_decode_vec2(uint64_t w, struct prismrv_usse_vec2 *out)
{
   struct prismrv_usse_vec2 d;
   unsigned hw;

   if (BITS(w, 63, 59) != 1 || GET(w, 48) || GET(w, 49))
      return false;
   memset(&d, 0, sizeof(d));
   switch (BITS(w, 14, 12)) {
   case 0: d.op = PRISMRV_USSE_VMUL; break;
   case 1: d.op = PRISMRV_USSE_VADD; break;
   case 5: d.op = PRISMRV_USSE_VMIN; break;
   case 6: d.op = PRISMRV_USSE_VMAX; break;
   default: return false;
   }
   if (!pred_from_bits(BITS(w, 58, 56), false, &d.pred) ||
       !dst_bank_from_bits(w, &hw))
      return false;
   d.flags = (GET(w, 43) ? PRISMRV_USSE_FLAG_NOSCHED : 0) |
             (GET(w, 52) ? PRISMRV_USSE_FLAG_SYNCS : 0) |
             (GET(w, 55) ? PRISMRV_USSE_FLAG_SKIPINV : 0);
   field_reg(hw, BITS(w, 27, 22), 2, &d.dst.bank, &d.dst.reg);
   d.dst.mask = BITS(w, 42, 39);

   field_reg(src_bank_from_bits(GET(w, 30), GET(w, 31)), BITS(w, 11, 6), 2,
             &d.src[0].bank, &d.src[0].reg);
   d.src[0].neg = GET(w, 37);
   d.src[0].abs = GET(w, 38);
   for (unsigned c = 0; c < 4; c++) {
      unsigned v = 0;

      for (unsigned k = 0; k < 3; k++)
         v |= GET(w, swz1_bits[c][k]) << k;
      d.src[0].swz[c] = swz1_chars[v];
   }
   field_reg(src_bank_from_bits(GET(w, 28), GET(w, 29)), BITS(w, 5, 0), 2,
             &d.src[1].bank, &d.src[1].reg);
   d.src[1].abs = GET(w, 36);
   memcpy(d.src[1].swz, swz16[BITS(w, 47, 44)], 5);
   *out = d;
   return true;
}

bool
prismrv_usse_encode_vmov(const struct prismrv_usse_vmov *in, uint64_t *out)
{
   uint64_t w = UINT64_C(7) << 59;
   unsigned field, hw, o, pa, pb;
   int preset;

   if (in->type > PRISMRV_USSE_MOV_I32 || in->repeat < 1 || in->repeat > 4 ||
       (in->flags & ~15u) || !pred_to_bits(in->pred, true, &pb))
      return false;
   if (!in->dst.mask || in->dst.mask > PRISMRV_USSE_MASK_XYZW)
      return false;
   if (in->src.neg || in->src.abs)
      return false;                     /* vmov has no modifiers */
   preset = swz16_index(in->src.swz);
   if (preset < 0)
      return false;
   w |= (uint64_t)in->type << 40 | (uint64_t)(in->repeat - 1) << 44;
   if (in->flags & PRISMRV_USSE_FLAG_NOSCHED) w |= BIT64(43);
   if (in->flags & PRISMRV_USSE_FLAG_END)     w |= BIT64(50);
   if (in->flags & PRISMRV_USSE_FLAG_SYNCS)   w |= BIT64(52);
   if (in->flags & PRISMRV_USSE_FLAG_SKIPINV) w |= BIT64(55);
   w |= (uint64_t)pb << 56;

   if (!reg_field(in->dst.bank, in->dst.reg, 1, &field, &hw) ||
       !dst_bank_bits(hw, &w))
      return false;
   w |= (uint64_t)field << 18 | (uint64_t)in->dst.mask << 24;
   if (!reg_field(in->src.bank, in->src.reg, 1, &field, &hw) ||
       !src_bank_bits(hw, &o, &pa))
      return false;
   w |= (uint64_t)field << 6 | (uint64_t)o << 30 | (uint64_t)pa << 31 |
        (uint64_t)preset << 35;
   *out = w;
   return true;
}

bool
prismrv_usse_decode_vmov(uint64_t w, struct prismrv_usse_vmov *out)
{
   static const uint64_t must_be_zero =
      UINT64_C(0x3f) | (UINT64_C(0x3f) << 12) | BIT64(28) | BIT64(29) |
      BIT64(34) | BIT64(39) | BIT64(46) | BIT64(47) | BIT64(48) | BIT64(49) |
      BIT64(53) | BIT64(54);
   struct prismrv_usse_vmov d;
   unsigned hw;

   if (BITS(w, 63, 59) != 7 || (w & must_be_zero) || BITS(w, 42, 40) > 2)
      return false;
   memset(&d, 0, sizeof(d));
   if (!pred_from_bits(BITS(w, 58, 56), true, &d.pred) ||
       !dst_bank_from_bits(w, &hw))
      return false;
   d.type = BITS(w, 42, 40);
   d.repeat = BITS(w, 45, 44) + 1;
   d.flags = (GET(w, 43) ? PRISMRV_USSE_FLAG_NOSCHED : 0) |
             (GET(w, 50) ? PRISMRV_USSE_FLAG_END : 0) |
             (GET(w, 52) ? PRISMRV_USSE_FLAG_SYNCS : 0) |
             (GET(w, 55) ? PRISMRV_USSE_FLAG_SKIPINV : 0);
   field_reg(hw, BITS(w, 23, 18), 1, &d.dst.bank, &d.dst.reg);
   d.dst.mask = BITS(w, 27, 24);
   field_reg(src_bank_from_bits(GET(w, 30), GET(w, 31)), BITS(w, 11, 6), 1,
             &d.src.bank, &d.src.reg);
   memcpy(d.src.swz, swz16[BITS(w, 38, 35)], 5);
   *out = d;
   return true;
}
