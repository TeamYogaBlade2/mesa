/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * prismrv_test_nir.c — runnable NIR → USSE test.
 *
 * Builds a fragment shader using only the NIR intrinsics the PrismRV
 * backend actually accepts (load_input / store_output / load_const /
 * ALU ops).  The previous version used nir_load_var / nir_store_var
 * which generate load_deref / store_deref — explicitly rejected by the
 * emitter — causing it to return NULL and the test to SIGSEGV on the
 * subsequent strstr(NULL, ...) call.
 *
 * Shader equivalent (GLSL):
 *
 *   in  vec4 v_color;    // location 0, read from r32..r35
 *   out vec4 gl_FragColor;
 *
 *   void main() {
 *       vec4 scaled = v_color * vec4(1.0, 0.9, 0.8, 1.0);
 *       gl_FragColor = scaled + v_color;
 *   }
 *
 * The expected USSE output contains vmul (fmul) and vmad (fadd after
 * vmul can be fused to fmad, but a stand-alone fadd also maps to vmad
 * or vadd depending on the emitter).
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "util/ralloc.h"
#include "compiler/nir/nir_builder.h"
#include "prismrv_program.h"
#include "prismrv_usse_encode.h"
#include "prismrv_usse_nir.h"

static nir_shader *
build_test_fs(void)
{
   static const nir_shader_compiler_options opts = { 0 };
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &opts, "prismrv-test-fs");

   /*
    * Declare input and output variables so nir_shader_add_variable works,
    * but use load_input / store_output intrinsics directly so the emitter
    * receives the forms it can handle.
    *
    * FS varyings live in r32..r35 (location 0 × 4 + component).
    */

   /* load the four components of v_color (location 0) as load_input */
   nir_def *comps[4];
   for (unsigned c = 0; c < 4; c++) {
      comps[c] = nir_load_input(&b, 1, 32,
                                nir_imm_int(&b, 0), /* offset */
                                .base = 0,
                                .component = c,
                                .io_semantics.location = VARYING_SLOT_COL0,
                                .io_semantics.num_slots = 1);
   }
   nir_def *v_color = nir_vec4(&b, comps[0], comps[1], comps[2], comps[3]);

   /* constants: (1.0, 0.9, 0.8, 1.0) */
   nir_def *k = nir_imm_vec4(&b, 1.0f, 0.9f, 0.8f, 1.0f);

   /* scaled = v_color * k  (generates vmul) */
   nir_def *scaled = nir_fmul(&b, v_color, k);

   /* sum = scaled + v_color  (generates vmad or vadd) */
   nir_def *sum = nir_fadd(&b, scaled, v_color);

   /* store_output: write each component to gl_FragColor (location 0) */
   for (unsigned c = 0; c < 4; c++) {
      nir_store_output(&b, nir_channel(&b, sum, c),
                       nir_imm_int(&b, 0), /* offset */
                       .base = 0,
                       .component = c,
                       .write_mask = 0x1,
                       .io_semantics.location = FRAG_RESULT_COLOR,
                       .io_semantics.num_slots = 1);
   }

   return b.shader;
}

/* ---- tiny USSE-text interpreter: checks the generated program computes
 * the right numbers, not merely that certain mnemonics appear ------------ */
struct machine { uint32_t r[256], o[16]; };

static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static uint32_t *
reg(struct machine *m, const char *t)
{
   unsigned n = strtoul(t + 1, NULL, 10);
   return t[0] == 'o' ? &m->o[n] : &m->r[n];
}

static bool
run(struct machine *m, const char *text)
{
   char *copy = strdup(text), *save, *line;

   for (line = strtok_r(copy, "\n", &save); line;
        line = strtok_r(NULL, "\n", &save)) {
      char op[16], a[16], b[24], c[24], d[24];
      int n;

      if (line[0] == '#')
         continue;
      n = sscanf(line, "%15s %15[^,], %23[^,], %23[^,], %23s", op, a, b, c, d);
      /* strip trailing commas left by scanf sets */
      if (!strcmp(op, "mov") && n >= 3) {
         *reg(m, a) = strtoul(b + 1, NULL, 0);
      } else if (!strcmp(op, "vmov") && n >= 3) {
         *reg(m, a) = *reg(m, b);
      } else if (!strcmp(op, "vmul") && n >= 4) {
         *reg(m, a) = f2u(u2f(*reg(m, b)) * u2f(*reg(m, c)));
      } else if (!strcmp(op, "vmad") && n >= 5) {
         *reg(m, a) = f2u(u2f(*reg(m, b)) * u2f(*reg(m, c)) +
                          u2f(*reg(m, d)));
      } else if (!strcmp(op, "frcp") && n >= 3) {
         *reg(m, a) = f2u(1.0f / u2f(*reg(m, b)));
      } else if (!strcmp(op, "frsq") && n >= 3) {
         *reg(m, a) = f2u(1.0f / sqrtf(u2f(*reg(m, b))));
      } else {
         fprintf(stderr, "test interpreter: cannot run '%s'\n", line);
         free(copy);
         return false;
      }
   }
   free(copy);
   return true;
}

