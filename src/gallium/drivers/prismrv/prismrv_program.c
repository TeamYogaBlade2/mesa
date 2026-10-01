/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * prismrv_program.c — NIR → USSE text backend for the PrismRV command
 * stream (SET_PROG_VS / SET_PROG_FS), see prismrv_program.h for the
 * register ABI.
 *
 * Scope (deliberately narrow, everything else is a compile failure):
 *   - one function, one basic block (no if/loop/jump/phi/call)
 *   - 32-bit float ALU: mov vec* fneg fadd fsub fmul ffma frcp frsq
 *   - intrinsics: load_input, load_uniform (constant offset),
 *     store_output (VS position + one varying, FS colour)
 *   - texture: 2D nearest sampling (nir_texop_tex), fragment stage
 *   - every operation is emitted per component (scalar), so source
 *     swizzles and write masks are honoured exactly
 *
 * The output is text for the executor's parser (usse_emu.parse); it is
 * NOT the real SGX USSE binary encoding.
 */
#include "prismrv_program.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "compiler/glsl_types.h"
#include "util/macros.h"
#include "util/ralloc.h"

#define REG_TEMP_BASE 64
#define REG_MAX       256

static unsigned
type_size_vec4(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false);
}

static unsigned
type_size_bytes(const struct glsl_type *type, bool bindless)
{
   return glsl_count_dword_slots(type, bindless) * 4;
}

static const nir_shader_compiler_options prismrv_nir_options = {
   .lower_flrp32 = true,
   .lower_fpow = true,
   .lower_fsat = true,
   .lower_ffract = true,
   .lower_fdiv = true,     /* a/b -> a * rcp(b) */
   .lower_fsqrt = true,    /* sqrt(x) -> x * rsq(x) */
   .lower_fmod = true,
   .lower_bitfield_extract = true,
   .lower_bitfield_insert = true,
   .lower_uadd_carry = true,
   .lower_usub_borrow = true,
   .lower_scmp = true,
   .max_unroll_iterations = 0,
   .force_indirect_unrolling = 0,
};

const nir_shader_compiler_options *
prismrv_get_nir_options(void)
{
   return &prismrv_nir_options;
}

struct emit_ctx {
   char *out;
   size_t out_len;
   mesa_shader_stage stage;
   unsigned next_reg;
   unsigned *def_reg;            /* SSA index -> first scalar register */
   unsigned num_defs;
   bool unsupported;
   bool text_overflow;
};

static void
fail(struct emit_ctx *c, const char *fmt, ...)
{
   va_list ap;

   if (!c->unsupported) {
      fprintf(stderr, "prismrv: ");
      va_start(ap, fmt);
      vfprintf(stderr, fmt, ap);
      va_end(ap);
      fprintf(stderr, "\n");
   }
   c->unsupported = true;
}

static void
emit(struct emit_ctx *c, const char *fmt, ...)
{
   char line[160];
   va_list ap;
   int n;

   va_start(ap, fmt);
   n = vsnprintf(line, sizeof(line), fmt, ap);
   va_end(ap);
   if (n < 0 || (size_t)n >= sizeof(line) - 1) {
      fail(c, "internal: instruction text too long");
      return;
   }
   if (c->out_len + n + 2 > PRISMRV_MAX_USSE_TEXT) {
      c->text_overflow = true;
      fail(c, "shader exceeds %u bytes of USSE text", PRISMRV_MAX_USSE_TEXT);
      return;
   }
   ralloc_strcat(&c->out, line);
   ralloc_strcat(&c->out, "\n");
   c->out_len += n + 1;
}

/* allocate num_components contiguous scalar registers for a def */
static unsigned
def_alloc(struct emit_ctx *c, const nir_def *def, unsigned min_regs)
{
   unsigned n = MAX2(def->num_components, min_regs);
   unsigned base = c->next_reg;

   if (def->index >= c->num_defs) {
      fail(c, "internal: SSA index out of range");
      return 0;
   }
   if (base + n > REG_MAX) {
      fail(c, "out of registers (temporaries r%u..r%u exhausted)",
           REG_TEMP_BASE, REG_MAX - 1);
      return 0;
   }
   c->def_reg[def->index] = base;
   c->next_reg += n;
   return base;
}

