/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * NIR -> native SGX544 USSE words.  See prismrv_usse_nir.h for the ABI
 * assumptions and the scope.
 *
 * Instruction selection (all forms verified against the vendor disassembler):
 *
 *   fadd a,b      vadd d, a, b                 sources: any bank
 *   fsub a,b      vadd d, -b, a                (negate exists on src1)
 *   fmul a,b      vmul d, a, b
 *   fneg a        vadd d, -a, -0.0
 *   fabs a        vadd d, |a|, -0.0
 *   mov/copy a    vmov.i32 d, a                bit-exact, any source bank
 *   ffma a,b,c    vmad d, a, b, c              (src1 = r/pa operand), else
 *                 vmul t,a,b ; vadd d,t,c      (two roundings, NOT fused)
 *   store_output  vmov.i32 o, src
 *   fmin/fmax     vmin/vmax d, a, b            (the vendor compiler emits the f16 forms
 *                 vf16min/vf16max for GLSL min/max/clamp; NaN behaviour of the
 *                 hardware is UNVERIFIED, NIR leaves it open for GLSL)
 *   frcp/frsq/flog2/fexp2   vrcp/vrsq/vlog/vexp d, a      (f32; the vendor's
 *                 lowering turns fdiv/fsqrt/fpow into these)
 *
 * The addend -0.0 (and not +0.0) keeps the sign of a zero result:
 * (-0) + (-0) = -0 and (+0) + (-0) = +0.
 *
 * Only VMAD still has the "source 1 reads r/pa only" restriction; vadd/vmul
 * and vmov read r/pa/sa/o freely, so constants and uniforms need no
 * special handling.
 */
#include "prismrv_usse_nir.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "compiler/glsl_types.h"
#include "util/macros.h"
#include "util/ralloc.h"

#include "prismrv_program.h"
#include "prismrv_usse_encode.h"

/*
 * Dense register model: a scalar value lives in register `reg` of its bank.
 * The vector instruction groups address registers in pairs (field * 2) and
 * select the component inside the pair with the write mask / swizzle, so a
 * scalar in register k is component (k & 1) of the pair at register (k & ~1).
 * vmov addresses registers 1:1.  (UNVERIFIED but consistent with all the
 * vendor code seen; see the header.)
 */
struct loc {
   uint8_t bank;     /* enum prismrv_usse_bank */
   uint8_t reg;      /* scalar register number */
   bool valid;
};

static unsigned loc_base(struct loc l) { return l.reg & ~1u; }
static unsigned loc_comp(struct loc l) { return l.reg & 1u; }

struct opnd {
   struct loc l;
   bool neg, abs;
};

struct ctx {
   struct prismrv_usse_binary *out;
   struct loc color[4];       /* FS colour components, packed at the end */
   struct loc *locs;          /* [def index * 4 + component] */
   unsigned num_defs;
   unsigned next_r;           /* next free temporary slot */
   unsigned pool_next;        /* next free sa slot */
   unsigned vary_slots;
   bool failed;
};

static void
fail(struct ctx *c, const char *fmt, ...)
{
   va_list ap;

   if (!c->failed) {
      fprintf(stderr, "prismrv(native): ");
      va_start(ap, fmt);
      vfprintf(stderr, fmt, ap);
      va_end(ap);
      fprintf(stderr, "\n");
   }
   c->failed = true;
}

static struct loc
mkloc(unsigned bank, unsigned reg)
{
   return (struct loc){ .bank = bank, .reg = reg, .valid = true };
}

static struct opnd
plain(struct loc l)
{
   return (struct opnd){ .l = l };
}

/* source 1 of a VMAD can only read r and pa (verified) */
static bool
src1_ok(struct loc l)
{
   return l.bank == PRISMRV_USSE_BANK_R || l.bank == PRISMRV_USSE_BANK_PA;
}

static bool
emit_vmad(struct ctx *c, struct loc dst, struct opnd s1, struct opnd s2,
          struct opnd s3)
{
   struct prismrv_usse_vmad in = { 0 };
   struct opnd s[3] = { s1, s2, s3 };
   uint64_t w;

