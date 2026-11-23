/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>

#include "../prismrv_vendor_submit.h"
#include "../prismrv_vendor_kick.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fprintf(stderr, "\n"); fails++; } } while (0)

int
main(void)
{
   const uint32_t bos[] = { 7, 8, 9 };
   const int32_t fences[] = { 11 };
   struct prismrv_vk_submit_args a = { .ccb_handle = 3, .entry_offset = 2 * PRISMRV_VK_ENTRY_BYTES, .hwrc_va = 0xf0010000,
                                       .first_kick = true, .last_in_scene = true, .bo_handles = bos, .num_bos = 3,
                                       .in_fences = fences, .num_in_fences = 1 };
   struct drm_prismrv_submit_vendor s;

   CHECK(prismrv_vk_fill_submit(&s, &a), "fill rejected");
   CHECK(s.ccb_handle == 3 && s.ccb_offset == 2 * 0x540 + 0x50, "ccb_offset %#x", s.ccb_offset);
   CHECK(s.hwrc_va == 0xf0010000 && s.data0 == 0, "hwrc/data0");
   CHECK(s.flags == (PRISMRV_VKICK_FIRST | PRISMRV_VKICK_LAST_IN_SCENE), "flags %#x", s.flags);
   CHECK(s.num_bos == 3 && s.bos == (uintptr_t)bos && s.num_in_fences == 1 && s.in_fences == (uintptr_t)fences, "arrays");
   CHECK(s.pad0 == 0 && s.pad1 == 0 && s.out_fence_fd == -1, "pads/out fence");

   a.first_kick = false; a.last_in_scene = false;
   CHECK(prismrv_vk_fill_submit(&s, &a) && s.flags == 0, "flags 0");
   a.entry_offset = 2;
   CHECK(!prismrv_vk_fill_submit(&s, &a), "unaligned entry accepted");
   a.entry_offset = 0; a.bo_handles = NULL;
   CHECK(!prismrv_vk_fill_submit(&s, &a), "bos NULL with count accepted");

   printf("submit args: %d failures\n", fails);
   return fails ? 1 : 0;
}