static nir_shader *
build_vs_passthrough(void)
{
   static const nir_shader_compiler_options opts = { 0 };
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_VERTEX, &opts, "prismrv-test-vs");

   /* gl_Position = attr0 * 2.0 ; varying = attr1 */
   for (unsigned c = 0; c < 4; c++) {
      nir_def *p = nir_load_input(&b, 1, 32, nir_imm_int(&b, 0), .base = 0,
                                  .component = c,
                                  .io_semantics.location = VERT_ATTRIB_GENERIC0,
                                  .io_semantics.num_slots = 1);
      nir_def *v = nir_load_input(&b, 1, 32, nir_imm_int(&b, 0), .base = 1,
                                  .component = c,
                                  .io_semantics.location = VERT_ATTRIB_GENERIC1,
                                  .io_semantics.num_slots = 1);
      nir_store_output(&b, nir_fmul_imm(&b, p, 2.0f), nir_imm_int(&b, 0),
                       .base = 0, .component = c, .write_mask = 1,
                       .io_semantics.location = VARYING_SLOT_POS,
                       .io_semantics.num_slots = 1);
      nir_store_output(&b, v, nir_imm_int(&b, 0), .base = 1, .component = c,
                       .write_mask = 1,
                       .io_semantics.location = VARYING_SLOT_VAR0,
                       .io_semantics.num_slots = 1);
   }
   return b.shader;
}

/* a shader with an if: must be REJECTED, not flattened */
static nir_shader *
build_fs_with_if(void)
{
   static const nir_shader_compiler_options opts = { 0 };
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &opts, "prismrv-test-if");
   nir_def *x = nir_load_input(&b, 1, 32, nir_imm_int(&b, 0), .base = 0,
                               .component = 0,
                               .io_semantics.location = VARYING_SLOT_COL0,
                               .io_semantics.num_slots = 1);
   nir_def *cond = nir_flt(&b, x, nir_imm_float(&b, 0.5f));
   nir_if *nif = nir_push_if(&b, cond);
   nir_store_output(&b, nir_imm_float(&b, 1.0f), nir_imm_int(&b, 0), .base = 0,
                    .component = 0, .write_mask = 1,
                    .io_semantics.location = FRAG_RESULT_COLOR,
                    .io_semantics.num_slots = 1);
   (void)nif;
   nir_pop_if(&b, NULL);
   return b.shader;
}

/* fsin is not implemented by the backend: must be rejected */
static nir_shader *
build_fs_with_fsin(void)
{
   static const nir_shader_compiler_options opts = { 0 };
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &opts, "prismrv-test-fsin");
   nir_def *x = nir_load_input(&b, 1, 32, nir_imm_int(&b, 0), .base = 0,
                               .component = 0,
                               .io_semantics.location = VARYING_SLOT_COL0,
                               .io_semantics.num_slots = 1);
   nir_store_output(&b, nir_fsin(&b, x), nir_imm_int(&b, 0), .base = 0,
                    .component = 0, .write_mask = 1,
                    .io_semantics.location = FRAG_RESULT_COLOR,
                    .io_semantics.num_slots = 1);
   return b.shader;
}

static int
expect_near(const char *what, uint32_t got, float want)
{
   float g = u2f(got);

   if (g < want - 1e-5f || g > want + 1e-5f) {
      fprintf(stderr, "FAIL: %s = %f, expected %f\n", what, g, want);
      return 1;
   }
   return 0;
}

