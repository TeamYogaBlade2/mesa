/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * prismrv_test_nir.c — runnable NIR → USSE test (no meson needed).
 *
 * Builds a trivial fragment shader with the nir_builder:
 *     gl_FragColor = v_color.xyzw * vec4(1.0, 0.9, 0.8, 1.0) + v_color;
 * runs the PrismRV NIR→USSE emitter and checks that the expected
 * vmul/vmad instruction text is present.
 *
 * Build (from the mesa root):
 *   clang -std=c11 -DHAVE_ENDIAN_H -DHAVE_PTHREAD -DHAVE_STRUCT_TIMESPEC \
 *     -D_GNU_SOURCE -Isrc/compiler/nir/generated -Isrc -Isrc/compiler/nir \
 *     -Isrc/compiler -Isrc/util -Iinclude \
 *     src/gallium/drivers/prismrv/prismrv_test_nir.c \
 *     src/gallium/drivers/prismrv/prismrv_program.c \
 *     src/compiler/nir/nir.c src/compiler/nir/nir_builtin_ops.c \
 *     src/compiler/nir/nir_vla.c ... -o /tmp/prismrv_nir_test
 */
#include <stdio.h>
#include <string.h>

#include "util/ralloc.h"

#include "compiler/nir/nir_builder.h"
#include "prismrv_program.h"

static nir_shader *
build_test_fs(void)
{
   const struct glsl_type *vec4 =
      glsl_vector_type(GLSL_TYPE_FLOAT, 4);

   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, NULL, "prismrv-test-fs");

   nir_variable *v_color = nir_variable_create(
      b.shader, nir_var_shader_in, vec4, "v_color");
   v_color->data.location = VARYING_SLOT_COL0;

   nir_variable *out = nir_variable_create(
      b.shader, nir_var_shader_out, vec4, "gl_FragColor");
   out->data.location = FRAG_RESULT_COLOR;

   nir_def *v = nir_load_var(&b, v_color);
   nir_def *scaled = nir_fmul(&b, v, nir_imm_vec4(&b, 1.0f, 0.9f, 0.8f, 1.0f));
   nir_def *sum = nir_fadd(&b, scaled, v);

   nir_store_var(&b, out, sum, 0xf);
   return b.shader;
}

int
main(void)
{
   nir_shader *nir = build_test_fs();
   char *usse = prismrv_nir_to_usse(NULL, nir);

   printf("---- generated USSE ----\n%s------------------------\n", usse);

   int has_mul = strstr(usse, "vmul ") != NULL;
   int has_mad = strstr(usse, "vmad ") != NULL;

   if (!has_mul) {
      fprintf(stderr, "FAIL: no fmul → vmul in output\n");
      return 1;
   }
   if (!has_mad) {
      fprintf(stderr, "FAIL: no fadd → vmad in output\n");
      return 1;
   }
   printf("PRISMRV NIR→USSE TEST PASS\n");
   ralloc_free(nir);
   return 0;
}
