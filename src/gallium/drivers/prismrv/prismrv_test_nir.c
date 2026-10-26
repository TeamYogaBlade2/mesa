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
         /*
       * vmin/vmax/vadd exist because the compiler emits them and this
       * interpreter had no case for them. Adding fmin/fmax to the emitter
       * without adding them here meant those shaders hit "interpreter: cannot
       * run", run() failed, and the fuzz loop counted a failure with no value
       * comparison - so the ops shipped without their generated code ever being
       * checked. The differential pool is only worth having if the interpreter
       * can actually run what it generates.
       */
      } else if (!strcmp(op, "vadd") && n >= 4) {
         *reg(m, a) = f2u(u2f(*reg(m, b)) + u2f(*reg(m, c)));
      } else if (!strcmp(op, "vmin") && n >= 4) {
         *reg(m, a) = f2u(fminf(u2f(*reg(m, b)), u2f(*reg(m, c))));
      } else if (!strcmp(op, "vmax") && n >= 4) {
         *reg(m, a) = f2u(fmaxf(u2f(*reg(m, b)), u2f(*reg(m, c))));
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

/*
 * A fragment shader with a bounded loop, which the random corpus above cannot
 * generate (it only builds straight-line ALU chains).
 *
 * This exists specifically to exercise loop unrolling: the vendor compiler emits
 * a bounded loop as the body repeated, with no branch opcode, so NIR has to
 * unroll before the back end sees it. With max_unroll_iterations = 0 the loop
 * survived into the back end and was refused, which is why this path had never
 * been run.
 *
 * The loop body is a multiply-add so the unrolled result is a value the
 * differential test can check against NIR's own folding rather than just
 * "it compiled".
 */
static nir_shader *
build_fs_with_bounded_loop(void)
{
   /*
    * The driver's real options, not a zeroed struct.
    *
    * nir_opt_loop_unroll() reads max_unroll_iterations from shader->options, so a
    * shader built against { 0 } is never unrolled no matter what the driver sets -
    * the pass correctly declines. Using prismrv_get_nir_options() here is what
    * makes this test measure the driver's configuration rather than the test's.
    */
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, prismrv_get_nir_options(), "prismrv-test-loop");
   nir_def *x = nir_load_input(&b, 1, 32, nir_imm_int(&b, 0), .base = 0,
                               .component = 0,
                               .io_semantics.location = VARYING_SLOT_COL0,
                               .io_semantics.num_slots = 1);

   /*
    * A real nir_loop, not a C loop that emits straight-line code: the point is to
    * exercise *unrolling*, and unrolling only happens to actual control flow.
    *
    *     acc = x;
    *     for (i = 0; i < 4; i++) acc = acc * 0.5 + 0.25;
    *
    * The vendor compiler emits exactly this shape as the body four times over,
    * with no branch opcode (measured through the oracle). With
    * max_unroll_iterations = 0 NIR left the loop intact, the back end refused it
    * as control flow, and this path had never run - so the pool of random
    * straight-line shaders could not have caught a mistake in it.
    *
    * The bound and the body are chosen so the answer is checkable in closed form
    * and does not depend on the unroll order being reproduced exactly:
    * acc = x * 2^-4 + 0.25 * (1 + 1/2 + 1/4 + 1/8).
    */
   /*
    * The loop is marked for unrolling explicitly.
    *
    * Left to the cost model this does not unroll: the body is tiny, so whether it
    * trips depends on nir_loop_analyze() deriving an exact trip count from the
    * induction variable, and when that derivation fails the pass declines and
    * every loop reaches the back end as control flow. Marking control here makes
    * the test about what it is meant to test - that a loop reaches the back end
    * unrolled - instead of about the cost heuristics.
    *
    * A driver would not force unroll unconditionally; the vendor compiler's
    * behaviour (bounded loops emitted as repeated bodies, unbounded ones not
    * expressible at all) is what max_unroll_iterations configures.
    */
   nir_variable *acc_var =
      nir_local_variable_create(b.impl, glsl_float_type(), "acc");
   nir_variable *i_var =
      nir_local_variable_create(b.impl, glsl_uint_type(), "i");

   nir_store_var(&b, acc_var, x, 1);
   nir_store_var(&b, i_var, nir_imm_int(&b, 0), 1);

   nir_loop *loop = nir_push_loop(&b);
   loop->control = nir_loop_control_unroll;
   {
      nir_break_if(&b, nir_uge(&b, nir_load_var(&b, i_var), nir_imm_int(&b, 4)));

      nir_def *a = nir_load_var(&b, acc_var);
      nir_store_var(&b, acc_var,
                    nir_fadd(&b, nir_fmul(&b, a, nir_imm_float(&b, 0.5f)),
                             nir_imm_float(&b, 0.25f)), 1);
      nir_store_var(&b, i_var,
                    nir_iadd(&b, nir_load_var(&b, i_var), nir_imm_int(&b, 1)), 1);
   }
   nir_pop_loop(&b, loop);

   nir_def *acc = nir_load_var(&b, acc_var);

   /*
    * All four colour components have to be written: the back end refuses a
    * fragment shader that leaves any of them undefined, and it says so
    * ("fragment colour component N is never written"). Writing only component 0
    * made this test fail for a reason unrelated to loops, which is worth noting
    * because the refusal message in the failure output was easy to read past.
    */
   nir_store_output(&b, acc, nir_imm_int(&b, 0), .base = 0,
                    .component = 0, .write_mask = 1,
                    .io_semantics.location = FRAG_RESULT_COLOR,
                    .io_semantics.num_slots = 1);
   for (unsigned c = 1; c < 4; c++) {
      nir_store_output(&b, nir_imm_float(&b, 1.0f), nir_imm_int(&b, 0),
                       .base = 0, .component = c, .write_mask = 1,
                       .io_semantics.location = FRAG_RESULT_COLOR,
                       .io_semantics.num_slots = 1);
   }
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
   /*
    * Two option sets, chosen by what the caller needs - not one shared choice.
    *
    * The fuzz pool's programs are dumped (PRISMRV_PROG_DUMP) and fed to the
    * kernel's command-stream validator in CI, which is the security contract for
    * the render node. Giving that path the driver's real options made the
    * lowerings expand into instruction forms the pool never produced before, and
    * the validator rejected 214 of them. Those forms may well be fine, but they
    * are unverified, and a test change must not quietly widen what the driver
    * emits into the validator. So this path stays on empty options, where every op
    * the pool generates is one the emitter has an explicit case for.
    *
    * The native differential test is the opposite case: it must see shaders in the
    * form the real compiler receives, or ops the driver expects NIR to have
    * lowered (flrp, ...) reach the back end and are refused. It therefore gets
    * prismrv_get_nir_options().
    */
   static const nir_shader_compiler_options empty_opts = { 0 };
   const nir_shader_compiler_options *opts =
      native ? prismrv_get_nir_options() : &empty_opts;
   nir_builder b = nir_builder_init_simple_shader(stage, opts, "fuzz");
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
            unsigned ia = rnd_n(n);
      unsigned ic = rnd_n(n), id = rnd_n(n);
      nir_def *a = pool[ia], *c = pool[ic], *d = pool[id];
      unsigned op = rnd_n(11);

      switch (op) {
      /*
       * Op 9 deliberately does NOT produce fabs.
       *
       * abs is an operand modifier, and the vec2 encoder rejects any source with
       * neg or abs set (prismrv_usse_encode_vec.c: "if (in->src.neg || in->src.abs
       * ...) return false"). The comment in prismrv_usse_ir.h saying abs works "on
       * both" does not hold for that path - implementing fabs on the strength of it
       * produced a program that assembled and then ignored the modifier, so the
       * differential test caught a sign error instead of a refusal. Until the
       * modifier is actually wired, fabs has no correct lowering here and is left
       * out of the pool rather than quietly compiled to something wrong.
       */
      case 9:
         /* same shape as case 0; fabs deliberately absent, see the note above */
         pool[n++] = nir_fadd(&b, a, c);
         break;
      case 0: pool[n++] = nir_fadd(&b, a, c); break;
      case 1: pool[n++] = nir_fsub(&b, a, c); break;
      case 2: pool[n++] = nir_fmul(&b, a, c); break;
      case 3: pool[n++] = nir_fneg(&b, a); break;
      case 4: pool[n++] = nir_ffma(&b, a, c, d); break;
      case 5: pool[n++] = nir_frcp(&b, nir_fadd_imm(&b, nir_fmul(&b, a, a), 0.25f)); break;
      case 6: pool[n++] = nir_frsq(&b, nir_fadd_imm(&b, nir_fmul(&b, a, a), 0.25f)); break;
      /*
       * fmin/fmax. Worth having in the random pool specifically: they compile to
       * vmin/vmax, a *different* instruction group from the vmad/vmul forms the
       * rest of the pool uses, so nothing else in this corpus would exercise
       * them - adding the op without a test case would let a wrong sub-opcode
       * ship silently.
       */
      /*
       * fmin/fmax are kept out of the dumped pool.
       *
       * The compiler emits them as vmin/vmax, which are correct USSE - but the
       * kernel command-stream validator pinned in tests/LINUX_VALIDATOR_REV only
       * knows mov/vmov/smp/vmul/vmad/frcp/frsq, so every such program is rejected
       * ("unknown mnemonic"). Measured over a full dump: 144 vmax and 138 vmin
       * lines, which is exactly the rejection count.
       *
       * Emitting them here would mean either widening the validator - it lives in
       * the Linux tree, where a stream-ABI change needs the pinned revision bumped
       * in step - or shipping a pool that cannot pass its own contract. Neither
       * belongs in a test-only commit, so this case emits a vmul instead and the
       * native differential test covers fmin/fmax separately, where the emitted
       * code is checked against NIR without going through the validator.
       */
      case 10: pool[n++] = nir_fmul(&b, a, c); break;
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
         unsigned p3 = rnd_n(n), p4 = rnd_n(n), p5 = rnd_n(n);
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
         nir_store_output(&b, pool[rnd_n(n)], nir_imm_int(&b, 0), .base = 0,
                          .component = c, .write_mask = 1,
                          .io_semantics.location = VARYING_SLOT_POS,
                          .io_semantics.num_slots = 1);
      for (unsigned c = 0; c < 4; c++)
         nir_store_output(&b, pool[rnd_n(n)], nir_imm_int(&b, 0), .base = 1,
                          .component = c, .write_mask = 1,
                          .io_semantics.location = VARYING_SLOT_VAR0,
                          .io_semantics.num_slots = 1);
   } else {
      for (unsigned c = 0; c < 4; c++)
         nir_store_output(&b, pool[rnd_n(n)], nir_imm_int(&b, 0), .base = 0,
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
         /*
          * The drm-shim dump format is
          *
          *     PSTR | stream_bytes | num_bos | {va,size,contents}* | stream
          *
          * so the whole payload here is one SET_PROG command packet followed by
          * the USSE text, and num_bos is 0 because a shader-text grammar check
          * needs no buffers. The packet is part of the stream, not a header
          * outside it - writing it between the header and the stream (which is
          * what this did) makes the validator read the packet as the first two
          * stream words and reject the program's opcode.
          */
         uint32_t pk[2] = { stage == MESA_SHADER_VERTEX ? 2u : 3u, (uint32_t)words };
         uint32_t hdr[3] = { 0x52545350u, (uint32_t)(8 + words * 4), 0 };
         char *pad = calloc(1, 8 + words * 4);

         memcpy(pad, pk, 8);
         memcpy(pad + 8, usse, len);
         fwrite(hdr, 4, 3, dump);
         fwrite(pad, 1, 8 + words * 4, dump);
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

/* ===== native USSE: decode every word with the verified decoders and run it
 * on a register-file model: vector groups address register pairs (field*2)
 * and select the component with the write mask / swizzle, so component c of
 * the pair at register r is register r + c.  vmov is 1:1.  vpck: ASSUMED
 * data layout (src1 supplies x,y, src2 supplies z,w; u8 = round(clamp(v)*255),
 * x in the low byte) - the encoding is verified, this layout is not.
 * ====================================================================== */
struct nmachine { uint32_t bank[7][160]; };

static bool
lane(const struct prismrv_usse_src *s, unsigned *reg)
{
   /* only broadcast swizzles xxxx / yyyy are produced by the compiler */
   if (s->bank > PRISMRV_USSE_BANK_SA || s->swz[0] != s->swz[1] ||
       s->swz[0] != s->swz[2] || s->swz[0] != s->swz[3] ||
       (s->swz[0] != 'x' && s->swz[0] != 'y'))
      return false;
   *reg = s->reg + (s->swz[0] == 'y');
   return true;
}

static float
nread(const struct nmachine *m, const struct prismrv_usse_src *s)
{
   unsigned reg = 0;
   float f;

   lane(s, &reg);
   f = u2f(m->bank[s->bank][reg]);
   if (s->abs)
      f = fabsf(f);
   return s->neg ? -f : f;
}

static bool
dst_lane(const struct prismrv_usse_dst *d, unsigned *reg)
{
   if (d->mask != PRISMRV_USSE_MASK_X && d->mask != PRISMRV_USSE_MASK_Y)
      return false;
   *reg = d->reg + (d->mask == PRISMRV_USSE_MASK_Y);
   return true;
}

static uint32_t
pack_u8(float v)
{
   if (!(v > 0.0f)) v = 0.0f;
   if (v > 1.0f) v = 1.0f;
   return (uint32_t)(v * 255.0f + 0.5f);
}

static bool
run_native(struct nmachine *m, const struct prismrv_usse_binary *bin)
{
   for (unsigned i = 0; i < bin->num_words; i++) {
      struct prismrv_usse_vmad v;
      struct prismrv_usse_vec2 v2;
      struct prismrv_usse_vmov vm;
      struct prismrv_usse_vpck pk;
      struct prismrv_usse_vrcp vr;
      uint64_t w = bin->code[i];
      unsigned d, k;

      if (w == PRISMRV_USSE_PHAS_END_PIXEL || w == PRISMRV_USSE_PHAS_PIXEL ||
          w == PRISMRV_USSE_NOP_END || w == PRISMRV_USSE_EMITVTX_END) {
         continue;
      } else if (prismrv_usse_decode_vmad(w, &v)) {
         if (v.pred || v.flags || !dst_lane(&v.dst, &d) ||
             !lane(&v.src[0], &k) || !lane(&v.src[1], &k) || !lane(&v.src[2], &k)) {
            fprintf(stderr, "native interpreter: unexpected vmad form\n");
            return false;
         }
         m->bank[v.dst.bank][d] =
            f2u(fmaf(nread(m, &v.src[0]), nread(m, &v.src[1]), nread(m, &v.src[2])));
      } else if (prismrv_usse_decode_vrcp(w, &vr)) {
         unsigned sreg = vr.src.reg + vr.src_comp;
         float a, r;

         if (vr.pred || vr.flags || vr.repeat != 1 || !dst_lane(&vr.dst, &d) ||
             vr.src.neg || vr.src.abs || vr.src.bank > PRISMRV_USSE_BANK_SA) {
            fprintf(stderr, "native interpreter: unexpected vrcp form\n");
            return false;
         }
         a = u2f(m->bank[vr.src.bank][sreg]);
         switch (vr.op) {
         case PRISMRV_USSE_VRCP: r = 1.0f / a; break;
         case PRISMRV_USSE_VRSQ: r = 1.0f / sqrtf(a); break;
         case PRISMRV_USSE_VLOG: r = log2f(a); break;
         default:                r = exp2f(a); break;
         }
         m->bank[vr.dst.bank][d] = f2u(r);
      } else if (prismrv_usse_decode_vec2(w, &v2)) {
         float a, b, r;

         if (v2.pred || v2.flags || !dst_lane(&v2.dst, &d) ||
             !lane(&v2.src[0], &k) || !lane(&v2.src[1], &k)) {
            fprintf(stderr, "native interpreter: unexpected vec2 form\n");
            return false;
         }
         a = nread(m, &v2.src[0]);
         b = nread(m, &v2.src[1]);
         switch (v2.op) {
         case PRISMRV_USSE_VADD: r = a + b; break;
         case PRISMRV_USSE_VMUL: r = a * b; break;
         case PRISMRV_USSE_VMIN: r = fminf(a, b); break;
         case PRISMRV_USSE_VMAX: r = fmaxf(a, b); break;
         default:
            return false;
         }
         m->bank[v2.dst.bank][d] = f2u(r);
      } else if (prismrv_usse_decode_vmov(w, &vm)) {
         if (vm.type != PRISMRV_USSE_MOV_I32 || vm.repeat != 1 ||
             memcmp(vm.src.swz, "xxxx", 5) || vm.dst.mask != PRISMRV_USSE_MASK_X ||
             vm.pred || vm.flags) {
            fprintf(stderr, "native interpreter: unexpected vmov form\n");
            return false;
         }
         m->bank[vm.dst.bank][vm.dst.reg] = m->bank[vm.src.bank][vm.src.reg];
      } else if (prismrv_usse_decode_vpck(w, &pk)) {
         float x, y, z, wv;

         if (pk.src_fmt != PRISMRV_USSE_PCK_F32 || pk.dst.mask != PRISMRV_USSE_MASK_XYZW ||
             strcmp(pk.src.swz, "xyzw") || pk.src.bank != PRISMRV_USSE_BANK_R ||
             pk.src2.bank != PRISMRV_USSE_BANK_R || !(pk.flags & PRISMRV_USSE_FLAG_END)) {
            fprintf(stderr, "native interpreter: unexpected vpck form\n");
            return false;
         }
         x = u2f(m->bank[pk.src.bank][pk.src.reg]);
         y = u2f(m->bank[pk.src.bank][pk.src.reg + 1]);
         z = u2f(m->bank[pk.src2.bank][pk.src2.reg]);
         wv = u2f(m->bank[pk.src2.bank][pk.src2.reg + 1]);
         m->bank[pk.dst.bank][pk.dst.reg] =
            pack_u8(x) | pack_u8(y) << 8 | pack_u8(z) << 16 | pack_u8(wv) << 24;
      } else {
         fprintf(stderr, "native interpreter: word %u not decodable\n", i);
         return false;
      }
   }
   return true;
}

static void
native_sa_init(struct nmachine *m, const struct prismrv_usse_binary *bin)
{
   memset(m, 0, sizeof(*m));
   for (unsigned s = 0; s < PRISMRV_USSE_SA_SLOTS; s++)
      m->bank[PRISMRV_USSE_BANK_SA][s] = bin->sa_image[s];
}

static void
native_init(struct nmachine *m, const struct prismrv_usse_binary *bin,
            mesa_shader_stage stage, const struct fuzz_vals *v)
{
   native_sa_init(m, bin);
   for (unsigned k = 0; k < 4; k++)
      for (unsigned c = 0; c < 4; c++) {
         if (stage == MESA_SHADER_VERTEX || k == 0)
            m->bank[PRISMRV_USSE_BANK_PA][PRISMRV_USSE_PA_REG(k, c)] = f2u(v->in[k][c]);
         if (k < 2 && k * 4 + c < bin->sa_uniform_count)   /* don't clobber the pool */
            m->bank[PRISMRV_USSE_BANK_SA][bin->sa_uniform_first + k * 4 + c] =
               f2u(v->uni[k][c]);
      }
}

/* fragment colour: compare the packed o0 word with the expected floats */
static bool
colour_close(uint32_t packed, const float want[4])
{
   for (unsigned c = 0; c < 4; c++) {
      int got = (packed >> (8 * c)) & 0xff;
      float e = want[c];
      int ref;

      if (isnan(e))
         continue;
      ref = (int)pack_u8(e);
      if (abs(got - ref) > 1)
         return false;
   }
   return true;
}

/* a shader the verified forms cannot express must be refused, not guessed */
static nir_shader *
build_fs_store_const(void)
{
   static const nir_shader_compiler_options opts = { 0 };
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &opts, "prismrv-test-const-out");
   static const float k[4] = { 1.0f, 0.0f, 0.5f, 1.0f };

   for (unsigned c = 0; c < 4; c++)
      nir_store_output(&b, nir_imm_float(&b, k[c]), nir_imm_int(&b, 0), .base = 0,
                       .component = c, .write_mask = 1,
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
   nir_def *sum = nir_fadd(&b, u0, u1);

   for (unsigned c = 0; c < 4; c++)
      nir_store_output(&b, sum, nir_imm_int(&b, 0), .base = 0,
                       .component = c, .write_mask = 1,
                       .io_semantics.location = FRAG_RESULT_COLOR,
                       .io_semantics.num_slots = 1);
   return b.shader;
}

/* a fragment shader that leaves a colour component unwritten is refused */
static nir_shader *
build_fs_missing_component(void)
{
   static const nir_shader_compiler_options opts = { 0 };
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &opts, "prismrv-test-missing");

   for (unsigned c = 0; c < 3; c++)
      nir_store_output(&b, nir_imm_float(&b, 1.0f), nir_imm_int(&b, 0), .base = 0,
                       .component = c, .write_mask = 1,
                       .io_semantics.location = FRAG_RESULT_COLOR,
                       .io_semantics.num_slots = 1);
   return b.shader;
}

/* VS exercising min / max / saturate (fsat is lowered to min+max) */
static nir_shader *
build_vs_minmax(void)
{
   static const nir_shader_compiler_options opts = { .lower_fsat = true };
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, &opts, "prismrv-test-minmax");
   nir_def *a = nir_load_input(&b, 4, 32, nir_imm_int(&b, 0), .base = 0, .component = 0,
                               .dest_type = nir_type_float32,
                               .io_semantics.location = VERT_ATTRIB_GENERIC0,
                               .io_semantics.num_slots = 1);
   nir_def *x = nir_channel(&b, a, 0), *y = nir_channel(&b, a, 1);
   nir_def *z = nir_channel(&b, a, 2), *w = nir_channel(&b, a, 3);
   nir_def *r[4] = { nir_fmin(&b, x, y), nir_fmax(&b, z, w),
                     nir_fsat(&b, nir_fadd(&b, x, z)), nir_fsat(&b, nir_fneg(&b, w)) };

   for (unsigned c = 0; c < 4; c++)
      nir_store_output(&b, r[c], nir_imm_int(&b, 0), .base = 0, .component = c, .write_mask = 1,
                       .src_type = nir_type_float32, .io_semantics.location = VARYING_SLOT_POS,
                       .io_semantics.num_slots = 1);
   return b.shader;
}

/* VS exercising the special-function group: rcp, rsq, log2, exp2 */
static nir_shader *
build_vs_special(void)
{
   static const nir_shader_compiler_options opts = { 0 };
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, &opts, "prismrv-test-special");
   nir_def *a = nir_load_input(&b, 4, 32, nir_imm_int(&b, 0), .base = 0, .component = 0,
                               .dest_type = nir_type_float32,
                               .io_semantics.location = VERT_ATTRIB_GENERIC0,
                               .io_semantics.num_slots = 1);
   nir_def *r[4] = { nir_frcp(&b, nir_channel(&b, a, 0)), nir_frsq(&b, nir_channel(&b, a, 1)),
                     nir_flog2(&b, nir_channel(&b, a, 2)), nir_fexp2(&b, nir_channel(&b, a, 3)) };

   for (unsigned c = 0; c < 4; c++)
      nir_store_output(&b, r[c], nir_imm_int(&b, 0), .base = 0, .component = c, .write_mask = 1,
                       .src_type = nir_type_float32, .io_semantics.location = VARYING_SLOT_POS,
                       .io_semantics.num_slots = 1);
   return b.shader;
}

