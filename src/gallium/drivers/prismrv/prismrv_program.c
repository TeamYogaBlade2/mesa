/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * prismrv_program.c — NIR → USSE backend.
 *
 * Walks a NIR shader and emits USSE text compatible with the PrismRV
 * emulator's parser (usse_emu.parse).  Register conventions match the
 * hardware two-stage model used by the kernel driver:
 *
 *   r0..r15   vertex inputs / scratch
 *   r16..r47  uniforms      (vec4 N at registers N..N+3)
 *   r32..r47  varyings      (interpolated by the PBE before invocation)
 *   o0..o3    fragment output
 *
 * Only the vec4 subset needed for the first GL tests is handled;
 * unsupported NIR ops abort compilation loudly rather than miscompiling.
 */
#include "prismrv_program.h"

#include "util/ralloc.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* SSA definition → vec4 register allocation */
struct regmap {
   unsigned next;                       /* next free vec4 base */
};

/* every NIR SSA def gets a vec4 group; keep it simple and linear */
static unsigned
ssa_base(struct regmap *rm, const nir_def *def)
{
   (void)rm;
   if (!def)
      return 0;
   return 64 + (unsigned)(def->index * 4);
}

static const char *
chan(unsigned i)
{
   static const char *chans[] = { "x", "y", "z", "w" };
   return chans[i & 3];
}

/* render "<reg>" or "<reg>.<swiz>" depending on component count */
static void
emit_def(char *buf, size_t len, unsigned base, const nir_def *def)
{
   unsigned comps = def ? def->num_components : 4;
   if (comps == 1)
      snprintf(buf, len, "r%u", base);
   else {
      size_t off = (size_t)snprintf(buf, len, "r%u.", base);
      for (unsigned c = 0; c < comps && off < len - 1; c++, off++)
         buf[off] = 'x' + c;
      buf[off] = 0;
   }
}

struct emit_ctx {
   char *out;                /* ralloc'd string being built */
   struct regmap rm;
   enum mesa_shader_stage stage;
};

static void
emit_line(struct emit_ctx *c, const char *fmt, ...)
{
   char line[256];
   va_list ap;

   va_start(ap, fmt);
   vsnprintf(line, sizeof(line), fmt, ap);
   va_end(ap);

   ralloc_strcat(&c->out, line);
   ralloc_strcat(&c->out, "\n");
}

static void
emit_alu(struct emit_ctx *c, nir_alu_instr *alu)
{
   const nir_op_info *info = &nir_op_infos[alu->op];
   char dst[64], sa[64], sb[64], sc[64];
   unsigned base = ssa_base(&c->rm, &alu->def);

   switch (alu->op) {
   case nir_op_mov:
      emit_def(dst, sizeof(dst), base, &alu->def);
      emit_def(sa, sizeof(sa), ssa_base(&c->rm, alu->src[0].src.ssa),
               alu->src[0].src.ssa);
      emit_line(c, "vmov %s, %s, swizzle(xyzw)", dst, sa);
      break;

   case nir_op_fmul:
      emit_def(dst, sizeof(dst), base, &alu->def);
      emit_def(sa, sizeof(sa), ssa_base(&c->rm, alu->src[0].src.ssa),
               alu->src[0].src.ssa);
      emit_def(sb, sizeof(sb), ssa_base(&c->rm, alu->src[1].src.ssa),
               alu->src[1].src.ssa);
      emit_line(c, "vmul %s, %s, %s", dst, sa, sb);
      break;

   case nir_op_fadd:
      emit_def(dst, sizeof(dst), base, &alu->def);
      emit_def(sa, sizeof(sa), ssa_base(&c->rm, alu->src[0].src.ssa),
               alu->src[0].src.ssa);
      emit_def(sb, sizeof(sb), ssa_base(&c->rm, alu->src[1].src.ssa),
               alu->src[1].src.ssa);
      /* a + b == a*1 + b */
      emit_line(c, "vmad %s, %s, r61, %s", dst, sa, sb);
      break;

   case nir_op_fsub:
      emit_def(dst, sizeof(dst), base, &alu->def);
      emit_def(sa, sizeof(sa), ssa_base(&c->rm, alu->src[0].src.ssa),
               alu->src[0].src.ssa);
      emit_def(sb, sizeof(sb), ssa_base(&c->rm, alu->src[1].src.ssa),
               alu->src[1].src.ssa);
      /* a - b == b*(-1) + a (note the operand swap: vmad is
       * dst = srcA*srcB + srcC) */
      emit_line(c, "vmad %s, %s, r62, %s", dst, sb, sa);
      break;

   default:
      fprintf(stderr,
              "prismrv: unsupported NIR op '%s' (%d); instruction skipped\n",
              info->name, alu->op);
      break;
   }
}

