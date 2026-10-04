/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 */
#ifndef PRISMRV_PROGRAM_H_
#define PRISMRV_PROGRAM_H_

#include "compiler/nir/nir.h"

/* Upper bound on the generated USSE text (keeps one draw's command
 * stream well below the command BO capacity). */
#define PRISMRV_MAX_USSE_TEXT 16384

/*
 * Register ABI shared with the PrismRV executor (ta_stage.py, shaderc.py,
 * libprismrv.py):
 *
 *   r0..r15    VS attributes: element i occupies r[4*i .. 4*i+3]
 *   r16..r31   uniforms of the stage being run (vec4 i at r[16+4*i]);
 *              VS and FS each have their own block (SET_UNIFORMS carries
 *              a stage word), so both start at r16
 *   r32..r35   FS: the single interpolated varying, all four components
 *   r60..r62   constants 0.0 / 1.0 / -1.0 (materialised by the program)
 *   r64..r255  temporaries (linear allocation, no reuse)
 *   o0..o3     VS clip position / FS colour
 *   o4..o7     VS varying (rgba): the executor transports four floats
 */
#define PRISMRV_MAX_VS_ATTRIBS   4
#define PRISMRV_MAX_UNIFORM_VEC4 4

/* NIR compiler options for both stages. */
const nir_shader_compiler_options *prismrv_get_nir_options(void);

/*
 * Lower and optimise a private clone of @nir for the backend, then emit
 * USSE text (ralloc'd off @memctx).  @nir itself is not modified.  Returns NULL if the shader uses anything the
 * backend cannot express: unsupported constructs are never silently
 * dropped or miscompiled.
 */
char *prismrv_nir_to_usse(void *memctx, nir_shader *nir);

#endif /* PRISMRV_PROGRAM_H_ */