   if (c->failed)
      return false;
   if (c->out->num_words >= PRISMRV_USSE_MAX_WORDS) {
      fail(c, "shader exceeds %u instructions", PRISMRV_USSE_MAX_WORDS);
      return false;
   }
   in.dst.bank = dst.bank;
   in.dst.reg = loc_base(dst);
   in.dst.mask = loc_comp(dst) ? PRISMRV_USSE_MASK_Y : PRISMRV_USSE_MASK_X;
   for (unsigned i = 0; i < 3; i++) {
      in.src[i].bank = s[i].l.bank;
      in.src[i].reg = loc_base(s[i].l);
      in.src[i].neg = s[i].neg;
      in.src[i].abs = s[i].abs;
      memcpy(in.src[i].swz, loc_comp(s[i].l) ? "yyyy" : "xxxx", 5);
   }
   if (!prismrv_usse_encode_vmad(&in, &w)) {
      fail(c, "internal: vmad not encodable (dst %u:%u)", dst.bank, dst.reg);
      return false;
   }
   c->out->code[c->out->num_words++] = w;
   return true;
}

static bool
emit_vec2(struct ctx *c, unsigned op, struct loc dst, struct opnd s1,
          struct opnd s2)
{
   struct prismrv_usse_vec2 in = { 0 };
   struct opnd s[2] = { s1, s2 };
   uint64_t w;

   if (c->failed)
      return false;
   if (c->out->num_words >= PRISMRV_USSE_MAX_WORDS) {
      fail(c, "shader exceeds %u instructions", PRISMRV_USSE_MAX_WORDS);
      return false;
   }
   in.op = op;
   in.dst.bank = dst.bank;
   in.dst.reg = loc_base(dst);
   in.dst.mask = loc_comp(dst) ? PRISMRV_USSE_MASK_Y : PRISMRV_USSE_MASK_X;
   for (unsigned i = 0; i < 2; i++) {
      in.src[i].bank = s[i].l.bank;
      in.src[i].reg = loc_base(s[i].l);
      in.src[i].neg = s[i].neg;
      in.src[i].abs = s[i].abs;
      memcpy(in.src[i].swz, loc_comp(s[i].l) ? "yyyy" : "xxxx", 5);
   }
   if (!prismrv_usse_encode_vec2(&in, &w)) {
      fail(c, "internal: vec2 op not encodable (dst %u:%u)", dst.bank, dst.reg);
      return false;
   }
   c->out->code[c->out->num_words++] = w;
   return true;
}

static bool
emit_vmov_pred(struct ctx *c, struct loc dst, struct loc src, uint8_t pred)
{
   struct prismrv_usse_vmov in = { .type = PRISMRV_USSE_MOV_I32, .repeat = 1,
                                   .pred = pred };
   uint64_t w;

   if (c->failed)
      return false;
   if (c->out->num_words >= PRISMRV_USSE_MAX_WORDS) {
      fail(c, "shader exceeds %u instructions", PRISMRV_USSE_MAX_WORDS);
      return false;
   }
   in.dst.bank = dst.bank;
   in.dst.reg = dst.reg;
   in.dst.mask = PRISMRV_USSE_MASK_X;
   in.src.bank = src.bank;
   in.src.reg = src.reg;
   memcpy(in.src.swz, "xxxx", 5);
   if (!prismrv_usse_encode_vmov(&in, &w)) {
      fail(c, "internal: vmov not encodable (dst %u:%u src %u:%u)", dst.bank,
           dst.reg, src.bank, src.reg);
      return false;
   }
   c->out->code[c->out->num_words++] = w;
   return true;
}

static bool
emit_vmov(struct ctx *c, struct loc dst, struct loc src)
{
   return emit_vmov_pred(c, dst, src, PRISMRV_USSE_PRED_NONE);
}

/*
 * Emit a comparison: vf16sub with a test condition, writing p0.
 *
 * "a < b" is the difference tested for negative-and-non-zero; the condition values are the ones
 * measured by sweeping the field (analysis/NATIVE_USSE_COMPILER.md, "Test-group encoding").
 */
static bool
emit_compare(struct ctx *c, struct loc a, struct loc b, unsigned cond)
{
   struct prismrv_usse_test in = { .op = PRISMRV_USSE_TEST_VF16SUB,
                                   .cond = cond, .chan = 0, .dst_pred = 0,
                                   .src1 = loc_base(a), .src2 = loc_base(b),
                                   .imm_form = false };
   uint64_t w;

   if (c->failed)
      return false;
   if (c->out->num_words >= PRISMRV_USSE_MAX_WORDS) {
      fail(c, "shader exceeds %u instructions", PRISMRV_USSE_MAX_WORDS);
      return false;
   }
   if (!prismrv_usse_encode_test(&in, &w)) {
      fail(c, "internal: comparison not encodable");
      return false;
   }
   c->out->code[c->out->num_words++] = w;
   return true;
}

