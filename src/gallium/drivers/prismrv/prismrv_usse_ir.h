/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * PrismRV USSE IR - the instruction level the NIR back end will target to
 * produce NATIVE SGX544 USSE code (not the textual PrismRV V1 programs).
 *
 * Operands are explicit about everything the hardware distinguishes: the
 * register BANK (r/o/pa/sa/internal), the register number, per-source
 * negate / absolute value and the swizzle.  Nothing here is a bare
 * "unsigned reg".
 *
 * Status: only the plain VMAD form is modelled, and only that form has been
 * verified against the vendor disassembler (see prismrv_usse_encode.c).
 */
#ifndef PRISMRV_USSE_IR_H_
#define PRISMRV_USSE_IR_H_

#include <stdbool.h>
#include <stdint.h>

enum prismrv_usse_bank {
   PRISMRV_USSE_BANK_R,    /* temporaries; even register numbers r0..r118 */
   PRISMRV_USSE_BANK_O,    /* output */
   PRISMRV_USSE_BANK_PA,   /* primary attributes */
   PRISMRV_USSE_BANK_SA,   /* secondary attributes */
   PRISMRV_USSE_BANK_I,    /* internal registers i0..i3 */
};

#define PRISMRV_USSE_FLAG_NOSCHED  (1u << 0)
#define PRISMRV_USSE_FLAG_SYNCS    (1u << 1)
#define PRISMRV_USSE_FLAG_SKIPINV  (1u << 2)
#define PRISMRV_USSE_FLAG_END      (1u << 3)   /* vmov only (verified); ends the program */

enum prismrv_usse_pred {
   PRISMRV_USSE_PRED_NONE,
   PRISMRV_USSE_PRED_P0,
   PRISMRV_USSE_PRED_NOT_P0,
};

#define PRISMRV_USSE_MASK_X  (1u << 0)
#define PRISMRV_USSE_MASK_Y  (1u << 1)
#define PRISMRV_USSE_MASK_Z  (1u << 2)   /* vec2 group / vmov only */
#define PRISMRV_USSE_MASK_W  (1u << 3)
#define PRISMRV_USSE_MASK_XYZW 0xfu

struct prismrv_usse_dst {
   uint8_t bank;     /* enum prismrv_usse_bank */
   uint8_t reg;
   uint8_t mask;     /* PRISMRV_USSE_MASK_*; the hardware offers only
                      * {all, x, y, xy} */
};

struct prismrv_usse_src {
   uint8_t bank;
   uint8_t reg;
   bool neg;
   bool abs;
   /*
    * Four component letters, e.g. "xyzw".  The hardware does NOT take an
    * arbitrary swizzle: per source slot there are exactly 8 presets (the
    * four broadcasts xxxx/yyyy/zzzz/wwww plus four fixed patterns that
    * differ per slot).  A swizzle outside the slot's presets has to be
    * produced by an extra move.
    */
   char swz[5];
};

/* dst = src[0] * src[1] + src[2]   (plain VMAD) */
struct prismrv_usse_vmad {
   uint8_t flags;            /* PRISMRV_USSE_FLAG_* */
   uint8_t pred;             /* enum prismrv_usse_pred */
   struct prismrv_usse_dst dst;
   struct prismrv_usse_src src[3];
};

/*
 * vec2 group: two-source vector ALU ops (opcode 00001).  Unlike VMAD,
 * sources can read r/pa/sa/o freely, src1 has a free per-component swizzle
 * ("xyzw" plus the literal constants '0' '1' '2' 'h' = 0.0 1.0 2.0 0.5),
 * src2 has 16 presets, and the destination mask is a full xyzw bitmask.
 * Negate exists on src1 only; abs on both.  (Verified, see
 * tools/usse_oracle/vec_model.py in the prismrv repository.)
 */
enum prismrv_usse_vec2_op {
   PRISMRV_USSE_VMUL,     /* dst = src1 * src2 */
   PRISMRV_USSE_VADD,     /* dst = src1 + src2 */
   PRISMRV_USSE_VMIN,
   PRISMRV_USSE_VMAX,
};

struct prismrv_usse_vec2 {
   uint8_t op;               /* enum prismrv_usse_vec2_op */
   uint8_t flags;            /* NOSCHED | SYNCS | SKIPINV */
   uint8_t pred;
   struct prismrv_usse_dst dst;     /* mask: any of X|Y|Z|W, not empty */
   struct prismrv_usse_src src[2];
};

/*
 * vmov: copy.  Register numbers are NOT doubled here (r0, r1, r2, ...).
 * Only the plain integer types are verified: .i32 is the bit-exact 32-bit
 * move used for register copies and constant/uniform forwarding.
 */
enum prismrv_usse_mov_type { PRISMRV_USSE_MOV_I8, PRISMRV_USSE_MOV_I16,
                             PRISMRV_USSE_MOV_I32 };

struct prismrv_usse_vmov {
   uint8_t type;             /* enum prismrv_usse_mov_type */
   uint8_t repeat;           /* 1..4 */
   uint8_t flags;            /* NOSCHED | SYNCS | SKIPINV | END */
   uint8_t pred;
   struct prismrv_usse_dst dst;
   struct prismrv_usse_src src;     /* swz: one of 16 presets; no neg/abs */
};

#endif /* PRISMRV_USSE_IR_H_ */
