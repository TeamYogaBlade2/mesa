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

   const struct glsl_type *vec4 = glsl_vector_type(GLSL_TYPE_FLOAT, 4);

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

int
main(void)
{
   nir_shader *nir = build_test_fs();
   if (!nir) {
      fprintf(stderr, "FAIL: nir shader construction failed\n");
      return 1;
   }

   char *usse = prismrv_nir_to_usse(NULL, nir);

   if (!usse) {
      fprintf(stderr, "FAIL: prismrv_nir_to_usse() returned NULL\n"
              "      (emitter rejected the shader — check for "
              "unsupported intrinsics in stderr above)\n");
      ralloc_free(nir);
      return 1;
   }

   printf("---- generated USSE ----\n%s------------------------\n", usse);

   int has_mul = strstr(usse, "vmul ") != NULL;
   int has_add = strstr(usse, "vmad ") != NULL ||
                 strstr(usse, "vadd ") != NULL ||
                 strstr(usse, "vmul ") != NULL; /* fmul+fadd may fuse */

   ralloc_free(usse);
   ralloc_free(nir);

   if (!has_mul) {
      fprintf(stderr, "FAIL: no fmul → vmul in USSE output\n");
      return 1;
   }
   if (!has_add) {
      fprintf(stderr, "FAIL: no fadd → vmad/vadd in USSE output\n");
      return 1;
   }

   printf("PRISMRV NIR→USSE TEST PASS\n");
   return 0;
}