/* the test condition for a comparison op, or 0 when @op is not one */
static unsigned
compare_condition(nir_op op)
{
   switch (op) {
   case nir_op_flt: return PRISMRV_USSE_TEST_NANZ;   /* a - b <  0 */
   case nir_op_fge: return PRISMRV_USSE_TEST_PAZ;    /* a - b >= 0 */
   case nir_op_feq: return PRISMRV_USSE_TEST_TAZ;    /* a - b == 0 */
   case nir_op_fneu: return PRISMRV_USSE_TEST_TANZ;   /* a - b != 0 */
   default: return 0;
   }
}

static bool
emit_vrcp(struct ctx *c, unsigned op, struct loc dst, struct loc src)
{
   struct prismrv_usse_vrcp in = { .op = op, .repeat = 1 };
   uint64_t w;

   if (c->failed)
      return false;
   if (c->out->num_words >= PRISMRV_USSE_MAX_WORDS) {
      fail(c, "shader exceeds %u instructions", PRISMRV_USSE_MAX_WORDS);
      return false;
   }
   in.dst.bank = dst.bank;
   in.dst.reg = loc_base(dst);
   in.dst.mask = loc_comp(dst) ? PRISMRV_USSE_MASK_Y : PRISMRV_USSE_MASK_X;
   in.src.bank = src.bank;
   in.src.reg = loc_base(src);
   in.src_comp = loc_comp(src);
   if (!prismrv_usse_encode_vrcp(&in, &w)) {
      fail(c, "internal: vrcp not encodable (dst %u:%u src %u:%u)", dst.bank,
           dst.reg, src.bank, src.reg);
      return false;
   }
   c->out->code[c->out->num_words++] = w;
   return true;
}

static struct loc
alloc_temp(struct ctx *c)
{
   if (c->next_r >= PRISMRV_USSE_R_SLOTS) {
      fail(c, "out of temporaries (%u scalar registers)",
           PRISMRV_USSE_R_SLOTS);
      return (struct loc){ 0 };
   }
   return mkloc(PRISMRV_USSE_BANK_R, c->next_r++);
}

/* four consecutive temporaries starting at an even register */
static struct loc
alloc_temp_block4(struct ctx *c)
{
   c->next_r = ALIGN_POT(c->next_r, 2);
   if (c->next_r + 4 > PRISMRV_USSE_R_SLOTS) {
      fail(c, "out of temporaries for the colour block");
      return (struct loc){ 0 };
   }
   c->next_r += 4;
   return mkloc(PRISMRV_USSE_BANK_R, c->next_r - 4);
}

/* sa slot holding the 32-bit literal @bits (deduplicated) */
static struct loc
sa_const(struct ctx *c, uint32_t bits)
{
   struct prismrv_usse_binary *o = c->out;

   for (unsigned s = o->sa_pool_first; s < c->pool_next; s++)
      if (o->sa_image[s] == bits)
         return mkloc(PRISMRV_USSE_BANK_SA, s);
   if (c->pool_next >= PRISMRV_USSE_SA_SLOTS) {
      fail(c, "constant/uniform pool full (%u secondary-attribute slots)",
           PRISMRV_USSE_SA_SLOTS);
      return (struct loc){ 0 };
   }
   o->sa_image[c->pool_next] = bits;
   return mkloc(PRISMRV_USSE_BANK_SA, c->pool_next++);
}

static struct loc
src_loc(struct ctx *c, const nir_alu_instr *alu, unsigned src, unsigned comp)
{
   const nir_def *def = alu->src[src].src.ssa;
   struct loc l = c->locs[def->index * 4 + alu->src[src].swizzle[comp]];

   if (!l.valid)
      fail(c, "internal: use of an undefined SSA component");
   return l;
}

static void
set_loc(struct ctx *c, const nir_def *def, unsigned comp, struct loc l)
{
   if (def->index >= c->num_defs || comp >= 4) {
      fail(c, "internal: SSA index/component out of range");
      return;
   }
   c->locs[def->index * 4 + comp] = l;
}

