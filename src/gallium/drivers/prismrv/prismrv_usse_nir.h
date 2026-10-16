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
 * Native ABI: what is known and what is not.  Evidence comes from running
 * the vendor GLES driver and disassembling the code it generates (prismrv
 * repo, analysis/NATIVE_USSE_COMPILER.md, "Running the vendor GLES driver").
 * All of it is concentrated in the PRISMRV_USSE_*_REG() macros / the sa
 * layout so it can be corrected in one place.
 *
 *   - Register model.  Vendor code uses whole vec4 operands (mask xyzw,
 *     swizzle xyzw: component i of the vector at register n is register
 *     n+i) and scalar forms like "pa10.x, pa8.yyyy".  This compiler places
 *     every scalar in the x component of an even register ("pair model"):
 *     valid under that model, but it wastes the odd registers.   [WASTEFUL]
 *   - r   : temporaries, r0..r118 even only.
 *   - pa  : inputs: VS attribute a / FS varying v, component c ->
 *           pa[2*(4a+c)]  (vendor: varying v_color is pa0..pa3, dense) [WRONG
 *           SPACING, same cause as above]
 *   - sa  : uniforms + literals as 32-bit slots; slot s = sa[2*s]:
 *             slot 0 = 1.0f, slot 1 = 0.0f, slots 2.. uniforms, then the
 *           literal pool.  Vendor: uniforms ARE in sa, but the first one is
 *           sa10 (sa0..sa9 are the driver's) and a mediump vec4 is 4 x f16
 *           packed in two sa registers.                         [LAYOUT WRONG]
 *   - o   : VS outputs o[2*c] / o[2*(4+c)]  [UNVERIFIED].  FS colour: the
 *           vendor packs to RGBA8 with vpcku8f16/f32 into ONE register o0
 *           (inside a 'phas' phase structure); 4 float outputs, as emitted
 *           here for the FS, are NOT what an RGBA8 target consumes. [WRONG]
 *   - Precision.  Vendor mediump FS code is f16 (vf16mad ...), highp FS does
 *           not compile; this back end is f32 only (right for the VS).
 *   - No 'end' flag / phase header is emitted.
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
