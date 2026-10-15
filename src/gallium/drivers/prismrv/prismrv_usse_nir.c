/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * NIR -> native SGX544 USSE words.  See prismrv_usse_nir.h for the ABI
 * assumptions and the scope.
 *
 * Instruction selection (all through the verified plain VMAD,
 * dst = src1 * src2 + src3):
 *
 *   fmul a,b      vmad d, a,  b,    -0.0
 *   fadd a,b      vmad d, a,  1.0,  b
 *   fsub a,b      vmad d, a,  1.0, -b           (or  b, -1.0, a)
 *   ffma a,b,c    vmad d, a,  b,    c
 *   fneg a        vmad d, a, -1.0, -0.0
 *   fabs a        vmad d, |a|, 1.0, -0.0
 *   mov/copy a    vmad d, a,  1.0, -0.0
 *
 * The addend -0.0 (and not +0.0) keeps the sign of a zero result:
 * (-0) + (-0) = -0 and (+0) + (-0) = +0, so the result of x*y is bit-exact.
 *
 * Source 1 cannot read the sa bank (verified), so every instruction needs
 * at least one operand in r/pa for that slot; if neither operand is (e.g.
 * fadd of two uniforms) the compile fails until a vmov/vadd form is
 * modelled - it is never "fixed up" with a guess.
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

struct loc {
   uint8_t bank;     /* enum prismrv_usse_bank */
   uint8_t reg;      /* even register number */
   bool valid;
};

struct opnd {
   struct loc l;
   bool neg, abs;
};

struct ctx {
   struct prismrv_usse_binary *out;
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
mkloc(unsigned bank, unsigned slot)
{
   return (struct loc){ .bank = bank, .reg = PRISMRV_USSE_SLOT_REG(slot),
                        .valid = true };
}

static struct opnd
plain(struct loc l)
{
   return (struct opnd){ .l = l };
}

static struct opnd
sa_one(void)
{
   return plain(mkloc(PRISMRV_USSE_BANK_SA, PRISMRV_USSE_SA_ONE));
}

static struct opnd
sa_one_neg(void)
{
   struct opnd o = sa_one();
   o.neg = true;
   return o;
}

static struct opnd
sa_negzero(void)
{
   struct opnd o = plain(mkloc(PRISMRV_USSE_BANK_SA, PRISMRV_USSE_SA_ZERO));
   o.neg = true;
   return o;
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
   in.dst.reg = dst.reg;
   in.dst.mask = PRISMRV_USSE_MASK_X;
   for (unsigned i = 0; i < 3; i++) {
      in.src[i].bank = s[i].l.bank;
      in.src[i].reg = s[i].l.reg;
      in.src[i].neg = s[i].neg;
      in.src[i].abs = s[i].abs;
      memcpy(in.src[i].swz, "xxxx", 5);
   }
   if (!prismrv_usse_encode_vmad(&in, &w)) {
      fail(c, "internal: vmad not encodable (dst %u:%u)", dst.bank, dst.reg);
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

/* sa slot holding the 32-bit literal @bits (deduplicated) */
static struct loc
sa_const(struct ctx *c, uint32_t bits)
{
   struct prismrv_usse_binary *o = c->out;

   if (bits == 0x3f800000u)
      return mkloc(PRISMRV_USSE_BANK_SA, PRISMRV_USSE_SA_ONE);
   if (bits == 0)
      return mkloc(PRISMRV_USSE_BANK_SA, PRISMRV_USSE_SA_ZERO);
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
   case nir_op_ffma:
      break;
   default:
      fail(c, "unsupported NIR ALU op '%s' (no verified USSE form yet)",
           info->name);
      return;
   }

   for (unsigned ch = 0; ch < n && !c->failed; ch++) {
      struct loc a = src_loc(c, alu, 0, ch), b = { 0 }, cc = { 0 };
      struct loc d;
      bool ok = true;

      if (info->num_inputs > 1)
         b = src_loc(c, alu, 1, ch);
      if (info->num_inputs > 2)
         cc = src_loc(c, alu, 2, ch);
      if (c->failed)
         return;

      switch (alu->op) {
      case nir_op_mov:
         ok = src1_ok(a);
         break;
      case nir_op_fneg:
      case nir_op_fabs:
         ok = src1_ok(a);
         break;
      case nir_op_fadd:
      case nir_op_fsub:
         ok = src1_ok(a) || src1_ok(b);
         break;
      case nir_op_fmul:
         ok = src1_ok(a) || src1_ok(b);
         break;
      case nir_op_ffma:
         ok = src1_ok(a) || src1_ok(b);
         break;
      default:
         break;
      }
      if (!ok) {
         fail(c, "'%s' needs an r/pa operand for source 1 but all operands "
                 "are constants/uniforms (needs the vmov/vadd forms)",
              info->name);
         return;
      }

      d = alloc_temp(c);
      if (c->failed)
         return;

      switch (alu->op) {
      case nir_op_mov:
         emit_vmad(c, d, plain(a), sa_one(), sa_negzero());
         break;
      case nir_op_fneg:
         emit_vmad(c, d, plain(a), sa_one_neg(), sa_negzero());
         break;
      case nir_op_fabs: {
         struct opnd s1 = plain(a);
         s1.abs = true;
         emit_vmad(c, d, s1, sa_one(), sa_negzero());
         break;
      }
      case nir_op_fadd:
         if (src1_ok(a))
            emit_vmad(c, d, plain(a), sa_one(), plain(b));
         else
            emit_vmad(c, d, plain(b), sa_one(), plain(a));
         break;
      case nir_op_fsub:
         if (src1_ok(a)) {
            struct opnd s3 = plain(b);
            s3.neg = true;
            emit_vmad(c, d, plain(a), sa_one(), s3);     /* a*1 + (-b) */
         } else {
            emit_vmad(c, d, plain(b), sa_one_neg(), plain(a)); /* b*-1 + a */
         }
         break;
      case nir_op_fmul:
         if (src1_ok(a))
            emit_vmad(c, d, plain(a), plain(b), sa_negzero());
         else
            emit_vmad(c, d, plain(b), plain(a), sa_negzero());
         break;
      case nir_op_ffma:
         if (src1_ok(a))
            emit_vmad(c, d, plain(a), plain(b), plain(cc));
         else
            emit_vmad(c, d, plain(b), plain(a), plain(cc));
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
                 mkloc(PRISMRV_USSE_BANK_PA, 4 * vec + comp + i));
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
         if (slot >= 64) {
            fail(c, "output slot %u out of range", slot);
            return;
         }
         if (!src1_ok(src)) {
            fail(c, "output stores a constant/uniform directly (needs the "
                    "vmov form)");
            return;
         }
         emit_vmad(c, mkloc(PRISMRV_USSE_BANK_O, slot), plain(src),
                   sa_one(), sa_negzero());
      }
      break;
   }

   default:
      fail(c, "unsupported intrinsic '%s'",
           nir_intrinsic_infos[intr->intrinsic].name);
      break;
   }
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
   out->sa_image[PRISMRV_USSE_SA_ONE] = 0x3f800000u;
   out->sa_image[PRISMRV_USSE_SA_ZERO] = 0;
   out->sa_uniform_first = PRISMRV_USSE_SA_UNIFORM0;
   out->sa_uniform_count = uniforms;
   out->sa_pool_first = PRISMRV_USSE_SA_UNIFORM0 + uniforms;