static void
emit_alu(struct ctx *c, nir_alu_instr *alu)
{
   const nir_op_info *info = &nir_op_infos[alu->op];
   unsigned n = alu->def.num_components;

   if (alu->def.bit_size != 32) {
      fail(c, "ALU op '%s' with %u-bit result", info->name,
           alu->def.bit_size);
      return;
   }
   for (unsigned s = 0; s < info->num_inputs; s++)
      if (alu->src[s].src.ssa->bit_size != 32) {
         fail(c, "ALU op '%s' with non-32-bit source", info->name);
         return;
      }

   switch (alu->op) {
   case nir_op_vec2:
   case nir_op_vec3:
   case nir_op_vec4:
      /* pure renaming: component ch simply lives where source ch lives */
      for (unsigned ch = 0; ch < n; ch++)
         set_loc(c, &alu->def, ch, src_loc(c, alu, ch, 0));
      return;
   case nir_op_mov:
   case nir_op_fneg:
   case nir_op_fabs:
   case nir_op_fadd:
   case nir_op_fsub:
   case nir_op_fmul:
   case nir_op_fmin:
   case nir_op_fmax:
   case nir_op_ffma:
   case nir_op_frcp:
   case nir_op_frsq:
   case nir_op_flog2:
   case nir_op_fexp2:
   case nir_op_bcsel:
      break;
   default:
      fail(c, "unsupported NIR ALU op '%s' (no verified USSE form yet)",
           info->name);
      return;
   }

   for (unsigned ch = 0; ch < n && !c->failed; ch++) {
      struct loc a = src_loc(c, alu, 0, ch), b = { 0 }, cc = { 0 };
      struct loc d, negzero = { 0 };

      if (info->num_inputs > 1)
         b = src_loc(c, alu, 1, ch);
      if (info->num_inputs > 2)
         cc = src_loc(c, alu, 2, ch);
      if (c->failed)
         return;

      if (alu->op == nir_op_fneg || alu->op == nir_op_fabs) {
         negzero = sa_const(c, 0x80000000u);
         if (c->failed)
            return;
      }
      if (alu->op == nir_op_bcsel) {
         /*
          * A select is the predicate the comparison wrote plus two predicated copies.  The
          * comparison is emitted here rather than where its own instruction appears, because the
          * emitter only models p0: emitting it inline means nothing can clobber it between the test
          * and the select.  A comparison whose result is used any other way still fails the compile
          * rather than producing a value.
          */
         nir_alu_instr *cmp = nir_def_as_alu(alu->src[0].src.ssa);
         unsigned cond = cmp ? compare_condition(cmp->op) : 0;

         if (!cond) {
            fail(c, "select whose condition is not a supported comparison");
            return;
         }
         if (!emit_compare(c, src_loc(c, cmp, 0, ch), src_loc(c, cmp, 1, ch), cond))
            return;
         d = alloc_temp(c);
         if (c->failed)
            return;
         if (!emit_vmov_pred(c, d, src_loc(c, alu, 1, ch), PRISMRV_USSE_PRED_P0) ||
             !emit_vmov_pred(c, d, src_loc(c, alu, 2, ch), PRISMRV_USSE_PRED_NOT_P0))
            return;
         set_loc(c, &alu->def, ch, d);
         continue;
      }

      d = alloc_temp(c);
      if (c->failed)
         return;

      switch (alu->op) {
      case nir_op_mov:
         emit_vmov(c, d, a);
         break;
      case nir_op_frcp:
         emit_vrcp(c, PRISMRV_USSE_VRCP, d, a);
         break;
      case nir_op_frsq:
         emit_vrcp(c, PRISMRV_USSE_VRSQ, d, a);
         break;
      case nir_op_flog2:
         emit_vrcp(c, PRISMRV_USSE_VLOG, d, a);
         break;
      case nir_op_fexp2:
         emit_vrcp(c, PRISMRV_USSE_VEXP, d, a);
         break;
      case nir_op_fneg: {
         struct opnd s1 = plain(a);
         s1.neg = true;
         emit_vec2(c, PRISMRV_USSE_VADD, d, s1, plain(negzero));
         break;
      }
      case nir_op_fabs: {
         struct opnd s1 = plain(a);
         s1.abs = true;
         emit_vec2(c, PRISMRV_USSE_VADD, d, s1, plain(negzero));
         break;
      }
      case nir_op_fadd:
         emit_vec2(c, PRISMRV_USSE_VADD, d, plain(a), plain(b));
         break;
      case nir_op_fsub: {
         struct opnd s1 = plain(b);
         s1.neg = true;
         emit_vec2(c, PRISMRV_USSE_VADD, d, s1, plain(a));   /* -b + a */
         break;
      }
      case nir_op_fmul:
         emit_vec2(c, PRISMRV_USSE_VMUL, d, plain(a), plain(b));
         break;
      case nir_op_fmin:
         emit_vec2(c, PRISMRV_USSE_VMIN, d, plain(a), plain(b));
         break;
      case nir_op_fmax:
         emit_vec2(c, PRISMRV_USSE_VMAX, d, plain(a), plain(b));
         break;
      case nir_op_ffma:
         if (src1_ok(a)) {
            emit_vmad(c, d, plain(a), plain(b), plain(cc));
         } else if (src1_ok(b)) {
            emit_vmad(c, d, plain(b), plain(a), plain(cc));
         } else {
            /* VMAD source 1 cannot read sa/o: unfused mul + add */
            struct loc t = alloc_temp(c);

            emit_vec2(c, PRISMRV_USSE_VMUL, t, plain(a), plain(b));
            emit_vec2(c, PRISMRV_USSE_VADD, d, plain(t), plain(cc));
         }
         break;
      default:
         UNREACHABLE("filtered above");
      }
      set_loc(c, &alu->def, ch, d);
   }
}