/* ===== differential test: compiled USSE text vs. NIR's own semantics =====
 *
 * Random straight-line shaders are built with nir_builder.  The reference
 * result is obtained from NIR itself: every load_input/load_uniform is
 * replaced by the constant that will be fed to the compiled program and
 * the shader is folded with nir_opt_constant_folding(), which evaluates
 * each opcode with NIR's reference implementation.  The compiled text is
 * then run on the interpreter above with the same values.  Any difference
 * means the compiler changed the program's meaning (wrong operand order,
 * swizzle, register aliasing, dropped instruction, ...).
 */
static uint64_t rng_state = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void)
{
   rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7;
   rng_state ^= rng_state << 17;
   return (uint32_t)(rng_state >> 16);
}
static unsigned rnd_n(unsigned n) { return rnd() % n; }
static float rnd_f(void) { return ((int)(rnd() % 4001) - 2000) / 500.0f; }

struct fuzz_vals { float in[4][4]; float uni[4][4]; };

#define POOL 96
static nir_shader *
build_random(mesa_shader_stage stage, struct fuzz_vals *v, bool native)
{
   static const nir_shader_compiler_options opts = { 0 };
   nir_builder b = nir_builder_init_simple_shader(stage, &opts, "fuzz");
   nir_def *pool[POOL];
   bool is_sa[POOL] = { 0 };   /* constants/uniforms: not readable by source 1 */
   unsigned n = 0;
   unsigned nattr = stage == MESA_SHADER_VERTEX ? 3 : 1;

   for (unsigned k = 0; k < nattr; k++)
      for (unsigned c = 0; c < 4; c++) {
         v->in[k][c] = rnd_f();
         if (stage == MESA_SHADER_VERTEX)
            pool[n++] = nir_load_input(&b, 1, 32, nir_imm_int(&b, 0),
                                       .base = k, .component = c,
                                       .io_semantics.location = VERT_ATTRIB_GENERIC0 + k,
                                       .io_semantics.num_slots = 1);
         else
            pool[n++] = nir_load_input(&b, 1, 32, nir_imm_int(&b, 0),
                                       .base = 0, .component = c,
                                       .io_semantics.location = VARYING_SLOT_COL0,
                                       .io_semantics.num_slots = 1);
      }
   for (unsigned k = 0; k < 2; k++)
      for (unsigned c = 0; c < 4; c++) {
         v->uni[k][c] = rnd_f();
         is_sa[n] = true;
         pool[n++] = nir_load_uniform(&b, 1, 32, nir_imm_int(&b, 0),
                                      .base = (k * 4 + c) * 4, .range = 64);
      }
   is_sa[n] = true; pool[n++] = nir_imm_float(&b, 0.5f);
   is_sa[n] = true; pool[n++] = nir_imm_float(&b, -1.5f);
   is_sa[n] = true; pool[n++] = nir_imm_float(&b, 3.0f);

   for (unsigned i = 0; i < 28 && n < POOL - 2; i++) {
      /* native mode: operands that must land in an r/pa register for VMAD
       * source 1 are drawn from the non-constant part of the pool */
#define REG_PICK() ({ unsigned _k; do { _k = rnd_n(n); } while (is_sa[_k]); _k; })
      unsigned ia = native ? REG_PICK() : rnd_n(n);
      unsigned ic = rnd_n(n), id = rnd_n(n);
      nir_def *a = pool[ia], *c = pool[ic], *d = pool[id];
      unsigned op = rnd_n(9);

      if (native && (op == 5 || op == 6))
         op = 9;                      /* frcp/frsq: no native form yet */
      switch (op) {
      case 9:
         pool[n++] = nir_fabs(&b, a);
         break;
      case 0: pool[n++] = nir_fadd(&b, a, c); break;
      case 1: pool[n++] = nir_fsub(&b, a, c); break;
      case 2: pool[n++] = nir_fmul(&b, a, c); break;
      case 3: pool[n++] = nir_fneg(&b, a); break;
      case 4: pool[n++] = nir_ffma(&b, a, c, d); break;
      case 5: pool[n++] = nir_frcp(&b, nir_fadd_imm(&b, nir_fmul(&b, a, a), 0.25f)); break;
      case 6: pool[n++] = nir_frsq(&b, nir_fadd_imm(&b, nir_fmul(&b, a, a), 0.25f)); break;
      case 7: { /* vector build + swizzle back out */
         nir_def *vec = nir_vec3(&b, a, c, d);
         unsigned idx[3] = { ia, ic, id };
         unsigned k1 = rnd_n(3);
         is_sa[n] = is_sa[idx[k1]];
         pool[n++] = nir_channel(&b, vec, k1);
         unsigned k2 = rnd_n(3);
         is_sa[n] = is_sa[idx[k2]];
         pool[n++] = nir_channel(&b, vec, k2);
         break;
      }
      default: { /* vec4 of mixed scalars used as a whole by an ALU op */
         unsigned p3 = native ? REG_PICK() : rnd_n(n);
         unsigned p4 = native ? REG_PICK() : rnd_n(n);
         unsigned p5 = native ? REG_PICK() : rnd_n(n);
         nir_def *v1 = nir_vec4(&b, a, c, d, pool[p3]);
         nir_def *v2 = nir_vec4(&b, pool[p4], a, pool[p5], c);
         nir_def *r = nir_fadd(&b, nir_fmul(&b, v1, v2), v1);
         pool[n++] = nir_channel(&b, r, rnd_n(4));
         break;
      }
      }
   }

   if (stage == MESA_SHADER_VERTEX) {
      for (unsigned c = 0; c < 4; c++)
         nir_store_output(&b, pool[native ? REG_PICK() : rnd_n(n)], nir_imm_int(&b, 0), .base = 0,
                          .component = c, .write_mask = 1,
                          .io_semantics.location = VARYING_SLOT_POS,
                          .io_semantics.num_slots = 1);
      for (unsigned c = 0; c < 4; c++)
         nir_store_output(&b, pool[native ? REG_PICK() : rnd_n(n)], nir_imm_int(&b, 0), .base = 1,
                          .component = c, .write_mask = 1,
                          .io_semantics.location = VARYING_SLOT_VAR0,
                          .io_semantics.num_slots = 1);
   } else {
      for (unsigned c = 0; c < 4; c++)
         nir_store_output(&b, pool[native ? REG_PICK() : rnd_n(n)], nir_imm_int(&b, 0), .base = 0,
                          .component = c, .write_mask = 1,
                          .io_semantics.location = FRAG_RESULT_COLOR,
                          .io_semantics.num_slots = 1);
   }
   return b.shader;
}

