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
 *   register number = 2 * field; b49/b48 = extended banks for src1/src2:
 *   (o,pa) = 10 c, 01 #imm (numbers 1:1); 00/11 = indexed r[il/ih+#n] (rejected)
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

/* vec2-group source bank: (ext, o bit, pa bit).  ext selects the extended
 * banks c / # (b49 for src1, b48 for src2); the indexed r[il/ih+#n] are not
 * modelled.  c and # numbers are 1:1, the others doubled. */
static bool
vec2_src_bank(unsigned bank, unsigned reg, unsigned *ext, unsigned *o,
              unsigned *pa, unsigned *field)
{
   if (bank == PRISMRV_USSE_BANK_C || bank == PRISMRV_USSE_BANK_IMM) {
      if (reg > 63)
         return false;
      *ext = 1;
      *o = bank == PRISMRV_USSE_BANK_C;
      *pa = bank == PRISMRV_USSE_BANK_IMM;
      *field = reg;
      return true;
   }
   unsigned hw;

   *ext = 0;
   return reg_field(bank, reg, 2, field, &hw) && src_bank_bits(hw, o, pa);
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

static bool
vec2_src_decode(unsigned ext, unsigned o, unsigned pa, unsigned field,
                struct prismrv_usse_src *s)
{
   if (!ext) {
      field_reg(src_bank_from_bits(o, pa), field, 2, &s->bank, &s->reg);
      return true;
   }
   if (o == pa)
      return false;               /* r[il+#n] / r[ih+#n]: indexed, not modelled */
   s->bank = o ? PRISMRV_USSE_BANK_C : PRISMRV_USSE_BANK_IMM;
   s->reg = field;
   return true;
}

bool
prismrv_usse_encode_vec2(const struct prismrv_usse_vec2 *in, uint64_t *out)
{
   /* measured sub-opcodes: vmul vadd vmin vmax vdsx vdsy (see prismrv analysis doc) */
   static const uint8_t sub[] = { 0, 1, 5, 6, 3, 4 };
   uint64_t w = BIT64(59);
   unsigned field, hw, o, pa, pb;

   if (in->op > PRISMRV_USSE_VDSY || (in->flags & ~7u) ||
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

      unsigned ext;

      if (!vec2_src_bank(s->bank, s->reg, &ext, &o, &pa, &field) || s->swz[4])
         return false;
      w |= (uint64_t)field << 6 | (uint64_t)o << 30 | (uint64_t)pa << 31 |
           (uint64_t)ext << 49;
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

      unsigned ext;

      if (s->neg || preset < 0 ||
          !vec2_src_bank(s->bank, s->reg, &ext, &o, &pa, &field))
         return false;
      w |= (uint64_t)field | (uint64_t)o << 28 | (uint64_t)pa << 29 |
           (uint64_t)preset << 44 | (uint64_t)ext << 48;
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

   if (BITS(w, 63, 59) != 1)
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

   if (!vec2_src_decode(GET(w, 49), GET(w, 30), GET(w, 31), BITS(w, 11, 6),
                        &d.src[0]))
      return false;
   d.src[0].neg = GET(w, 37);
   d.src[0].abs = GET(w, 38);
   for (unsigned c = 0; c < 4; c++) {
      unsigned v = 0;

      for (unsigned k = 0; k < 3; k++)
         v |= GET(w, swz1_bits[c][k]) << k;
      d.src[0].swz[c] = swz1_chars[v];
   }
   if (!vec2_src_decode(GET(w, 48), GET(w, 28), GET(w, 29), BITS(w, 5, 0),
                        &d.src[1]))
      return false;
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

/*
 * vpck (opcode 01000), verified scope: dst u8 <- src f16/f32, .scale, repeat 1
 *   src  reg [13:8] (x2), bank b30=o b31=pa (b49: c / #)
 *   src swizzle, 2 bits per char (0 x 1 y 2 z 3 w):
 *        c0 (b0,b1) [f16] or (b0,b7) [f32]; c1 (b16,b17); c2 (b14,b15); c3 (b19,b20)
 *   src2 (f32 only) reg [6:1] (x2), bank b28=o b29=pa (b48: c / #)
 *   dst  reg [27:21] (7 bits, 1:1), bank (b32,b33,b51) as vec2, mask b34..b37
 *   fmt  dst b38..b40 = 0 (u8), src b41..b43 = 5 (f16) / 6 (f32), scale b18
 *   flags end b50, syncs b52, nosched b54, skipinv b55; predicate [58:56]
 *   (0 none, 1 p0).  Bits 2-7, 28, 29, 48 are zero in the f16 form (the f32
 *   form uses them for src2); b53 and [47:44] are zero.
 */
static const uint8_t vpck_swz_lo[4] = { 0, 16, 14, 19 };

bool
prismrv_usse_encode_vpck(const struct prismrv_usse_vpck *in, uint64_t *out)
{
   uint64_t w = (UINT64_C(8) << 59) | BIT64(18);
   unsigned field, o, pa, ext;
   bool f32 = in->src_fmt == PRISMRV_USSE_PCK_F32;

   if ((in->src_fmt != PRISMRV_USSE_PCK_F16 && !f32) || (in->flags & ~15u) ||
       (in->pred != PRISMRV_USSE_PRED_NONE && in->pred != PRISMRV_USSE_PRED_P0))
      return false;
   w |= (uint64_t)in->src_fmt << 41;
   if (in->flags & PRISMRV_USSE_FLAG_NOSCHED) w |= BIT64(54);
   if (in->flags & PRISMRV_USSE_FLAG_END)     w |= BIT64(50);
   if (in->flags & PRISMRV_USSE_FLAG_SYNCS)   w |= BIT64(52);
   if (in->flags & PRISMRV_USSE_FLAG_SKIPINV) w |= BIT64(55);
   if (in->pred == PRISMRV_USSE_PRED_P0)      w |= BIT64(56);

   if (!in->dst.mask || in->dst.mask > PRISMRV_USSE_MASK_XYZW ||
       in->dst.bank == PRISMRV_USSE_BANK_I || in->dst.bank == PRISMRV_USSE_BANK_C ||
       in->dst.bank == PRISMRV_USSE_BANK_IMM || in->dst.reg > 127 ||
       (in->dst.bank == PRISMRV_USSE_BANK_R && in->dst.reg >= 60) ||
       !dst_bank_bits(in->dst.bank, &w))
      return false;
   w |= (uint64_t)in->dst.reg << 21 | (uint64_t)in->dst.mask << 34;

   if (in->src.neg || in->src.abs ||
       !vec2_src_bank(in->src.bank, in->src.reg, &ext, &o, &pa, &field))
      return false;
   w |= (uint64_t)field << 8 | (uint64_t)o << 30 | (uint64_t)pa << 31 |
        (uint64_t)ext << 49;
   for (unsigned c = 0; c < 4; c++) {
      const char *p = memchr("xyzw", in->src.swz[c], 4);
      unsigned v;

      if (!p || !in->src.swz[c])
         return false;
      v = p - "xyzw";
      if (f32 && c == 0)
         w |= (uint64_t)(v & 1) | (uint64_t)(v >> 1) << 7;
      else
         w |= (uint64_t)v << vpck_swz_lo[c];
   }
   if (in->src.swz[4])
      return false;
   if (f32) {
      if (in->src2.neg || in->src2.abs ||
          !vec2_src_bank(in->src2.bank, in->src2.reg, &ext, &o, &pa, &field))
         return false;
      w |= (uint64_t)field << 1 | (uint64_t)o << 28 | (uint64_t)pa << 29 |
           (uint64_t)ext << 48;
   }
   *out = w;
   return true;
}

bool
prismrv_usse_decode_vpck(uint64_t w, struct prismrv_usse_vpck *out)
{
   struct prismrv_usse_vpck d;
   unsigned hw, p;
   bool f32;

   if (BITS(w, 63, 59) != 8 || !GET(w, 18) || GET(w, 53) || BITS(w, 47, 44) ||
       BITS(w, 40, 38) != 0)
      return false;
   f32 = BITS(w, 43, 41) == 6;
   if (BITS(w, 43, 41) != 5 && !f32)
      return false;
   if (!f32 && (w & ((UINT64_C(0x3f) << 2) | BIT64(28) | BIT64(29) | BIT64(48))))
      return false;
   p = BITS(w, 58, 56);
   if (p > 1 || !dst_bank_from_bits(w, &hw) ||
       (hw == PRISMRV_USSE_BANK_R && BITS(w, 27, 21) >= 60))
      return false;
   memset(&d, 0, sizeof(d));
   d.src_fmt = f32 ? PRISMRV_USSE_PCK_F32 : PRISMRV_USSE_PCK_F16;
   d.pred = p ? PRISMRV_USSE_PRED_P0 : PRISMRV_USSE_PRED_NONE;
   d.flags = (GET(w, 54) ? PRISMRV_USSE_FLAG_NOSCHED : 0) |
             (GET(w, 50) ? PRISMRV_USSE_FLAG_END : 0) |
             (GET(w, 52) ? PRISMRV_USSE_FLAG_SYNCS : 0) |
             (GET(w, 55) ? PRISMRV_USSE_FLAG_SKIPINV : 0);
   d.dst.bank = hw;
   d.dst.reg = BITS(w, 27, 21);
   d.dst.mask = BITS(w, 37, 34);
   if (!vec2_src_decode(GET(w, 49), GET(w, 30), GET(w, 31), BITS(w, 13, 8), &d.src))
      return false;
   for (unsigned c = 0; c < 4; c++) {
      unsigned v = (f32 && c == 0) ? (GET(w, 0) | GET(w, 7) << 1)
                                   : BITS(w, vpck_swz_lo[c] + 1, vpck_swz_lo[c]);

      d.src.swz[c] = "xyzw"[v];
   }
   if (f32 && !vec2_src_decode(GET(w, 48), GET(w, 28), GET(w, 29), BITS(w, 6, 1),
                               &d.src2))
      return false;
   *out = d;
   return true;
}

/*
 * vrcp group (opcode 00110): vrcp / vrsq / vlog / vexp, f32 only
 *   op [42:41]; src reg [13:7] (x2, field 124..127 of bank r = i0..i3), bank
 *   b30=o b31=pa (b49: c / #, fields < 64), component [36:35], neg b37, abs b38;
 *   dst reg [27:21] (x2, 124..127 = i0..i3), bank (b32,b33,b51) as vec2,
 *   mask b0 = x, b1 = y; repeat [47:44] = count-1; nosched b43, end b50,
 *   syncs b52, skipinv b55; predicate [58:56] (0 none, 1 p0).
 *   Bits 2-6, 14-20, 28, 29, 34, 48 and the format bits [40:39], b53, b54 are 0.
 */
static bool
vr_reg_field(unsigned bank, unsigned reg, unsigned *field, unsigned *hw)
{
   if (bank == PRISMRV_USSE_BANK_I) {
      if (reg > 3)
         return false;
      *field = 124 + reg;
      *hw = PRISMRV_USSE_BANK_R;
      return true;
   }
   if (reg % 2 || reg / 2 >= (bank == PRISMRV_USSE_BANK_R ? 124u : 128u))
      return false;
   *field = reg / 2;
   *hw = bank;
   return true;
}

bool
prismrv_usse_encode_vrcp(const struct prismrv_usse_vrcp *in, uint64_t *out)
{
   uint64_t w = UINT64_C(6) << 59;
   unsigned field, hw, o, pa, ext = 0;

   if (in->op > PRISMRV_USSE_VEXP || in->repeat < 1 || in->repeat > 16 ||
       (in->flags & ~15u) || in->src_comp > 3 ||
       (in->pred != PRISMRV_USSE_PRED_NONE && in->pred != PRISMRV_USSE_PRED_P0))
      return false;
   w |= (uint64_t)in->op << 41 | (uint64_t)(in->repeat - 1) << 44 |
        (uint64_t)in->src_comp << 35;
   if (in->flags & PRISMRV_USSE_FLAG_NOSCHED) w |= BIT64(43);
   if (in->flags & PRISMRV_USSE_FLAG_END)     w |= BIT64(50);
   if (in->flags & PRISMRV_USSE_FLAG_SYNCS)   w |= BIT64(52);
   if (in->flags & PRISMRV_USSE_FLAG_SKIPINV) w |= BIT64(55);
   if (in->pred == PRISMRV_USSE_PRED_P0)      w |= BIT64(56);
   if (in->src.neg) w |= BIT64(37);
   if (in->src.abs) w |= BIT64(38);

   if (!in->dst.mask || in->dst.mask > PRISMRV_USSE_MASK_XY ||
       in->dst.bank == PRISMRV_USSE_BANK_C || in->dst.bank == PRISMRV_USSE_BANK_IMM ||
       !vr_reg_field(in->dst.bank, in->dst.reg, &field, &hw) ||
       !dst_bank_bits(hw, &w))
      return false;
   w |= (uint64_t)field << 21 | (uint64_t)in->dst.mask;

   if (in->src.bank == PRISMRV_USSE_BANK_C || in->src.bank == PRISMRV_USSE_BANK_IMM) {
      if (in->src.reg > 63)
         return false;
      ext = 1;
      o = in->src.bank == PRISMRV_USSE_BANK_C;
      pa = in->src.bank == PRISMRV_USSE_BANK_IMM;
      field = in->src.reg;
   } else if (!vr_reg_field(in->src.bank, in->src.reg, &field, &hw) ||
              !src_bank_bits(hw, &o, &pa)) {
      return false;
   }
   w |= (uint64_t)field << 7 | (uint64_t)o << 30 | (uint64_t)pa << 31 |
        (uint64_t)ext << 49;
   *out = w;
   return true;
}

bool
prismrv_usse_decode_vrcp(uint64_t w, struct prismrv_usse_vrcp *out)
{
   static const uint64_t must_be_zero =
      (UINT64_C(0x1f) << 2) | (UINT64_C(0x7f) << 14) | BIT64(28) | BIT64(29) |
      BIT64(34) | BIT64(48) | (UINT64_C(3) << 39) | BIT64(53) | BIT64(54);
   struct prismrv_usse_vrcp d;
   unsigned hw, p = BITS(w, 58, 56), f;

   if (BITS(w, 63, 59) != 6 || (w & must_be_zero) || p > 1 ||
       !dst_bank_from_bits(w, &hw))
      return false;
   memset(&d, 0, sizeof(d));
   d.op = BITS(w, 42, 41);
   d.repeat = BITS(w, 47, 44) + 1;
   d.pred = p ? PRISMRV_USSE_PRED_P0 : PRISMRV_USSE_PRED_NONE;
   d.flags = (GET(w, 43) ? PRISMRV_USSE_FLAG_NOSCHED : 0) |
             (GET(w, 50) ? PRISMRV_USSE_FLAG_END : 0) |
             (GET(w, 52) ? PRISMRV_USSE_FLAG_SYNCS : 0) |
             (GET(w, 55) ? PRISMRV_USSE_FLAG_SKIPINV : 0);
   f = BITS(w, 27, 21);
   if (hw == PRISMRV_USSE_BANK_R && f >= 124) {
      d.dst.bank = PRISMRV_USSE_BANK_I;
      d.dst.reg = f - 124;
   } else {
      d.dst.bank = hw;
      d.dst.reg = 2 * f;
   }
   d.dst.mask = BITS(w, 1, 0);
   f = BITS(w, 13, 7);
   if (GET(w, 49)) {
      if (GET(w, 30) == GET(w, 31) || f >= 64)
         return false;               /* il/ih indexed, or the g bank */
      d.src.bank = GET(w, 30) ? PRISMRV_USSE_BANK_C : PRISMRV_USSE_BANK_IMM;
      d.src.reg = f;
   } else {
      unsigned sb = src_bank_from_bits(GET(w, 30), GET(w, 31));

      if (sb == PRISMRV_USSE_BANK_R && f >= 124) {
         d.src.bank = PRISMRV_USSE_BANK_I;
         d.src.reg = f - 124;
      } else {
         d.src.bank = sb;
         d.src.reg = 2 * f;
      }
   }
   d.src.neg = GET(w, 37);
   d.src.abs = GET(w, 38);
   d.src_comp = BITS(w, 36, 35);
   *out = d;
   return true;
}

/*
 * The test group: an ALU operation whose result is tested into a predicate
 * instead of being written to a register.
 *
 * Verified against the vendor disassembler by reproducing a word the uKernel
 * contains and three further forms (analysis/NATIVE_USSE_COMPILER.md,
 * "Test-group encoding"):
 *
 *   op 48/cond 3  -> "and.testtaz.chan0 !c0, p0, r3, #8"  (= 0x48090181200C0188)
 *   op 14/cond 13 -> "vf16sub.testnanz.chan0 !c0, p0, r6, #8"   (the flt compare)
 *   op 14/cond 19 -> "vf16sub.testpaz.chan0 !c0, p0, r10, #8"  (fge)
 *   op 48/cond 5, chan1, p1 -> "and.testtanz.chan1 !c0, p1, r7, #9"
 *
 * The two-bit pairs are the parts a single-bit sweep cannot show: the immediate
 * second operand is bits 48|29, and addressing the condition operand in the c
 * bank is bits 32|51.  Both were found by diffing a hand-built word against the
 * uKernel's and then confirming the combination on other instructions.
 */
bool
prismrv_usse_encode_test(const struct prismrv_usse_test *in, uint64_t *out)
{
   uint64_t w;

   if (in->op > 63 || in->cond > 21 || in->chan > 2 || in->dst_pred > 2 ||
       in->src1 > 127 || in->src2 > 127)
      return false;

   w = (uint64_t)9 << 59;             /* instruction group */
   w |= (uint64_t)in->op << 14;
   w |= (uint64_t)in->cond << 39;
   w |= (uint64_t)in->chan << 36;
   w |= (uint64_t)in->dst_pred << 34;
   w |= (uint64_t)in->src1 << 7;
   w |= (uint64_t)in->src2;
   if (in->imm_form)
      w |= BIT64(48) | BIT64(29);     /* second source: immediate form */
   w |= BIT64(32) | BIT64(51);        /* condition operand: c bank */
   *out = w;
   return true;
}

/*
 * The sampler group.
 *
 * Field layout measured by sweeping the vendor compiler's own word
 * 0xE000000000090000 ("smp1d r0, r72, r0, drc0"):
 *
 *   [63:59] group (0b11100), [43:42] 1d/2d/3d, [27:21] destination,
 *   [20:16] coordinate / 8, [33:32] drc, [31:30] destination bank (o/pa),
 *   [39] destination in pa, [58:56] predicate, [52]/[54]/[55] the usual flags.
 *
 * The third operand the disassembler prints (r0 above) has no field here that
 * a sweep attributably moves, so it is emitted as zero; what it means is
 * recorded as unknown rather than invented.
 */
bool
prismrv_usse_encode_smp(const struct prismrv_usse_smp *in, uint64_t *out)
{
   uint64_t w = (uint64_t)0x1c << 59;
   unsigned pb;

   if (in->dim < 1 || in->dim > 3 || in->drc > 2 || (in->flags & ~7u) ||
       in->dst > 127 || (in->coord & 7u) || in->coord > 127 ||
       !pred_to_bits(in->pred, false, &pb))
      return false;
   if (in->dst_bank != PRISMRV_USSE_BANK_R &&
       in->dst_bank != PRISMRV_USSE_BANK_PA)
      return false;

   if (in->dim == 2)
      w |= BIT64(42);
   else if (in->dim == 3)
      w |= BIT64(43);
   w |= (uint64_t)in->dst << 21;
   w |= (uint64_t)(in->coord / 8) << 16;
   w |= (uint64_t)in->drc << 32;
   /* bit 39 moves the destination to pa; [31:30] belong to the third operand the
    * disassembler prints, whose meaning is not established, so only r and pa are
    * offered here and an output-bank destination is refused above. */
   if (in->dst_bank == PRISMRV_USSE_BANK_PA) w |= BIT64(39);
   w |= (uint64_t)pb << 56;
   if (in->flags & PRISMRV_USSE_FLAG_NOSCHED) w |= BIT64(54);
   if (in->flags & PRISMRV_USSE_FLAG_SYNCS)   w |= BIT64(52);
   if (in->flags & PRISMRV_USSE_FLAG_SKIPINV) w |= BIT64(55);
   *out = w;
   return true;
}