/* register holding component @comp (after swizzle) of an ALU source */
static unsigned
src_reg(struct emit_ctx *c, const nir_alu_instr *alu, unsigned src,
        unsigned comp)
{
   const nir_def *def = alu->src[src].src.ssa;
   unsigned swz = alu->src[src].swizzle[comp];

   return c->def_reg[def->index] + swz;
}

static void
emit_alu(struct emit_ctx *c, nir_alu_instr *alu)
{
   const nir_op_info *info = &nir_op_infos[alu->op];
   unsigned n = alu->def.num_components;
   unsigned dst;

   if (alu->def.bit_size != 32) {
      fail(c, "ALU op '%s' with %u-bit result", info->name,
           alu->def.bit_size);
      return;
   }
   for (unsigned s = 0; s < info->num_inputs; s++) {
      if (alu->src[s].src.ssa->bit_size != 32 ||
          (info->input_types[s] != nir_type_float &&
           info->input_types[s] != nir_type_invalid &&
           info->input_types[s] != nir_type_float32)) {
         /* nir_op_mov/vecN have nir_type_invalid (untyped) inputs */
         if (alu->src[s].src.ssa->bit_size != 32) {
            fail(c, "ALU op '%s' with non-32-bit source", info->name);
            return;
         }
      }
   }

   switch (alu->op) {
   case nir_op_mov:
   case nir_op_vec2:
   case nir_op_vec3:
   case nir_op_vec4:
   case nir_op_fneg:
   case nir_op_fadd:
   case nir_op_fsub:
   case nir_op_fmul:
   case nir_op_ffma:
   case nir_op_frcp:
   case nir_op_frsq:
      break;
   default:
      fail(c, "unsupported NIR ALU op '%s'", info->name);
      return;
   }

   dst = def_alloc(c, &alu->def, 1);
   if (c->unsupported)
      return;

   for (unsigned ch = 0; ch < n; ch++) {
      unsigned d = dst + ch;

      switch (alu->op) {
      case nir_op_mov:
         emit(c, "vmov r%u, r%u, swizzle(xxxx)", d, src_reg(c, alu, 0, ch));
         break;
      case nir_op_vec2:
      case nir_op_vec3:
      case nir_op_vec4:
         /* vecN: component ch comes from source ch (scalar, swizzle x) */
         emit(c, "vmov r%u, r%u, swizzle(xxxx)", d, src_reg(c, alu, ch, 0));
         break;
      case nir_op_fneg:
         emit(c, "vmul r%u, r%u, r62", d, src_reg(c, alu, 0, ch));
         break;
      case nir_op_fmul:
         emit(c, "vmul r%u, r%u, r%u", d, src_reg(c, alu, 0, ch),
              src_reg(c, alu, 1, ch));
         break;
      case nir_op_fadd:
         /* a + b == a*1 + b */
         emit(c, "vmad r%u, r%u, r61, r%u", d, src_reg(c, alu, 0, ch),
              src_reg(c, alu, 1, ch));
         break;
      case nir_op_fsub:
         /* a - b == b*(-1) + a */
         emit(c, "vmad r%u, r%u, r62, r%u", d, src_reg(c, alu, 1, ch),
              src_reg(c, alu, 0, ch));
         break;
      case nir_op_ffma:
         emit(c, "vmad r%u, r%u, r%u, r%u", d, src_reg(c, alu, 0, ch),
              src_reg(c, alu, 1, ch), src_reg(c, alu, 2, ch));
         break;
      case nir_op_frcp:
         emit(c, "frcp r%u, r%u", d, src_reg(c, alu, 0, ch));
         break;
      case nir_op_frsq:
         emit(c, "frsq r%u, r%u", d, src_reg(c, alu, 0, ch));
         break;
      default:
         UNREACHABLE("filtered above");
      }
   }
}

static void
emit_load_const(struct emit_ctx *c, nir_load_const_instr *lc)
{
   unsigned dst;

   /* 16/64-bit values cannot be zero-extended or truncated safely */
   if (lc->def.bit_size != 32) {
      fail(c, "%u-bit load_const", lc->def.bit_size);
      return;
   }
   dst = def_alloc(c, &lc->def, 1);
   if (c->unsupported)
      return;
   for (unsigned i = 0; i < lc->def.num_components; i++)
      emit(c, "mov r%u, #0x%08x", dst + i, lc->value[i].u32);
}

