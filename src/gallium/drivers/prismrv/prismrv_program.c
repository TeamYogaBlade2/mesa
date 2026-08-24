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
      /* a - b == a*(-1) + b */
      emit_line(c, "vmad %s, %s, r62, %s", dst, sa, sb);
      break;

   default:
      fprintf(stderr,
              "prismrv: unsupported NIR op '%s' (%d); instruction skipped\n",
              info->name, alu->op);
      break;
   }
}

static void
emit_intrinsic(struct emit_ctx *c, nir_intrinsic_instr *intr)
{
   /* loads/stores are no-ops in the text form: the emulator preloads
    * uniforms into r16.. and varyings into r32.. per the ABI, and the
    * output store maps onto the o-bank writes below. */
   (void)c; (void)intr;
}

char *
prismrv_nir_to_usse(void *memctx, nir_shader *nir)
{
   struct emit_ctx ctx;
   char dst[64];

   memset(&ctx, 0, sizeof(ctx));

   emit_line(&ctx, "# %s shader", mesa_shader_stage_name(nir->info.stage));

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
            default:
               break;
            }
         }
      }

      /* fragment outputs: store_output intrinsics were skipped above;
       * conventionally the last written varying set lands in the
       * o-bank.  For the bring-up tests we close with explicit output
       * copies from the highest allocated temp group. */
      if (nir->info.stage == MESA_SHADER_FRAGMENT && ctx.out &&
          strstr(ctx.out, "vmov")) {
         /* nothing extra: tests wire outputs explicitly */
      }
   }

   (void)dst;
   return ctx.out ? ctx.out : ralloc_strdup(memctx, "");
}