   c.out = out;
   c.num_defs = impl->ssa_alloc;
   c.locs = rzalloc_array(NULL, struct loc, MAX2(c.num_defs, 1) * 4);
   c.pool_next = out->sa_pool_first;

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

   out->num_r_slots = c.next_r;
   out->sa_pool_count = c.pool_next - out->sa_pool_first;
   ralloc_free(c.locs);
   ralloc_free(nir);
   return !c.failed;
}

void
prismrv_usse_disasm_vmad(uint64_t word, char *buf, unsigned size)
{
   static const char *const bank[] = { "r", "o", "pa", "sa", "i" };
   struct prismrv_usse_vmad v;
   int n;

   if (!prismrv_usse_decode_vmad(word, &v)) {
      snprintf(buf, size, ".word 0x%016llx", (unsigned long long)word);
      return;
   }
   n = snprintf(buf, size, "vmad %s%u.%s%s", bank[v.dst.bank], v.dst.reg,
                v.dst.mask == 3 ? "xy" : v.dst.mask == 2 ? "y" : "x",
                v.pred ? (v.pred == PRISMRV_USSE_PRED_P0 ? " (p0)" : " (!p0)")
                       : "");
   for (unsigned i = 0; i < 3 && n > 0 && (unsigned)n < size; i++)
      n += snprintf(buf + n, size - n, ", %s%s%s%u.%s%s", v.src[i].neg ? "-" : "",
                    v.src[i].abs ? "|" : "", bank[v.src[i].bank], v.src[i].reg,
                    v.src[i].swz, v.src[i].abs ? "|" : "");
}