static bool
const_slot(struct ctx *c, nir_src *src, unsigned *slot, const char *what)
{
   if (!nir_src_is_const(*src)) {
      fail(c, "indirect addressing in %s", what);
      return false;
   }
   *slot = nir_src_as_uint(*src);
   return true;
}

static void
emit_intrinsic(struct ctx *c, nir_intrinsic_instr *intr, mesa_shader_stage stage)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_load_input: {
      const nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
      unsigned off, vec;
      unsigned comp = nir_intrinsic_component(intr);

      if (intr->def.bit_size != 32) {
         fail(c, "non-32-bit load_input");
         return;
      }
      if (!const_slot(c, &intr->src[0], &off, "load_input"))
         return;
      if (stage == MESA_SHADER_VERTEX) {
         vec = nir_intrinsic_base(intr) + off;
      } else if (sem.location >= VARYING_SLOT_VAR0) {
         vec = sem.location - VARYING_SLOT_VAR0 + off;
      } else if (sem.location == VARYING_SLOT_COL0) {
         vec = off;
      } else {
         fail(c, "fragment input slot %u unsupported", sem.location);
         return;
      }
      if (4 * vec + comp + intr->def.num_components > 64) {
         fail(c, "input %u beyond the primary attribute file", vec);
         return;
      }
      for (unsigned i = 0; i < intr->def.num_components; i++)
         set_loc(c, &intr->def, i,
                 mkloc(PRISMRV_USSE_BANK_PA, PRISMRV_USSE_PA_REG(vec, comp + i)));
      break;
   }

   case nir_intrinsic_ddx:
   case nir_intrinsic_ddx_fine:
   case nir_intrinsic_ddy:
   case nir_intrinsic_ddy_fine: {
      const nir_def *src = intr->src[0].ssa;
      unsigned n = intr->def.num_components;
      unsigned op = (intr->intrinsic == nir_intrinsic_ddx ||
                     intr->intrinsic == nir_intrinsic_ddx_fine)
                    ? PRISMRV_USSE_VDSX : PRISMRV_USSE_VDSY;

      if (intr->def.bit_size != 32) {
         fail(c, "non-32-bit derivative");
         return;
      }
      for (unsigned ch = 0; ch < n && !c->failed; ch++) {
         struct loc a = c->locs[src->index * 4 + ch], d;

         if (!a.valid) {
            fail(c, "internal: derivative of an undefined source");
            return;
         }
         d = alloc_temp(c);
         if (c->failed)
            return;
         if (!emit_vec2(c, op, d, plain(a), plain(a)))
            return;
         set_loc(c, &intr->def, ch, d);
      }
      break;
   }

   case nir_intrinsic_load_uniform: {
      unsigned off, first;
      unsigned n = intr->def.num_components;

      if (intr->def.bit_size != 32) {
         fail(c, "non-32-bit load_uniform");
         return;
      }
      if (!const_slot(c, &intr->src[0], &off, "load_uniform"))
         return;
      off += nir_intrinsic_base(intr);
      first = c->out->sa_uniform_first + off / 4;
      if (off % 4 || off / 4 + n > c->out->sa_uniform_count) {
         fail(c, "uniform access at byte %u out of range", off);
         return;
      }
      for (unsigned i = 0; i < n; i++)
         set_loc(c, &intr->def, i, mkloc(PRISMRV_USSE_BANK_SA, first + i));
      break;
   }

   case nir_intrinsic_store_output: {
      const nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
      const nir_def *val = intr->src[0].ssa;
      unsigned mask = nir_intrinsic_write_mask(intr);
      unsigned comp = nir_intrinsic_component(intr);
      unsigned off, oslot;

      if (val->bit_size != 32) {
         fail(c, "non-32-bit store_output");
         return;
      }
      if (!const_slot(c, &intr->src[1], &off, "store_output"))
         return;

      if (stage == MESA_SHADER_VERTEX) {
         if (sem.location == VARYING_SLOT_POS)
            oslot = 0;
         else if (sem.location >= VARYING_SLOT_VAR0)
            oslot = 4 + 4 * (sem.location - VARYING_SLOT_VAR0 + off);
         else if (sem.location == VARYING_SLOT_COL0)
            oslot = 4 + 4 * off;
         else {
            fail(c, "vertex output slot %u unsupported", sem.location);
            return;
         }
      } else {
         if ((sem.location != FRAG_RESULT_COLOR &&
              sem.location != FRAG_RESULT_DATA0) || off) {
            fail(c, "fragment output slot %u unsupported", sem.location);
            return;
         }
         oslot = 0;
      }

      for (unsigned i = 0; i < val->num_components; i++) {
         struct loc src = c->locs[val->index * 4 + i];
         unsigned slot = oslot + comp + i;

         if (!(mask & (1u << i)))
            continue;
         if (!src.valid) {
            fail(c, "internal: store of an undefined value");
            return;
         }
         if (stage == MESA_SHADER_FRAGMENT) {
            if (comp + i > 3) {
               fail(c, "fragment colour component %u", comp + i);
               return;
            }
            c->color[comp + i] = src;       /* packed in the epilogue */
            continue;
         }
         if (slot >= 64) {
            fail(c, "output slot %u out of range", slot);
            return;
         }
         emit_vmov(c, mkloc(PRISMRV_USSE_BANK_O, slot), src);
      }
      break;
   }

   default:
      fail(c, "unsupported intrinsic '%s'",
           nir_intrinsic_infos[intr->intrinsic].name);
      break;
   }
}