static void
emit_tex(struct emit_ctx *c, nir_tex_instr *tex)
{
   const nir_def *coord = NULL;
   unsigned dst;

   if (c->stage != MESA_SHADER_FRAGMENT) {
      fail(c, "texture sampling outside the fragment stage");
      return;
   }
   for (unsigned i = 0; i < tex->num_srcs; i++) {
      switch (tex->src[i].src_type) {
      case nir_tex_src_coord:
         coord = tex->src[i].src.ssa;
         break;
      default:
         fail(c, "texture source %d (lod/bias/offset/proj/shadow/...)",
              tex->src[i].src_type);
         return;
      }
   }
   if (!coord || tex->op != nir_texop_tex ||
       tex->sampler_dim != GLSL_SAMPLER_DIM_2D || tex->is_array ||
       tex->is_shadow || coord->num_components != 2 ||
       tex->dest_type != nir_type_float32 || tex->texture_index >= 8) {
      fail(c, "unsupported texture op %d (dim %d)", tex->op,
           tex->sampler_dim);
      return;
   }

   dst = def_alloc(c, &tex->def, 4);
   if (c->unsupported)
      return;
   emit(c, "smp r%u, r%u, #%u", dst, c->def_reg[coord->index],
        tex->texture_index);
}

/* constant slot offset of an IO intrinsic, or fail */
static bool
io_slot(struct emit_ctx *c, nir_intrinsic_instr *intr, nir_src *offset,
        unsigned *slot)
{
   if (!nir_src_is_const(*offset)) {
      fail(c, "indirect addressing in '%s'",
           nir_intrinsic_infos[intr->intrinsic].name);
      return false;
   }
   *slot = nir_intrinsic_base(intr) + nir_src_as_uint(*offset);
   return true;
}

static void
emit_intrinsic(struct emit_ctx *c, nir_intrinsic_instr *intr)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_load_input: {
      unsigned slot, dst, src_base;
      unsigned comp = nir_intrinsic_component(intr);
      unsigned n = intr->def.num_components;

      if (intr->def.bit_size != 32) {
         fail(c, "non-32-bit load_input");
         return;
      }
      if (!io_slot(c, intr, &intr->src[0], &slot))
         return;

      if (c->stage == MESA_SHADER_VERTEX) {
         if (slot >= PRISMRV_MAX_VS_ATTRIBS) {
            fail(c, "vertex input %u (max %u attributes)", slot,
                 PRISMRV_MAX_VS_ATTRIBS);
            return;
         }
         src_base = slot * 4 + comp;
      } else {
         const nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
         unsigned var = sem.location >= VARYING_SLOT_VAR0 ?
            sem.location - VARYING_SLOT_VAR0 :
            (sem.location == VARYING_SLOT_COL0 ? 0 : ~0u);

         if (var != 0) {
            fail(c, "fragment input slot %u (only one varying is "
                    "carried by the executor)", sem.location);
            return;
         }
         src_base = 32 + comp;
      }
      dst = def_alloc(c, &intr->def, 1);
      if (c->unsupported)
         return;
      for (unsigned i = 0; i < n; i++)
         emit(c, "vmov r%u, r%u, swizzle(xxxx)", dst + i, src_base + i);
      break;
   }

   case nir_intrinsic_load_uniform: {
      unsigned byte_off, dst, reg;
      unsigned n = intr->def.num_components;

      if (intr->def.bit_size != 32) {
         fail(c, "non-32-bit load_uniform");
         return;
      }
      if (!nir_src_is_const(intr->src[0])) {
         fail(c, "indirect uniform access");
         return;
      }
      byte_off = nir_intrinsic_base(intr) + nir_src_as_uint(intr->src[0]);
      reg = 16 + byte_off / 4;
      if (reg + n > 16 + PRISMRV_MAX_UNIFORM_VEC4 * 4) {
         fail(c, "uniform at byte offset %u beyond %u vec4 uniforms",
              byte_off, PRISMRV_MAX_UNIFORM_VEC4);
         return;
      }
      dst = def_alloc(c, &intr->def, 1);
      if (c->unsupported)
         return;
      for (unsigned i = 0; i < n; i++)
         emit(c, "vmov r%u, r%u, swizzle(xxxx)", dst + i, reg + i);
      break;
   }

   case nir_intrinsic_store_output: {
      const nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
      const nir_def *val = intr->src[0].ssa;
      unsigned mask = nir_intrinsic_write_mask(intr);
      unsigned comp = nir_intrinsic_component(intr);
      unsigned slot, obase;

      if (val->bit_size != 32) {
         fail(c, "non-32-bit store_output");
         return;
      }
      if (!io_slot(c, intr, &intr->src[1], &slot))
         return;

      if (c->stage == MESA_SHADER_VERTEX) {
         if (sem.location == VARYING_SLOT_POS) {
            obase = 0;
         } else if (sem.location == VARYING_SLOT_VAR0 ||
                    sem.location == VARYING_SLOT_COL0) {
            obase = 4;
         } else {
            fail(c, "vertex output slot %u unsupported", sem.location);
            return;
         }
      } else {
         if (sem.location != FRAG_RESULT_COLOR &&
             sem.location != FRAG_RESULT_DATA0) {
            fail(c, "fragment output slot %u unsupported", sem.location);
            return;
         }
         obase = 0;
      }

      for (unsigned i = 0; i < val->num_components; i++) {
         unsigned ch = comp + i;

         if (!(mask & (1u << i)))
            continue;
         /* the executor carries 3 varying floats (o4..o6): alpha of a
          * varying is not transported */
         if (obase == 4 && c->stage == MESA_SHADER_VERTEX && ch >= 3)
            continue;
         emit(c, "vmov o%u, r%u, swizzle(xxxx)", obase + ch,
              c->def_reg[val->index] + i);
      }
      (void)slot;
      break;
   }

   default:
      fail(c, "unsupported intrinsic '%s'",
           nir_intrinsic_infos[intr->intrinsic].name);
      break;
   }
}

