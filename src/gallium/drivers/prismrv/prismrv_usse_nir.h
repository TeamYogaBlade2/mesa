/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * NIR -> NATIVE SGX544 USSE machine code (64-bit instruction words).
 *
 * This is the real-hardware path of the compiler.  It does not produce the
 * PrismRV V1 text programs of prismrv_program.c: the output is a vector of
 * 64-bit words that the stock vendor uKernel can run.  The V1 path stays
 * the emulator bring-up path.
 *
 * Instruction selection is limited to what prismrv_usse_encode.c can emit
 * and what has been verified against the vendor disassembler: plain VMAD,
 * the two-source vector group (vmul/vadd/vmin/vmax) and vmov.  Everything
 * else (control flow, texturing, rcp/rsq, compares, ...)
 * makes the compile FAIL; nothing is approximated.  As further forms are
 * modelled (vadd/vmul/vmov/vrcp/indexed/...), they are added here.
 *
 * ---------------------------------------------------------------------------
 * Native ABI assumptions (see analysis/NATIVE_USSE_COMPILER.md in the
 * prismrv repo; items marked UNVERIFIED have no hardware confirmation yet and
 * are concentrated in the PRISMRV_USSE_*_REG() macros below so they can be
 * corrected in ONE place):
 *
 *   - The encoder's register numbers are even (field * 2).  Every scalar
 *     value therefore occupies the x component of an even register
 *     ("pair" model) and is read with the broadcast swizzle xxxx and written
 *     with dst mask x.                                         [UNVERIFIED]
 *   - r   : temporaries (this compiler allocates r0..r118, even only)
 *   - pa  : shader inputs: VS attribute a, component c -> pa[2*(4a+c)];
 *           FS varying v, component c -> pa[2*(4v+c)]           [UNVERIFIED]
 *   - sa  : constants + uniforms, as 32-bit slots, slot s = sa[2*s]:
 *             slot 0 = 1.0f, slot 1 = 0.0f,
 *             slots 2 .. 2+U-1       = uniforms (byte offset / 4)
 *             slots 2+U ..           = literal pool (deduplicated)
 *           The values of the pool are returned in sa_image[]; uniforms
 *           are written by the driver at draw time.             [UNVERIFIED]
 *   - o   : outputs: VS position comp c -> o[2*c], varying comp c ->
 *           o[2*(4+c)]; FS colour comp c -> o[2*c]              [UNVERIFIED]
 *
 * Hardware constraints that ARE verified and enforced here: source 1 of a
 * VMAD can only read r/pa (vadd/vmul/vmov read any bank); negate exists on
 * VMAD sources 2/3 and vadd/vmul source 1 only; swizzles are per-slot
 * presets (vadd/vmul source 1: free per component); VMAD write masks are
 * {x, y, xy, all}, vec2/vmov masks are full xyzw bitmasks.
 * ---------------------------------------------------------------------------
 */
#ifndef PRISMRV_USSE_NIR_H_
#define PRISMRV_USSE_NIR_H_

#include <stdbool.h>
#include <stdint.h>

#include "compiler/nir/nir.h"

#define PRISMRV_USSE_MAX_WORDS     512
#define PRISMRV_USSE_SA_SLOTS      64    /* sa[0..126], even registers */
#define PRISMRV_USSE_R_SLOTS       60    /* r0..r118, even; 120+ = i0..i3 */
#define PRISMRV_USSE_SA_ONE        0
#define PRISMRV_USSE_SA_ZERO       1
#define PRISMRV_USSE_SA_UNIFORM0   2

/* register number of scalar slot @s (the encoder wants even numbers) */
#define PRISMRV_USSE_SLOT_REG(s)       (2u * (s))
#define PRISMRV_USSE_PA_REG(vec4, c)   PRISMRV_USSE_SLOT_REG(4u * (vec4) + (c))
#define PRISMRV_USSE_O_POS_REG(c)      PRISMRV_USSE_SLOT_REG(c)
#define PRISMRV_USSE_O_VARY_REG(c)     PRISMRV_USSE_SLOT_REG(4u + (c))
#define PRISMRV_USSE_O_COLOR_REG(c)    PRISMRV_USSE_SLOT_REG(c)

struct prismrv_usse_binary {
   mesa_shader_stage stage;

   uint64_t code[PRISMRV_USSE_MAX_WORDS];
   unsigned num_words;

   unsigned num_r_slots;            /* temporaries used */

   /* secondary-attribute image: constants filled in, uniform slots zero */
   uint32_t sa_image[PRISMRV_USSE_SA_SLOTS];
   unsigned sa_uniform_first;       /* slot index */
   unsigned sa_uniform_count;       /* 32-bit uniform slots */
   unsigned sa_pool_first;
   unsigned sa_pool_count;
};

/*
 * Compile @nir (not modified; a private clone is lowered) to native USSE.
 * Returns false and prints the reason to stderr if the shader uses
 * anything the verified encoder cannot express.
 */
bool prismrv_nir_to_usse_native(nir_shader *nir,
                                struct prismrv_usse_binary *out);

/* one-line disassembly of a native word (re-decoded through the encoders'
 * decoders: vmad, vec2 group, vmov; anything else prints as .word) */
void prismrv_usse_disasm(uint64_t word, char *buf, unsigned size);

#endif /* PRISMRV_USSE_NIR_H_ */
