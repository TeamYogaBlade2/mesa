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
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "util/ralloc.h"
#include "compiler/nir/nir_builder.h"
#include "prismrv_program.h"

static nir_shader *
build_test_fs(void)
{
   nir_shader_compiler_options opts = { 0 };
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
   nir_shader_compiler_options opts = { 0 };
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
   nir_shader_compiler_options opts = { 0 };
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
   nir_shader_compiler_options opts = { 0 };
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
   for (unsigned i = 0; i < 3; i++) {   /* varying = o4..o6 */
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

   if (fail)
      return 1;
   printf("PRISMRV NIR→USSE TEST PASS\n");
   return 0;
}