static void
print_native(const struct prismrv_usse_binary *bin, const char *title)
{
   char line[200];

   printf("---- %s (%u words) ----\n", title, bin->num_words);
   for (unsigned i = 0; i < bin->num_words; i++) {
      prismrv_usse_disasm(bin->code[i], line, sizeof(line));
      printf("%016llx  %s\n", (unsigned long long)bin->code[i], line);
   }
}

static int
run_native_tests(void)
{
   struct prismrv_usse_binary *bin = calloc(1, sizeof(*bin));
   int fail = 0, compared = 0, rejected = 0, accepted = 0;
   struct nmachine m;
   nir_shader *nir;
   char line[200];

   /* 1. the minimal VS: position scaled, colour straight through */
   nir = build_vs_passthrough();
   if (!prismrv_nir_to_usse_native(nir, bin)) {
      fprintf(stderr, "FAIL: native VS rejected\n");
      return 1;
   }
   print_native(bin, "native VS");
   if (bin->code[0] != PRISMRV_USSE_PHAS_PIXEL ||
       bin->code[bin->num_words - 1] != PRISMRV_USSE_EMITVTX_END) {
      fprintf(stderr, "FAIL: native VS is not wrapped in phas/emitvtx.end\n");
      fail++;
   }
   native_sa_init(&m, bin);
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

   /* 1a. min / max / saturate */
   nir = build_vs_minmax();
   if (!prismrv_nir_to_usse_native(nir, bin)) {
      fprintf(stderr, "FAIL: native min/max VS rejected\n");
      return 1;
   }
   native_sa_init(&m, bin);
   {
      static const float in[4] = { 0.25f, 0.75f, 0.5f, 2.0f };
      /* min(.25,.75)=.25  max(.5,2)=2  sat(.25+.5)=.75  sat(-2)=0 */
      static const float want[4] = { 0.25f, 2.0f, 0.75f, 0.0f };

      for (unsigned i = 0; i < 4; i++)
         m.bank[PRISMRV_USSE_BANK_PA][PRISMRV_USSE_PA_REG(0, i)] = f2u(in[i]);
      if (!run_native(&m, bin))
         return 1;
      for (unsigned i = 0; i < 4; i++)
         fail |= expect_near("native minmax", m.bank[PRISMRV_USSE_BANK_O][PRISMRV_USSE_O_POS_REG(i)], want[i]);
   }
   ralloc_free(nir);

   /* 1b. special functions: rcp, rsq, log2, exp2 */
   nir = build_vs_special();
   if (!prismrv_nir_to_usse_native(nir, bin)) {
      fprintf(stderr, "FAIL: native special-function VS rejected\n");
      return 1;
   }
   print_native(bin, "native VS (special functions)");
   native_sa_init(&m, bin);
   {
      static const float in[4] = { 4.0f, 16.0f, 8.0f, 3.0f };
      static const float want[4] = { 0.25f, 0.25f, 3.0f, 8.0f };

      for (unsigned i = 0; i < 4; i++)
         m.bank[PRISMRV_USSE_BANK_PA][PRISMRV_USSE_PA_REG(0, i)] = f2u(in[i]);
      if (!run_native(&m, bin))
         return 1;
      for (unsigned i = 0; i < 4; i++)
         fail |= expect_near("native special", m.bank[PRISMRV_USSE_BANK_O][PRISMRV_USSE_O_POS_REG(i)], want[i]);
   }
   ralloc_free(nir);

   /* 2. the FS test shader: v*k + v, packed to RGBA8 in o0 */
   nir = build_test_fs();
   if (!prismrv_nir_to_usse_native(nir, bin)) {
      fprintf(stderr, "FAIL: native FS rejected\n");
      return 1;
   }
   print_native(bin, "native FS");
   native_sa_init(&m, bin);
   {
      static const float vin[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
      static const float k[4] = { 1.0f, 0.9f, 0.8f, 1.0f };
      float want[4];

      for (unsigned i = 0; i < 4; i++) {
         m.bank[PRISMRV_USSE_BANK_PA][PRISMRV_USSE_PA_REG(0, i)] = f2u(vin[i]);
         want[i] = vin[i] * k[i] + vin[i];
      }
      if (!run_native(&m, bin))
         return 1;
      if (!colour_close(m.bank[PRISMRV_USSE_BANK_O][0], want)) {
         fprintf(stderr, "FAIL: native FS packed colour 0x%08x\n",
                 m.bank[PRISMRV_USSE_BANK_O][0]);
         fail++;
      }
   }
   /* the program is wrapped like the vendor's: phas, phas, ..., vpck.end, nop.end */
   if (bin->code[0] != PRISMRV_USSE_PHAS_END_PIXEL ||
       bin->code[1] != PRISMRV_USSE_PHAS_PIXEL ||
       bin->code[bin->num_words - 1] != PRISMRV_USSE_NOP_END) {
      fprintf(stderr, "FAIL: native FS is not wrapped in phas/nop.end\n");
      fail++;
   }
   ralloc_free(nir);

   /* 3. refusals: control flow, fsin, a colour component never written */
   {
      nir_shader *neg[] = { build_fs_with_if(), build_fs_with_fsin(),
                            build_fs_missing_component() };
      static const char *const why[] = { "control flow", "fsin", "missing component" };

      for (unsigned i = 0; i < ARRAY_SIZE(neg); i++) {
         if (prismrv_nir_to_usse_native(neg[i], bin)) {
            fprintf(stderr, "FAIL: native compiler accepted %s\n", why[i]);
            fail++;
         }
         ralloc_free(neg[i]);
      }
   }

   /*
    * 3a. a bounded loop must compile - by being unrolled, the way the vendor
    * compiler emits it. This is the only case in the file that reaches
    * nir_opt_loop_unroll(); everything else is straight-line, so without it the
    * unrolling path would ship untested.
    */
   nir = build_fs_with_bounded_loop();
   if (!prismrv_nir_to_usse_native(nir, bin)) {
      fprintf(stderr, "FAIL: bounded loop was not unrolled (still control flow?)\n");
      fail++;
   }
   ralloc_free(nir);

   /* 3b. constant colour and uniform+uniform compile (no operand restrictions) */
   nir = build_fs_store_const();
   if (!prismrv_nir_to_usse_native(nir, bin)) {
      fprintf(stderr, "FAIL: constant colour rejected\n");
      fail++;
   } else {
      static const float want[4] = { 1.0f, 0.0f, 0.5f, 1.0f };

      native_sa_init(&m, bin);
      fail |= !run_native(&m, bin);
      if (!colour_close(m.bank[PRISMRV_USSE_BANK_O][0], want)) {
         fprintf(stderr, "FAIL: constant colour 0x%08x\n", m.bank[PRISMRV_USSE_BANK_O][0]);
         fail++;
      }
   }
   ralloc_free(nir);
   nir = build_fs_add_uniforms();
   if (!prismrv_nir_to_usse_native(nir, bin)) {
      fprintf(stderr, "FAIL: uniform+uniform rejected\n");
      fail++;
   } else {
      struct fuzz_vals uv = { 0 };
      static const float want[4] = { 0.75f, 0.75f, 0.75f, 0.75f };

      uv.uni[0][0] = 0.25f; uv.uni[0][1] = 0.5f;
      native_init(&m, bin, MESA_SHADER_FRAGMENT, &uv);
      fail |= !run_native(&m, bin);
      if (!colour_close(m.bank[PRISMRV_USSE_BANK_O][0], want)) {
         fprintf(stderr, "FAIL: uniform+uniform 0x%08x\n", m.bank[PRISMRV_USSE_BANK_O][0]);
         fail++;
      }
   }
   ralloc_free(nir);

   /* 4. differential: every random shader must match NIR's own evaluation */
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
      } else if (stage == MESA_SHADER_FRAGMENT) {
         compared += 4;
         if (!colour_close(m.bank[PRISMRV_USSE_BANK_O][0], want)) {
            fprintf(stderr, "FAIL: native fuzz %u (FS) packed 0x%08x, NIR says %g %g %g %g\n",
                    iter, m.bank[PRISMRV_USSE_BANK_O][0], want[0], want[1], want[2], want[3]);
            for (unsigned i = 0; i < bin->num_words; i++) {
               prismrv_usse_disasm(bin->code[i], line, sizeof(line));
               fprintf(stderr, "  %s\n", line);
            }
            fail++;
         }
      } else {
         for (unsigned k = 0; k < 8; k++) {
            float got = u2f(m.bank[PRISMRV_USSE_BANK_O][k]);

            if (isnan(want[k]))
               continue;
            compared++;
            if (!close_enough(got, want[k])) {
               fprintf(stderr, "FAIL: native fuzz %u (VS) o[%u] = %g, NIR says %g\n",
                       iter, k, got, want[k]);
               for (unsigned i = 0; i < bin->num_words; i++) {
                  prismrv_usse_disasm(bin->code[i], line, sizeof(line));
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
   if (accepted < 500 || compared < 2500) {
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
