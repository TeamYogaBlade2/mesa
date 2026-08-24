/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 */
#ifndef PRISMRV_RESOURCE_H_
#define PRISMRV_RESOURCE_H_

struct prismrv_screen;

void prismrv_resource_screen_init(struct prismrv_screen *screen);
void prismrv_resource_screen_fini(struct prismrv_screen *screen);

#endif /* PRISMRV_RESOURCE_H_ */
/* include prismrv_device.h for struct prismrv_resource definition */
#include "prismrv_device.h"