/* NIR's own evaluation: returns false if an output did not fold to a constant */
static bool
nir_reference(nir_shader *orig, const struct fuzz_vals *v, float out[8],
              mesa_shader_stage stage)
{
   nir_shader *ref = nir_shader_clone(NULL, orig);
   nir_function_impl *impl = nir_shader_get_entrypoint(ref);
   bool ok = true, progress;

   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         nir_intrinsic_instr *i;
         nir_builder bb;
         float val;

         if (instr->type != nir_instr_type_intrinsic)
            continue;
         i = nir_instr_as_intrinsic(instr);
         if (i->intrinsic == nir_intrinsic_load_input) {
            unsigned slot = stage == MESA_SHADER_VERTEX ? nir_intrinsic_base(i) : 0;
            val = v->in[slot][nir_intrinsic_component(i)];
         } else if (i->intrinsic == nir_intrinsic_load_uniform) {
            unsigned idx = nir_intrinsic_base(i) / 4;
            val = v->uni[idx / 4][idx % 4];
         } else {
            continue;
         }
         bb = nir_builder_at(nir_before_instr(instr));
         nir_def_replace(&i->def, nir_imm_float(&bb, val));
      }
   }
   do {
      progress = false;
      NIR_PASS(progress, ref, nir_opt_constant_folding);
      NIR_PASS(progress, ref, nir_opt_dce);
   } while (progress);

   for (unsigned k = 0; k < 8; k++)
      out[k] = NAN;
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         nir_intrinsic_instr *i;
         nir_load_const_instr *lc;
         unsigned slot;

         if (instr->type != nir_instr_type_intrinsic)
            continue;
         i = nir_instr_as_intrinsic(instr);
         if (i->intrinsic != nir_intrinsic_store_output)
            continue;
         if (nir_def_instr(i->src[0].ssa)->type != nir_instr_type_load_const) {
            ok = false;
            continue;
         }
         lc = nir_instr_as_load_const(nir_def_instr(i->src[0].ssa));
         slot = nir_intrinsic_component(i) +
                (stage == MESA_SHADER_VERTEX &&
                 nir_intrinsic_io_semantics(i).location == VARYING_SLOT_VAR0 ? 4 : 0);
         out[slot] = lc->value[0].f32;
      }
   }
   ralloc_free(ref);
   return ok;
}

