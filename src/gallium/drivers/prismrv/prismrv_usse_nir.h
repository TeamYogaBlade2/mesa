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
 *   - Register model (dense).  Vendor code uses whole vec4 operands (mask
 *     xyzw, swizzle xyzw: component i of the vector at register n is
 *     register n+i) and scalar forms like "pa10.x, pa8.yyyy".  A scalar in
 *     register k is therefore emitted as component (k&1) of the pair at
 *     register (k&~1); vmov addresses registers 1:1.             [UNVERIFIED
 *     semantics, consistent with all vendor code seen]
 *   - r   : temporaries, r0..r118 even only.
 *   - pa  : inputs: VS attribute a / FS varying v, component c -> pa[4a+c]
 *           (vendor: varying v_color is pa0..pa3)
 *   - sa  : uniforms from sa10 (sa0..sa9 belong to the driver), one 32-bit
 *           register each, then a deduplicated literal pool.  Vendor mediump
 *           code uses 4 x f16 packed in two sa registers instead (f32 here).
 *   - o   : VS outputs o[c] / o[4+c]  [UNVERIFIED].  FS colour: packed to
 *           RGBA8 into o0 with vpcku8f32 (src1 = x,y regs, src2 = z,w regs:
 *           the split is an ASSUMPTION, only the encoding is verified),
 *           wrapped in the two phas words and a trailing nop.end like the
 *           vendor's single-phase pixel program.
 *   - Precision.  Vendor mediump FS code is f16 (vf16mad ...), highp FS does
 *           not compile; this back end is f32 only (right for the VS).
 *   - VS: wrapped as phas, code, emitvtx.end.freep (as in the vendor's vertex
 *     program, whose preceding phase that defaults attribute w to 1.0 is NOT
 *     emitted: it depends on the attribute format, i.e. on driver state).
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
#define PRISMRV_USSE_SA_SLOTS      64    /* sa0..sa63: vmov addresses 1:1 up to 63 */
#define PRISMRV_USSE_R_SLOTS       60    /* r0..r59: vmov's r field ends at 59 */
#define PRISMRV_USSE_SA_UNIFORM0   10    /* vendor code: first uniform at sa10 */

/* scalar register numbers (dense model, see the ABI notes above) */
#define PRISMRV_USSE_PA_REG(vec4, c)   (4u * (vec4) + (c))
#define PRISMRV_USSE_O_POS_REG(c)      (c)
#define PRISMRV_USSE_O_VARY_REG(c)     (4u + (c))

/* instruction words the FS program is wrapped in (single-phase pixel
 * program, as the vendor driver emits it); see prismrv_usse_nir.c */
#define PRISMRV_USSE_PHAS_END_PIXEL    UINT64_C(0xfa4c070000000000) /* phas.end #0, #0, pixel, end, parallel */
#define PRISMRV_USSE_PHAS_PIXEL        UINT64_C(0xfa44070000000000) /* phas #0, #0, pixel, end, parallel */
#define PRISMRV_USSE_NOP_END           UINT64_C(0xf804014000000000) /* nop.end */
#define PRISMRV_USSE_EMITVTX_END       UINT64_C(0xfb275000a0200000) /* emitvtx.end.freep #0, #0: ends a vertex program */

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
