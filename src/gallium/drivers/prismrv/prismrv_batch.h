/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 */
#ifndef PRISMRV_BATCH_H_
#define PRISMRV_BATCH_H_

#include "prismrv_device.h"

int prismrv_batch_submit(struct pipe_context *pctx,
                         enum prismrv_cmd_type type,
                         uint32_t cmd_bo, uint32_t cmd_size,
                         const uint32_t *bos, uint32_t num_bos);

#endif /* PRISMRV_BATCH_H_ */