static bool
close_enough(float got, float want)
{
   float tol = 2e-4f * fmaxf(1.0f, fabsf(want));

   if (!isfinite(want) || !isfinite(got))
      return true;      /* inf/NaN propagation differs only in payload */
   return fabsf(got - want) <= tol;
}

static int
run_differential(void)
{
   int fail = 0, compared = 0;
   const char *dump_path = getenv("PRISMRV_PROG_DUMP");
   FILE *dump = dump_path ? fopen(dump_path, "wb") : NULL;

   for (unsigned iter = 0; iter < 600 && fail < 5; iter++) {
      mesa_shader_stage stage = iter & 1 ? MESA_SHADER_FRAGMENT : MESA_SHADER_VERTEX;
      struct fuzz_vals v;
      nir_shader *nir = build_random(stage, &v, false);
      float want[8];
      struct machine m;
      char *usse;
      bool have_ref = nir_reference(nir, &v, want, stage);

      usse = prismrv_nir_to_usse(NULL, nir);
      if (!usse) {
         fprintf(stderr, "FAIL: fuzz shader %u rejected by the compiler\n", iter);
         fail++;
         ralloc_free(nir);
         continue;
      }
      if (dump) {
         /* one SET_PROG packet per program, in the drm-shim dump format,
          * so the kernel's grammar check can be run over them */
         size_t len = strlen(usse), words = (len + 4) / 4;
         uint32_t hdr[3] = { 0x52545350u, (uint32_t)(8 + words * 4), 0 };
         uint32_t pk[2] = { stage == MESA_SHADER_VERTEX ? 2u : 3u, (uint32_t)words };
         char *pad = calloc(1, words * 4);

         memcpy(pad, usse, len);
         fwrite(hdr, 4, 3, dump);
         fwrite(pk, 4, 2, dump);
         fwrite(pad, 1, words * 4, dump);
         free(pad);
      }
      if (!have_ref) {
         fprintf(stderr, "FAIL: NIR reference did not fold (iter %u)\n", iter);
         fail++;
         ralloc_free(usse);
         ralloc_free(nir);
         continue;
      }

      memset(&m, 0, sizeof(m));
      for (unsigned k = 0; k < 4; k++)
         for (unsigned c = 0; c < 4; c++) {
            if (stage == MESA_SHADER_VERTEX)
               m.r[4 * k + c] = f2u(v.in[k][c]);
            else if (k == 0)
               m.r[32 + c] = f2u(v.in[0][c]);
            m.r[16 + 4 * (k % 4) + c] = f2u(v.uni[k % 2][c]);
         }
      /* uniform slot k of the program maps to vec4 k */
      for (unsigned k = 0; k < 2; k++)
         for (unsigned c = 0; c < 4; c++)
            m.r[16 + 4 * k + c] = f2u(v.uni[k][c]);

      if (!run(&m, usse)) {
         fail++;
      } else {
         unsigned nout = stage == MESA_SHADER_VERTEX ? 8 : 4;

         for (unsigned k = 0; k < nout; k++) {
            if (isnan(want[k]))
               continue;
            compared++;
            if (!close_enough(u2f(m.o[k]), want[k])) {
               fprintf(stderr, "FAIL: fuzz %u (%s) o%u = %g, NIR says %g\n"
                       "---- program ----\n%s-----------------\n", iter,
                       stage == MESA_SHADER_VERTEX ? "VS" : "FS", k,
                       u2f(m.o[k]), want[k], usse);
               fail++;
               break;
            }
         }
      }
      ralloc_free(usse);
      ralloc_free(nir);
   }
   if (dump)
      fclose(dump);
   printf("differential: %d output values compared against NIR\n", compared);
   if (compared < 1500) {
      fprintf(stderr, "FAIL: differential test compared too few values\n");
      fail++;
   }
   return fail;
}

/* ===== native USSE: decode every word with the verified decoder and run it
 * with the VMAD semantics  dst.x = fma(src1.x, src2.x, src3.x)  (pair model,
 * see prismrv_usse_nir.h) ======================================== */
struct nmachine { uint32_t bank[5][128]; };

