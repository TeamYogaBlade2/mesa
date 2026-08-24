/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 */
#ifndef PRISMRV_PROGRAM_H_
#define PRISMRV_PROGRAM_H_

#include "compiler/nir/nir.h"

/* NIR → USSE text (ralloc'd off memctx).  The text is consumed either by
 * the emulator's usse_emu parser or by the kernel-side batch builder. */
char *prismrv_nir_to_usse(void *memctx, nir_shader *nir);

#endif /* PRISMRV_PROGRAM_H_ */