/*
 * FS epilogue: gather the four colour components in four consecutive
 * registers, pack them to RGBA8 into o0 (vpcku8f32, .end) and close the
 * program like the vendor's single-phase pixel program does (nop.end).
 */
static void
emit_fs_epilogue(struct ctx *c)
{
   struct prismrv_usse_vpck p = { .src_fmt = PRISMRV_USSE_PCK_F32,
                                  .flags = PRISMRV_USSE_FLAG_END };
   struct loc blk;
   uint64_t w;

   for (unsigned i = 0; i < 4; i++)
      if (!c->color[i].valid) {
         fail(c, "fragment colour component %u is never written", i);
         return;
      }
   blk = alloc_temp_block4(c);
   for (unsigned i = 0; i < 4 && !c->failed; i++)
      emit_vmov(c, mkloc(PRISMRV_USSE_BANK_R, blk.reg + i), c->color[i]);
   if (c->failed)
      return;
   p.dst = (struct prismrv_usse_dst){ PRISMRV_USSE_BANK_O, 0, PRISMRV_USSE_MASK_XYZW };
   p.src = (struct prismrv_usse_src){ .bank = PRISMRV_USSE_BANK_R, .reg = blk.reg };
   memcpy(p.src.swz, "xyzw", 5);
   p.src2 = (struct prismrv_usse_src){ .bank = PRISMRV_USSE_BANK_R, .reg = blk.reg + 2 };
   if (!prismrv_usse_encode_vpck(&p, &w)) {
      fail(c, "internal: vpck not encodable");
      return;
   }
   if (c->out->num_words + 2 > PRISMRV_USSE_MAX_WORDS) {
      fail(c, "shader exceeds %u instructions", PRISMRV_USSE_MAX_WORDS);
      return;
   }
   c->out->code[c->out->num_words++] = w;
   c->out->code[c->out->num_words++] = PRISMRV_USSE_NOP_END;
}

/* size of the uniform block in 32-bit slots; false if it cannot be known */
static bool
scan_uniforms(nir_function_impl *impl, unsigned *slots)
{
   unsigned max = 0;

   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         nir_intrinsic_instr *i;
         unsigned off, end;

         if (instr->type != nir_instr_type_intrinsic)
            continue;
         i = nir_instr_as_intrinsic(instr);
         if (i->intrinsic != nir_intrinsic_load_uniform)
            continue;
         if (!nir_src_is_const(i->src[0]))
            return false;
         off = nir_intrinsic_base(i) + nir_src_as_uint(i->src[0]);
         end = DIV_ROUND_UP(off, 4) + i->def.num_components;
         max = MAX2(max, end);
      }
   }
   *slots = max;
   return true;
}