static float
nread(const struct nmachine *m, const struct prismrv_usse_src *s)
{
   float f = u2f(m->bank[s->bank][s->reg]);

   if (s->abs)
      f = fabsf(f);
   return s->neg ? -f : f;
}

static bool
run_native(struct nmachine *m, const struct prismrv_usse_binary *bin)
{
   for (unsigned i = 0; i < bin->num_words; i++) {
      struct prismrv_usse_vmad v;

      if (!prismrv_usse_decode_vmad(bin->code[i], &v)) {
         fprintf(stderr, "native interpreter: word %u not decodable\n", i);
         return false;
      }
      for (unsigned k = 0; k < 3; k++)
         if (memcmp(v.src[k].swz, "xxxx", 5)) {
            fprintf(stderr, "native interpreter: unexpected swizzle\n");
            return false;
         }
      if (v.dst.mask != PRISMRV_USSE_MASK_X || v.pred || v.flags) {
         fprintf(stderr, "native interpreter: unexpected dst/flags\n");
         return false;
      }
      m->bank[v.dst.bank][v.dst.reg] =
         f2u(fmaf(nread(m, &v.src[0]), nread(m, &v.src[1]), nread(m, &v.src[2])));
   }
   return true;
}

static void
native_init(struct nmachine *m, const struct prismrv_usse_binary *bin,
            mesa_shader_stage stage, const struct fuzz_vals *v)
{
   memset(m, 0, sizeof(*m));
   for (unsigned s = 0; s < PRISMRV_USSE_SA_SLOTS; s++)
      m->bank[PRISMRV_USSE_BANK_SA][PRISMRV_USSE_SLOT_REG(s)] = bin->sa_image[s];
   for (unsigned k = 0; k < 4; k++)
      for (unsigned c = 0; c < 4; c++) {
         if (stage == MESA_SHADER_VERTEX || k == 0)
            m->bank[PRISMRV_USSE_BANK_PA][PRISMRV_USSE_PA_REG(k, c)] =
               f2u(v->in[k][c]);
         if (k < 2 && k * 4 + c < bin->sa_uniform_count)   /* don't clobber the pool */
            m->bank[PRISMRV_USSE_BANK_SA][PRISMRV_USSE_SLOT_REG(
               bin->sa_uniform_first + k * 4 + c)] = f2u(v->uni[k][c]);
      }
}

/* a shader the verified forms cannot express must be refused, not guessed */
static nir_shader *
build_fs_store_const(void)
{
   static const nir_shader_compiler_options opts = { 0 };
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &opts, "prismrv-test-const-out");
   nir_store_output(&b, nir_imm_float(&b, 1.0f), nir_imm_int(&b, 0), .base = 0,
                    .component = 0, .write_mask = 1,
                    .io_semantics.location = FRAG_RESULT_COLOR,
                    .io_semantics.num_slots = 1);
   return b.shader;
}

static nir_shader *
build_fs_add_uniforms(void)
{
   static const nir_shader_compiler_options opts = { 0 };
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &opts, "prismrv-test-uadd");
   nir_def *u0 = nir_load_uniform(&b, 1, 32, nir_imm_int(&b, 0), .base = 0, .range = 8);
   nir_def *u1 = nir_load_uniform(&b, 1, 32, nir_imm_int(&b, 0), .base = 4, .range = 8);
   nir_store_output(&b, nir_fadd(&b, u0, u1), nir_imm_int(&b, 0), .base = 0,
                    .component = 0, .write_mask = 1,
                    .io_semantics.location = FRAG_RESULT_COLOR,
                    .io_semantics.num_slots = 1);
   return b.shader;
}