/* lowering + optimisation so the emitter sees flat, scalar-friendly NIR */
static bool
prismrv_lower_and_optimize(nir_shader *nir)
{
   bool progress;

   nir_shader_get_entrypoint(nir);

   NIR_PASS(_, nir, nir_lower_returns);
   NIR_PASS(_, nir, nir_inline_functions);
   nir_remove_non_entrypoints(nir);
   NIR_PASS(_, nir, nir_lower_global_vars_to_local);
   NIR_PASS(_, nir, nir_lower_vars_to_ssa);
   NIR_PASS(_, nir, nir_lower_var_copies);

   if (nir->info.stage == MESA_SHADER_VERTEX ||
       nir->info.stage == MESA_SHADER_FRAGMENT) {
      /* uniform storage: byte offsets in declaration order */
      {
         unsigned off = 0;

         nir_foreach_uniform_variable(var, nir) {
            var->data.driver_location = off;
            off += type_size_bytes(var->type, false);
         }
         nir->num_uniforms = off;
      }
      NIR_PASS(_, nir, nir_lower_io, nir_var_uniform, type_size_bytes, 0);
      NIR_PASS(_, nir, nir_lower_io,
               nir_var_shader_in | nir_var_shader_out, type_size_vec4, 0);
   }
   NIR_PASS(_, nir, nir_lower_load_const_to_scalar);

   do {
      progress = false;
      NIR_PASS(progress, nir, nir_lower_vars_to_ssa);
      NIR_PASS(progress, nir, nir_lower_alu_to_scalar, NULL, NULL);
      NIR_PASS(progress, nir, nir_opt_copy_prop);
      NIR_PASS(progress, nir, nir_opt_remove_phis);
      NIR_PASS(progress, nir, nir_opt_dce);
      NIR_PASS(progress, nir, nir_opt_dead_cf);
      NIR_PASS(progress, nir, nir_opt_cse);
      NIR_PASS(progress, nir, nir_opt_algebraic);
      NIR_PASS(progress, nir, nir_opt_constant_folding);
      NIR_PASS(progress, nir, nir_lower_undef_to_zero, NULL);
      NIR_PASS(progress, nir, nir_opt_peephole_select, &(nir_opt_peephole_select_options){
         .limit = 8, .expensive_alu_ok = true });
   } while (progress);

   NIR_PASS(_, nir, nir_opt_algebraic_late);
   NIR_PASS(_, nir, nir_opt_dce);
   nir_index_ssa_defs(nir_shader_get_entrypoint(nir));
   return true;
}