bool
prismrv_nir_to_usse_native(nir_shader *orig, struct prismrv_usse_binary *out)
{
   nir_shader *nir;
   nir_function_impl *impl;
   struct ctx c = { 0 };
   unsigned uniforms = 0;

   memset(out, 0, sizeof(*out));
   if (orig->info.stage != MESA_SHADER_VERTEX &&
       orig->info.stage != MESA_SHADER_FRAGMENT) {
      fprintf(stderr, "prismrv(native): unsupported stage %s\n",
              mesa_shader_stage_name(orig->info.stage));
      return false;
   }

   nir = nir_shader_clone(NULL, orig);
   if (!nir)
      return false;
   prismrv_nir_lower_for_usse(nir);
   impl = nir_shader_get_entrypoint(nir);

   if (exec_list_length(&nir->functions) != 1 ||
       !exec_list_is_singular(&impl->body) ||
       exec_node_data(nir_cf_node, exec_list_get_head(&impl->body),
                      node)->type != nir_cf_node_block) {
      fprintf(stderr, "prismrv(native): shader has control flow; "
                      "not supported\n");
      ralloc_free(nir);
      return false;
   }
   if (!scan_uniforms(impl, &uniforms) ||
       PRISMRV_USSE_SA_UNIFORM0 + uniforms > PRISMRV_USSE_SA_SLOTS) {
      fprintf(stderr, "prismrv(native): uniform block does not fit the "
                      "secondary attribute file\n");
      ralloc_free(nir);
      return false;
   }

   out->stage = nir->info.stage;
   out->sa_uniform_first = PRISMRV_USSE_SA_UNIFORM0;
   out->sa_uniform_count = uniforms;
   out->sa_pool_first = PRISMRV_USSE_SA_UNIFORM0 + uniforms;

   c.out = out;
   c.num_defs = impl->ssa_alloc;
   c.locs = rzalloc_array(NULL, struct loc, MAX2(c.num_defs, 1) * 4);
   c.pool_next = out->sa_pool_first;

   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      out->code[out->num_words++] = PRISMRV_USSE_PHAS_END_PIXEL;
      out->code[out->num_words++] = PRISMRV_USSE_PHAS_PIXEL;
   } else {
      out->code[out->num_words++] = PRISMRV_USSE_PHAS_PIXEL;
   }

   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         switch (instr->type) {
         case nir_instr_type_load_const: {
            nir_load_const_instr *lc = nir_instr_as_load_const(instr);

            if (lc->def.bit_size != 32) {
               fail(&c, "%u-bit load_const", lc->def.bit_size);
               break;
            }
            for (unsigned i = 0; i < lc->def.num_components; i++)
               set_loc(&c, &lc->def, i, sa_const(&c, lc->value[i].u32));
            break;
         }
         case nir_instr_type_undef: {
            nir_undef_instr *u = nir_instr_as_undef(instr);

            for (unsigned i = 0; i < u->def.num_components; i++)
               set_loc(&c, &u->def, i, sa_const(&c, 0));
            break;
         }
         case nir_instr_type_alu:
            emit_alu(&c, nir_instr_as_alu(instr));
            break;
         case nir_instr_type_intrinsic:
            emit_intrinsic(&c, nir_instr_as_intrinsic(instr),
                           nir->info.stage);
            break;
         case nir_instr_type_tex:
            fail(&c, "texture sampling (smp) has no verified encoding yet");
            break;
         default:
            fail(&c, "unsupported NIR instruction type %d", instr->type);
            break;
         }
         if (c.failed)
            break;
      }
      if (c.failed)
         break;
   }

   if (!c.failed && nir->info.stage == MESA_SHADER_FRAGMENT)
      emit_fs_epilogue(&c);
   else if (!c.failed) {
      if (out->num_words + 1 > PRISMRV_USSE_MAX_WORDS)
         fail(&c, "shader exceeds %u instructions", PRISMRV_USSE_MAX_WORDS);
      else
         out->code[out->num_words++] = PRISMRV_USSE_EMITVTX_END;
   }

   out->num_r_slots = c.next_r;
   out->sa_pool_count = c.pool_next - out->sa_pool_first;
   ralloc_free(c.locs);
   ralloc_free(nir);
   return !c.failed;
}

static const char *const bank_name[] = { "r", "o", "pa", "sa", "i", "c", "#" };

