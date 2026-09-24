/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * prismrv_program.c — NIR → USSE backend.
 *
 * Walks a NIR shader and emits USSE text compatible with the PrismRV
 * emulator's parser (usse_emu.parse).  Register conventions:
 *
 *   r0..r15   scratch / temporaries
 *   r16..r31  VS per-vertex inputs  (r16 + location*4 + component)
 *   r32..r47  FS varyings           (r32 + location*4 + component)
 *   r48..     uniforms              (r48 + byte_offset/4)
 *   r60       constant 0.0          (emitted in preamble)
 *   r61       constant 1.0          (emitted in preamble)
 *   r62       constant -1.0         (emitted in preamble)
 *   o0..o3    VS clip position / FS colour output (per component)
 *   o4..      VS varying outputs
 *
 * Only 32-bit NIR is supported; 16-bit and 64-bit constants are
 * rejected (run nir_lower_bit_size before calling this backend).
 * Unsupported NIR ops abort compilation loudly rather than miscompiling.
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
   bool unsupported;         /* an op we cannot translate was seen */
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
      /*
       * The header promises to abort loudly rather than miscompile.
       * Silently skipping produced programs whose results are wrong
       * with no diagnostic, so fail the compilation instead.
       */
      fprintf(stderr, "prismrv: unsupported NIR op '%s' (%d); "
                      "failing shader compilation\n", info->name, alu->op);
      c->unsupported = true;
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
      fprintf(stderr, "prismrv: unsupported tex op %d (sampler_dim %d)\n",
              tex->op, tex->sampler_dim);
      c->unsupported = true;
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
   /*
    * ABI conventions for the USSE emulator / PVR uKernel:
    *
    *   r16..r31  = VS inputs  (one vec4 per attribute, index = location)
    *   r32..r47  = FS varyings (one vec4 per varying, index = location)
    *   uniforms  = loaded from HostCtl UBO mirror into r48..
    *
    * load_input / load_uniform / load_const are translated to explicit
    * vmov instructions from these pre-seeded register banks so that the
    * emulator and real uKernel both see the correct data.
    *
    * Previously these were silent no-ops, meaning the shader used
    * undefined register contents for all inputs — producing wrong output
    * on every draw without any compilation error.
    */
   switch (intr->intrinsic) {

   case nir_intrinsic_load_input: {
      /*
       * VS per-vertex attribute: register bank starts at r16.
       * location × 4 gives the first component of the attribute's vec4.
       */
      unsigned loc  = nir_intrinsic_base(intr);
      unsigned comp = nir_intrinsic_component(intr);
      unsigned dst  = ssa_base(&c->rm, &intr->def);
      unsigned ncomp = intr->def.num_components;
      unsigned src_base = (c->stage == MESA_SHADER_FRAGMENT ? 32 : 16)
                          + loc * 4 + comp;
      for (unsigned i = 0; i < ncomp; i++)
         emit_line(c, "vmov r%u, r%u, swizzle(xxxx)", dst + i, src_base + i);
      break;
   }

   case nir_intrinsic_load_uniform: {
      /*
       * Uniforms: emulator preloads them at r48 + base/4.
       * base is the byte offset; components are consecutive.
       */
      unsigned base  = nir_intrinsic_base(intr);
      unsigned dst   = ssa_base(&c->rm, &intr->def);
      unsigned ncomp = intr->def.num_components;
      unsigned src_r = 48 + base / 4;
      for (unsigned i = 0; i < ncomp; i++)
         emit_line(c, "vmov r%u, r%u, swizzle(xxxx)", dst + i, src_r + i);
      break;
   }

   case nir_intrinsic_load_deref:
      /*
       * Deref-based loads are not expected after the standard lowering
       * passes (lower_io eliminates them).  Treat as unsupported so the
       * caller knows to re-run the right lowering pass.
       */
      fprintf(stderr, "prismrv: load_deref reached emitter "
              "(missing lower_io pass?)\n");
      c->unsupported = true;
      break;

   case nir_intrinsic_store_output: {
      nir_def *val = intr->src[0].ssa;
      unsigned base = ssa_base(&c->rm, val);
      unsigned mask = nir_intrinsic_write_mask(intr);
      unsigned comps = val->num_components;
      unsigned loc = nir_intrinsic_base(intr);

      for (unsigned ch = 0; ch < comps && ch < 4; ch++) {
         if (!(mask & (1u << ch)))
            continue;
         if (c->stage == MESA_SHADER_VERTEX && loc == 0)
            emit_line(c, "vmov o%u, r%u, swizzle(xxxx)", ch, base + ch);
         else
            emit_line(c, "vmov o%u, r%u, swizzle(xxxx)",
                      c->stage == MESA_SHADER_FRAGMENT ? ch : 4 + ch,
                      base + ch);
      }
      break;
   }

   default:
      fprintf(stderr, "prismrv: unsupported intrinsic '%s' (%d)\n",
              nir_intrinsic_infos[intr->intrinsic].name, intr->intrinsic);
      c->unsupported = true;
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
            case nir_instr_type_load_const: {
               nir_load_const_instr *lc = nir_instr_as_load_const(instr);
               unsigned dst = ssa_base(&ctx.rm, &lc->def);

               /*
                * Only 32-bit constants are supported: the USSE text
                * format uses 32-bit hex literals and all working
                * registers are 32-bit.
                *
                * 16-bit values cannot simply be zero-extended: a
                * half-float 1.0 (0x3c00) is not the same bit pattern
                * as single-float 1.0 (0x3f800000).  64-bit values
                * similarly cannot be truncated without data loss.
                *
                * NIR lowering passes (nir_lower_bit_size) should
                * convert 16-bit and 64-bit ops to 32-bit before we
                * reach here.  If they do not, reject compilation
                * loudly rather than silently miscompile.
                */
               if (lc->def.bit_size != 32) {
                  fprintf(stderr,
                          "prismrv: %u-bit load_const not supported "
                          "(run nir_lower_bit_size first)\n",
                          lc->def.bit_size);
                  ctx.unsupported = true;
                  break;
               }

               for (unsigned c = 0; c < lc->def.num_components; c++)
                  emit_line(&ctx, "mov r%u, #0x%08x",
                            dst + c, lc->value[c].u32);
               break;
            }
            case nir_instr_type_alu:
               emit_alu(&ctx, nir_instr_as_alu(instr));
               break;
            case nir_instr_type_intrinsic:
               emit_intrinsic(&ctx, nir_instr_as_intrinsic(instr));
               break;
            case nir_instr_type_tex:
               emit_tex(&ctx, nir_instr_as_tex(instr));
               break;
            case nir_instr_type_undef:
               /* SSA undef: allocate a register; leave contents undefined.
                * Emit a zero-mov so the emulator does not read garbage. */
               emit_line(&ctx, "mov r%u, #0x00000000",
                         ssa_base(&ctx.rm, &nir_instr_as_undef(instr)->def));
               break;
            default:
               break;
            }
         }
      }

   }

   if (ctx.unsupported) {
      ralloc_free(ctx.out);
      return NULL;
   }

   return ctx.out;
}