static char *prismrv_nir_to_usse_owned(void *memctx, nir_shader *nir);

/*
 * The caller's shader is left untouched: the lowering passes run on a
 * private clone, which is freed here.  (Ownership of the NIR given to
 * create_*_state is the caller's business; a compiler entry point that
 * mutates and frees its argument breaks as soon as the same NIR is also
 * used by a shader cache or a second compile.)
 */
char *
prismrv_nir_to_usse(void *memctx, nir_shader *nir)
{
   nir_shader *clone = nir_shader_clone(NULL, nir);
   char *text;

   if (!clone)
      return NULL;
   text = prismrv_nir_to_usse_owned(memctx, clone);
   ralloc_free(clone);
   return text;
}

static char *
prismrv_nir_to_usse_owned(void *memctx, nir_shader *nir)
{
   struct emit_ctx c = { 0 };
   nir_function_impl *impl;

   if (nir->info.stage != MESA_SHADER_VERTEX &&
       nir->info.stage != MESA_SHADER_FRAGMENT) {
      fprintf(stderr, "prismrv: unsupported shader stage %s\n",
              mesa_shader_stage_name(nir->info.stage));
      return NULL;
   }

   prismrv_lower_and_optimize(nir);
   impl = nir_shader_get_entrypoint(nir);

   /* Control flow is not implemented: refuse anything but a single
    * basic block instead of flattening branches into straight-line
    * code (which would execute both sides of every 'if'). */
   if (exec_list_length(&nir->functions) != 1 ||
       !exec_list_is_singular(&impl->body) ||
       nir_cf_node_as_block(exec_node_data(nir_cf_node, exec_list_get_head(&impl->body), node)) == NULL) {
      fprintf(stderr, "prismrv: shader has control flow (if/loop/call); "
                      "not supported\n");
      return NULL;
   }

   c.stage = nir->info.stage;
   c.num_defs = impl->ssa_alloc;
   c.def_reg = rzalloc_array(NULL, unsigned, MAX2(c.num_defs, 1));
   c.next_reg = REG_TEMP_BASE;
   c.out = ralloc_strdup(memctx, "");

   emit(&c, "# %s shader", mesa_shader_stage_name(nir->info.stage));
   /* constant registers used by the fadd/fsub/fneg mappings */
   emit(&c, "mov r60, #0x00000000");
   emit(&c, "mov r61, #0x3f800000");
   emit(&c, "mov r62, #0xbf800000");

   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         switch (instr->type) {
         case nir_instr_type_load_const:
            emit_load_const(&c, nir_instr_as_load_const(instr));
            break;
         case nir_instr_type_alu:
            emit_alu(&c, nir_instr_as_alu(instr));
            break;
         case nir_instr_type_intrinsic:
            emit_intrinsic(&c, nir_instr_as_intrinsic(instr));
            break;
         case nir_instr_type_tex:
            emit_tex(&c, nir_instr_as_tex(instr));
            break;
         case nir_instr_type_undef: {
            /* defensive: lower_undef_to_zero normally removes these */
            nir_undef_instr *u = nir_instr_as_undef(instr);
            unsigned dst = def_alloc(&c, &u->def, 1);

            if (!c.unsupported)
               for (unsigned i = 0; i < u->def.num_components; i++)
                  emit(&c, "mov r%u, #0x00000000", dst + i);
            break;
         }
         default:
            /* jump / phi / call / deref / parallel_copy / ... */
            fail(&c, "unsupported NIR instruction type %d", instr->type);
            break;
         }
         if (c.unsupported)
            break;
      }
      if (c.unsupported)
         break;
   }

   ralloc_free(c.def_reg);
   if (c.unsupported) {
      ralloc_free(c.out);
      return NULL;
   }
   return c.out;
}