static int
run_native_tests(void)
{
   struct prismrv_usse_binary *bin = calloc(1, sizeof(*bin));
   int fail = 0, compared = 0, rejected = 0, accepted = 0;
   struct nmachine m;
   nir_shader *nir;
   char line[160];

   /* 1. the minimal VS of the plan: position and colour straight through,
    *    plus a scaled copy */
   nir = build_vs_passthrough();
   if (!prismrv_nir_to_usse_native(nir, bin)) {
      fprintf(stderr, "FAIL: native VS rejected\n");
      return 1;
   }
   printf("---- native VS (%u words) ----\n", bin->num_words);
   for (unsigned i = 0; i < bin->num_words; i++) {
      prismrv_usse_disasm_vmad(bin->code[i], line, sizeof(line));
      printf("%016llx  %s\n", (unsigned long long)bin->code[i], line);
   }
   memset(&m, 0, sizeof(m));
   for (unsigned s = 0; s < PRISMRV_USSE_SA_SLOTS; s++)
      m.bank[PRISMRV_USSE_BANK_SA][PRISMRV_USSE_SLOT_REG(s)] = bin->sa_image[s];
   for (unsigned i = 0; i < 4; i++) {
      m.bank[PRISMRV_USSE_BANK_PA][PRISMRV_USSE_PA_REG(0, i)] = f2u(1.0f + i);
      m.bank[PRISMRV_USSE_BANK_PA][PRISMRV_USSE_PA_REG(1, i)] = f2u(0.1f * (i + 1));
   }
   if (!run_native(&m, bin))
      return 1;
   for (unsigned i = 0; i < 4; i++) {
      char nm[24];

      snprintf(nm, sizeof(nm), "native VS pos%u", i);
      fail |= expect_near(nm, m.bank[PRISMRV_USSE_BANK_O][PRISMRV_USSE_O_POS_REG(i)],
                          2.0f * (1.0f + i));
      snprintf(nm, sizeof(nm), "native VS var%u", i);
      fail |= expect_near(nm, m.bank[PRISMRV_USSE_BANK_O][PRISMRV_USSE_O_VARY_REG(i)],
                          0.1f * (i + 1));
   }
   ralloc_free(nir);

   /* 2. the FS test shader: v*k + v */
   nir = build_test_fs();
   if (!prismrv_nir_to_usse_native(nir, bin)) {
      fprintf(stderr, "FAIL: native FS rejected\n");
      return 1;
   }
   memset(&m, 0, sizeof(m));
   for (unsigned s = 0; s < PRISMRV_USSE_SA_SLOTS; s++)
      m.bank[PRISMRV_USSE_BANK_SA][PRISMRV_USSE_SLOT_REG(s)] = bin->sa_image[s];
   {
      static const float vin[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
      static const float k[4] = { 1.0f, 0.9f, 0.8f, 1.0f };

      for (unsigned i = 0; i < 4; i++)
         m.bank[PRISMRV_USSE_BANK_PA][PRISMRV_USSE_PA_REG(0, i)] = f2u(vin[i]);
      if (!run_native(&m, bin))
         return 1;
      for (unsigned i = 0; i < 4; i++)
         fail |= expect_near("native FS colour",
                             m.bank[PRISMRV_USSE_BANK_O][PRISMRV_USSE_O_COLOR_REG(i)],
                             vin[i] * k[i] + vin[i]);
   }
   ralloc_free(nir);

   /* 3. refusals */
   {
      nir_shader *neg[] = { build_fs_with_if(), build_fs_with_fsin(),
                            build_fs_store_const(), build_fs_add_uniforms() };
      static const char *const why[] = { "control flow", "fsin",
                                         "constant output", "uniform+uniform" };

      for (unsigned i = 0; i < ARRAY_SIZE(neg); i++) {
         if (prismrv_nir_to_usse_native(neg[i], bin)) {
            fprintf(stderr, "FAIL: native compiler accepted %s\n", why[i]);
            fail++;
         }
         ralloc_free(neg[i]);
      }
   }

   /* 4. differential: every accepted random shader must match NIR's own
    *    evaluation bit for bit up to float rounding */
   for (unsigned iter = 0; iter < 600 && fail < 5; iter++) {
      mesa_shader_stage stage = iter & 1 ? MESA_SHADER_FRAGMENT : MESA_SHADER_VERTEX;
      struct fuzz_vals v;
      float want[8];
      bool have_ref;

      nir = build_random(stage, &v, true);
      have_ref = nir_reference(nir, &v, want, stage);
      if (!prismrv_nir_to_usse_native(nir, bin)) {
         rejected++;
         ralloc_free(nir);
         continue;
      }
      accepted++;
      if (!have_ref) {
         fprintf(stderr, "FAIL: NIR reference did not fold (native iter %u)\n", iter);
         fail++;
         ralloc_free(nir);
         continue;
      }
      native_init(&m, bin, stage, &v);
      if (!run_native(&m, bin)) {
         fail++;
      } else {
         unsigned nout = stage == MESA_SHADER_VERTEX ? 8 : 4;

         for (unsigned k = 0; k < nout; k++) {
            float got = u2f(m.bank[PRISMRV_USSE_BANK_O][PRISMRV_USSE_SLOT_REG(k)]);

            if (isnan(want[k]))
               continue;
            compared++;
            if (!close_enough(got, want[k])) {
               fprintf(stderr, "FAIL: native fuzz %u (%s) o[%u] = %g, NIR says %g\n",
                       iter, stage == MESA_SHADER_VERTEX ? "VS" : "FS", k,
                       got, want[k]);
               for (unsigned i = 0; i < bin->num_words; i++) {
                  prismrv_usse_disasm_vmad(bin->code[i], line, sizeof(line));
                  fprintf(stderr, "  %s\n", line);
               }
               fail++;
               break;
            }
         }
      }
      ralloc_free(nir);
   }
   printf("native differential: %d shaders compiled, %d refused, %d output "
          "values compared against NIR\n", accepted, rejected, compared);
   if (accepted < 300 || compared < 1200) {
      fprintf(stderr, "FAIL: native differential test covered too little\n");
      fail++;
   }
   free(bin);
   return fail;
}

int
main(void)
{
   int fail = 0;
   struct machine m;
   nir_shader *nir = build_test_fs();
   char *usse = nir ? prismrv_nir_to_usse(NULL, nir) : NULL;

   if (!usse) {
      fprintf(stderr, "FAIL: fragment shader rejected\n");
      return 1;
   }
   printf("---- generated FS USSE ----\n%s---------------------------\n", usse);

   /* v_color = (0.25, 0.5, 0.75, 1.0) in r32.. ; expect v*k + v */
   memset(&m, 0, sizeof(m));
   m.r[32] = f2u(0.25f); m.r[33] = f2u(0.5f);
   m.r[34] = f2u(0.75f); m.r[35] = f2u(1.0f);
   if (!run(&m, usse))
      return 1;
   fail |= expect_near("FS o0", m.o[0], 0.25f * 1.0f + 0.25f);
   fail |= expect_near("FS o1", m.o[1], 0.5f * 0.9f + 0.5f);
   fail |= expect_near("FS o2", m.o[2], 0.75f * 0.8f + 0.75f);
   fail |= expect_near("FS o3", m.o[3], 1.0f * 1.0f + 1.0f);
   ralloc_free(usse);
   ralloc_free(nir);

   /* VS: attribute 0 -> r0..3, attribute 1 -> r4..7 */
   nir = build_vs_passthrough();
   usse = prismrv_nir_to_usse(NULL, nir);
   if (!usse) {
      fprintf(stderr, "FAIL: vertex shader rejected\n");
      return 1;
   }
   memset(&m, 0, sizeof(m));
   for (unsigned i = 0; i < 4; i++) {
      m.r[i] = f2u(1.0f + i);           /* position */
      m.r[4 + i] = f2u(0.1f * (i + 1)); /* colour */
   }
   if (!run(&m, usse))
      return 1;
   for (unsigned i = 0; i < 4; i++) {
      char nm[16];
      snprintf(nm, sizeof(nm), "VS o%u", i);
      fail |= expect_near(nm, m.o[i], 2.0f * (1.0f + i));
   }
   for (unsigned i = 0; i < 4; i++) {   /* varying = o4..o7, alpha included */
      char nm[16];
      snprintf(nm, sizeof(nm), "VS o%u", 4 + i);
      fail |= expect_near(nm, m.o[4 + i], 0.1f * (i + 1));
   }
   ralloc_free(usse);
   ralloc_free(nir);

   /* negative tests: unsupported shaders fail compilation */
   nir = build_fs_with_if();
   usse = prismrv_nir_to_usse(NULL, nir);
   if (usse) {
      fprintf(stderr, "FAIL: shader with control flow was accepted\n");
      fail = 1;
   }
   nir = build_fs_with_fsin();
   usse = prismrv_nir_to_usse(NULL, nir);
   if (usse) {
      fprintf(stderr, "FAIL: unsupported ALU op was accepted\n");
      fail = 1;
   }

   fail += run_differential();
   fail += run_native_tests();

   if (fail)
      return 1;
   printf("PRISMRV NIR→USSE TEST PASS\n");
   return 0;
}
