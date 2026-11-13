/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * The PDS-heap objects the TA stream's state words point at.  Layout (analysis/agents/descriptor-object.md in the prismrv
 * repository, measured on 1166 captured objects):
 *
 *     [header: 0/4/8/12 dwords][control word][3-dword slot][PDS code ... af000000]      code starts at control word + 0x10
 *
 * Words whose cause is not understood are taken from the caller as `var` arguments and are marked so below; everything else
 * is a constant template.  Each builder returns the length in dwords or 0 if the arguments cannot be represented.
 * Every builder is tested against real captured objects (tests/prismrv_vendor_desc_test.c).
 *
 * NOT covered: vertex programs with 2 or 3 attributes, the stencil/other state-list counts that were never captured
 * (13..16 dwords), and the layout of the arena the objects live in.
 */
#ifndef PRISMRV_VENDOR_DESC_H
#define PRISMRV_VENDOR_DESC_H

#include <stdint.h>

#define PRISMRV_VK_DESC_STATE_LIST_DWORDS   20
#define PRISMRV_VK_DESC_SCISSOR_DWORDS      12
#define PRISMRV_VK_DESC_ACCESS_DWORDS       11
#define PRISMRV_VK_DESC_EPI_PAIR_DWORDS     16
#define PRISMRV_VK_DESC_EPI_OUT_DWORDS      16
#define PRISMRV_VK_DESC_EPI_HDRLESS_DWORDS  6
#define PRISMRV_VK_DESC_VPROG1_DWORDS       12

/* state list; task = 0x1a022203/04/05/07, count = the state block's dword count.  The control word comes from the measured
 * (task, count) table: 03:{8,11} 04:{12} 05:{17,18} 07:{17,19}; any other pair is refused. */
unsigned prismrv_vk_desc_state_list(uint32_t task, uint32_t block_va, uint32_t count, uint32_t *out);

/* scissor object (task 0x05800202, class 7): only the block VA moves */
unsigned prismrv_vk_desc_scissor(uint32_t block_va, uint32_t *out);

/* "access" object (task 0x1000e103); var[0..4] = dwords 3..7 (zero in most captures, otherwise a neighbouring object's words) */
unsigned prismrv_vk_desc_access(uint32_t block_va, const uint32_t var[5], uint32_t *out);

/* epilogue objects shared by tasks 0x1a024201/26201/28201; var[0..2] = dwords 3, 6, 7 (neighbour-dependent) */
unsigned prismrv_vk_desc_epilogue_pair(const uint32_t var[3], uint32_t *out);

/* fragment-output epilogue (task 0x1a022201, class 6); var[0..3] = dwords 3, 4, 7, 10 (cause not understood) */
unsigned prismrv_vk_desc_epilogue_out(const uint32_t var[4], uint32_t *out);

/* header-less epilogue (task 0x0800e100): control word at offset 0; dw3 not understood */
unsigned prismrv_vk_desc_epilogue_hdrless(uint32_t dw3, uint32_t *out);

/* vertex program with ONE attribute: [attr VA][dwords-1][stride][0] ctl; ctl = 0x00020602 (float) / 0x00020622 (16-bit) */
unsigned prismrv_vk_desc_vprog1(uint32_t attr_va, uint32_t dwords_m1, uint32_t stride, uint32_t ctl, uint32_t *out);

#endif