static void
emit_tex(struct emit_ctx *c, nir_tex_instr *tex)
{
   /* texture sample: source 0 = coordinate (vec2), result -> SSA regs.
    * Emits the emulator's `smp dst, uvReg, idReg` (nearest, 2D only).
    * The texture id is passed as an immediate: the executor registers
    * bound textures by index. */
   nir_src *coord = NULL;
   int base = -1;

   for (unsigned i = 0; i < tex->num_srcs; i++) {
      if (tex->src[i].src_type == nir_tex_src_coord)
         coord = &tex->src[i].src;
   }
   if (!coord || tex->op != nir_texop_tex || tex->sampler_dim != GLSL_SAMPLER_DIM_2D) {
      fprintf(stderr, "prismrv: unsupported tex op %d\n", tex->op);
      return;
   }

   base = ssa_base(&c->rm, &tex->def);
   emit_line(c, "smp r%u, r%u, #%u",
             base,
             ssa_base(&c->rm, coord->ssa),
             tex->texture_index);
}

static void
emit_intrinsic(struct emit_ctx *c, nir_intrinsic_instr *intr)
{
   /* loads are no-ops in the text form: the emulator preloads uniforms
    * into r16.. and varyings into r32.. per the ABI.
    *
    * Stores to the fragment colour output become explicit o-bank copies,
    * one scalar line per component (the emulator's parser is scalar). */
   switch (intr->intrinsic) {
   case nir_intrinsic_store_output: {
      nir_def *val = intr->src[0].ssa;
      unsigned base = ssa_base(&c->rm, val);
      unsigned mask = nir_intrinsic_write_mask(intr);
      unsigned comps = val->num_components;
      /* driver location: 0 = colour (FS) / clip position (VS) */
      unsigned loc = nir_intrinsic_base(intr);

      for (unsigned ch = 0; ch < comps && ch < 4; ch++) {
         if (!(mask & (1u << ch)))
            continue;
         if (c->stage == MESA_SHADER_VERTEX && loc == 0)
            /* VS: o0..3 = clip position, o4.. = varyings
             * (ta_stage.py _fetch_vertices convention) */
            emit_line(c, "vmov o%u, r%u, swizzle(xxxx)", ch, base + ch);
         else
            emit_line(c, "vmov o%u, r%u, swizzle(xxxx)",
                      c->stage == MESA_SHADER_FRAGMENT ? ch
                                                       : 4 + ch,
                      base + ch);
      }
      break;
   }
   default:
      break;
   }
}

char *
prismrv_nir_to_usse(void *memctx, nir_shader *nir)
{
   struct emit_ctx ctx;

   memset(&ctx, 0, sizeof(ctx));
   /* ralloc_strcat() requires a non-NULL destination string */
   ctx.out = ralloc_strdup(memctx, "");
   ctx.stage = nir->info.stage;

   emit_line(&ctx, "# %s shader", mesa_shader_stage_name(nir->info.stage));

   /*
    * Constant preamble.  fadd/fsub are lowered to vmad with a 1.0/-1.0
    * multiplier, matching the shaderc (Python UMD) convention where
    * r60 = 0.0f, r61 = +1.0f and r62 = -1.0f.  The executor does not
    * preseed these registers, so the program sets them up itself.
    */
   emit_line(&ctx, "mov r60, #0x00000000");
   emit_line(&ctx, "mov r61, #0x3f800000");
   emit_line(&ctx, "mov r62, #0xbf800000");

   /* walk every block of every function */
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            switch (instr->type) {
            case nir_instr_type_alu:
               emit_alu(&ctx, nir_instr_as_alu(instr));
               break;
            case nir_instr_type_intrinsic:
               emit_intrinsic(&ctx, nir_instr_as_intrinsic(instr));
               break;
            case nir_instr_type_tex:
               emit_tex(&ctx, nir_instr_as_tex(instr));
               break;
            default:
               break;
            }
         }
      }

   }

   return ctx.out ? ctx.out : ralloc_strdup(memctx, "");
}