static int
fmt_src(char *buf, unsigned size, const struct prismrv_usse_src *s)
{
   return snprintf(buf, size, ", %s%s%s%u.%s%s", s->neg ? "-" : "",
                   s->abs ? "|" : "", bank_name[s->bank], s->reg, s->swz,
                   s->abs ? "|" : "");
}

static const char *
mask_str(unsigned m, char *tmp)
{
   unsigned n = 0;

   for (unsigned i = 0; i < 4; i++)
      if (m & (1u << i))
         tmp[n++] = "xyzw"[i];
   tmp[n] = 0;
   return tmp;
}

void
prismrv_usse_disasm(uint64_t word, char *buf, unsigned size)
{
   struct prismrv_usse_vmad v;
   struct prismrv_usse_vec2 v2;
   struct prismrv_usse_vmov vm;
   struct prismrv_usse_vpck pk;
   struct prismrv_usse_vrcp vr;
   char mk[8];
   int n;

   if (prismrv_usse_decode_vmad(word, &v)) {
      n = snprintf(buf, size, "vmad %s%u.%s%s", bank_name[v.dst.bank], v.dst.reg,
                   mask_str(v.dst.mask, mk),
                   v.pred ? (v.pred == PRISMRV_USSE_PRED_P0 ? " (p0)" : " (!p0)") : "");
      for (unsigned i = 0; i < 3 && n > 0 && (unsigned)n < size; i++)
         n += fmt_src(buf + n, size - n, &v.src[i]);
   } else if (prismrv_usse_decode_vec2(word, &v2)) {
      static const char *const op[] = { "vmul", "vadd", "vmin", "vmax" };

      n = snprintf(buf, size, "%s %s%u.%s", op[v2.op], bank_name[v2.dst.bank],
                   v2.dst.reg, mask_str(v2.dst.mask, mk));
      for (unsigned i = 0; i < 2 && n > 0 && (unsigned)n < size; i++)
         n += fmt_src(buf + n, size - n, &v2.src[i]);
   } else if (word == PRISMRV_USSE_PHAS_END_PIXEL) {
      snprintf(buf, size, "phas.end #0, #0, pixel, end, parallel");
   } else if (word == PRISMRV_USSE_PHAS_PIXEL) {
      snprintf(buf, size, "phas #0, #0, pixel, end, parallel");
   } else if (word == PRISMRV_USSE_NOP_END) {
      snprintf(buf, size, "nop.end");
   } else if (word == PRISMRV_USSE_EMITVTX_END) {
      snprintf(buf, size, "emitvtx.end.freep #0, #0");
   } else if (prismrv_usse_decode_vpck(word, &pk)) {
      n = snprintf(buf, size, "vpcku8f%s%s.scale %s%u.%s",
                   pk.src_fmt == PRISMRV_USSE_PCK_F32 ? "32" : "16",
                   pk.flags & PRISMRV_USSE_FLAG_END ? ".end" : "",
                   bank_name[pk.dst.bank], pk.dst.reg, mask_str(pk.dst.mask, mk));
      if (n > 0 && (unsigned)n < size)
         n += fmt_src(buf + n, size - n, &pk.src);
      if (pk.src_fmt == PRISMRV_USSE_PCK_F32 && n > 0 && (unsigned)n < size)
         snprintf(buf + n, size - n, ", %s%u", bank_name[pk.src2.bank], pk.src2.reg);
   } else if (prismrv_usse_decode_vrcp(word, &vr)) {
      static const char *const op[] = { "vrcp", "vrsq", "vlog", "vexp" };

      n = snprintf(buf, size, "%s %s%u.%s, %s%s%s%u.%u%s", op[vr.op],
                   bank_name[vr.dst.bank], vr.dst.reg, mask_str(vr.dst.mask, mk),
                   vr.src.neg ? "-" : "", vr.src.abs ? "|" : "",
                   bank_name[vr.src.bank], vr.src.reg, vr.src_comp,
                   vr.src.abs ? "|" : "");
   } else if (prismrv_usse_decode_vmov(word, &vm)) {
      static const char *const ty[] = { "i8", "i16", "i32" };

      n = snprintf(buf, size, "vmov.%s%s %s%u.%s", ty[vm.type],
                   vm.flags & PRISMRV_USSE_FLAG_END ? ".end" : "",
                   bank_name[vm.dst.bank], vm.dst.reg, mask_str(vm.dst.mask, mk));
      if (n > 0 && (unsigned)n < size)
         fmt_src(buf + n, size - n, &vm.src);
   } else {
      snprintf(buf, size, ".word 0x%016llx", (unsigned long long)word);
   }
}
