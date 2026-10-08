/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 */
#ifndef PRISMRV_USSE_ENCODE_H_
#define PRISMRV_USSE_ENCODE_H_

#include "prismrv_usse_ir.h"

/*
 * Encode a plain VMAD into its 64-bit SGX544 USSE instruction word.
 * Returns false (and leaves *out untouched) if the instruction is not
 * representable: bad register number for the bank, a swizzle that is not
 * one of the slot's presets, negate on source 1, a bank the slot cannot
 * address, ...
 */
bool prismrv_usse_encode_vmad(const struct prismrv_usse_vmad *in,
                              uint64_t *out);

/*
 * Decode a word.  Returns false for anything outside the verified scope
 * (other opcodes, indexed sources, unknown bits set).
 */
bool prismrv_usse_decode_vmad(uint64_t word, struct prismrv_usse_vmad *out);

/* index (0..7) of @swz among source slot @slot's (1..3) presets, or -1 */
int prismrv_usse_swizzle_preset(unsigned slot, const char swz[4]);

#endif /* PRISMRV_USSE_ENCODE_H_ */
