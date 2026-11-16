/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * Host test of the vendor context against a FAKE kernel: heap placement is honoured (and, in a second fake, ignored), BOs are
 * plain memory, submit records its arguments.  Every cross-reference of an assembled frame is checked against where the
 * objects actually landed.  This tests address plumbing only - not that the hardware accepts the result.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../prismrv_vendor_ctx.h"

struct fake_bo { uint32_t handle, va, size, heap; uint8_t *mem; };
struct fake {
   struct fake_bo bo[32];
   unsigned n;
   uint32_t next[11];               /* bump pointer per heap */
   bool honour_heap;
   unsigned submits;
   struct drm_prismrv_submit_vendor last;
   uint32_t last_bos[64];
   int submit_ret;
};

static const uint32_t heap_base[11] = { 0x10000000, 0xd0000000, 0, 0xf0000000, 0xf4000000, 0xfc000000, 0xe4000000, 0xec000000, 0xe0000000, 0, 0xc8000000 };

static uint32_t
f_create(void *u, uint64_t size, uint32_t heap, uint32_t *va)
{
   struct fake *f = u;
   uint32_t h = f->honour_heap ? heap : 0;     /* a kernel without v6 support ignores the heap */
   struct fake_bo *b = &f->bo[f->n];

   if (!f->next[h])
      f->next[h] = heap_base[h] + (h == 3 ? 0x51000 : 0);    /* the firmware's objects are already reserved in heap 3 */
   b->handle = ++f->n;
   b->va = f->next[h];
   b->size = size;
   b->heap = heap;
   b->mem = calloc(1, size);
   f->next[h] += (size + 0xfff) & ~0xfffu;
   *va = b->va;
   return b->handle;
}

static void *f_map(void *u, uint32_t handle, uint64_t size) { (void)size; return ((struct fake *)u)->bo[handle - 1].mem; }

static int
f_submit(void *u, const struct drm_prismrv_submit_vendor *a)
{
   struct fake *f = u;

   f->submits++;
   f->last = *a;
   memcpy(f->last_bos, (const void *)(uintptr_t)a->bos, a->num_bos * 4);
   return f->submit_ret;
}

/* read device memory through the fake BOs */
static const uint8_t *
dev(struct fake *f, uint32_t va)
{
   for (unsigned i = 0; i < f->n; i++)
      if (va >= f->bo[i].va && va < f->bo[i].va + f->bo[i].size)
         return f->bo[i].mem + (va - f->bo[i].va);
   return NULL;
}

static uint32_t rd(struct fake *f, uint32_t va) { const uint8_t *p = dev(f, va); uint32_t v = 0; if (p) memcpy(&v, p, 4); return v; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fprintf(stderr, "\n"); fails++; } } while (0)

static struct prismrv_vk_frame_in
make_in(void)
{
   static const uint32_t code[4] = { 0x11111111, 0x22222222, 0x33333333, 0x44444444 };
   static const uint32_t scissor[3] = { 0x3f800000, 0, 0 }, access[9] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
   struct prismrv_vk_frame_in in = {
      .gl = { .width = 64, .height = 64, .depth = true, .vx = 0, .vy = 0, .vw = 64, .vh = 64, .depth_far = 1.0, .depth_func = PRISMRV_VK_ALWAYS,
              .depth_write = true, .depth_test = -1, .color_mask = { 1, 1, 1, 1 } },
      .code = code, .code_size = sizeof(code),
      .attr_va = 0x10197000, .attr_dwords_m1 = 2, .attr_stride = 12, .vprog_ctl = 0x00020602, .n_attrs = 1,
      .clear_quad_va = 0x10160000, .index_va = 0x1019d000, .index_count = 3,
      .scissor_block = scissor, .n_scissor = 3, .access_block = access, .n_access = 9,
      .hw = { .hwpbdesc = 0xf0000000, .syncinfo = 0xf0048000, .rtdataset = 0xf004a000, .rtdata = 0xf004a064, .ta8 = 0xd000c000,
              .ta9 = 0xd0007000, .ta15 = 0x40700000 },
      .render_details_va = 0xf0047000, .hwrc_va = 0xf0010000,
   };
   return in;
}

int
main(void)
{
   struct fake f = { .honour_heap = true };
   struct prismrv_vk_ops ops = { .user = &f, .bo_create = f_create, .bo_map = f_map, .submit = f_submit };
   struct prismrv_vk_ctx c;
   const char *why = "";

   /* a kernel that ignores the heap must be refused, not used */
   {
      struct fake g = { .honour_heap = false };
      struct prismrv_vk_ops go = ops;
      struct prismrv_vk_ctx gc;

      go.user = &g;
      CHECK(prismrv_vk_ctx_init(&gc, &go, &why) == -EFAULT, "heap-ignoring kernel accepted (%s)", why);
   }

   CHECK(prismrv_vk_ctx_init(&c, &ops, &why) == 0, "init: %s", why);
   CHECK(c.arena[PRISMRV_VK_ARENA_CODE].va == 0xf4000000, "code arena at 0x%08x", c.arena[PRISMRV_VK_ARENA_CODE].va);
   CHECK(c.arena[PRISMRV_VK_ARENA_CCB].va == 0xf0051000, "ccb arena at 0x%08x (after the reserved firmware objects)", c.arena[PRISMRV_VK_ARENA_CCB].va);

   struct prismrv_vk_frame_in in = make_in();
   struct prismrv_vk_frame_result r;
   CHECK(prismrv_vk_ctx_frame(&c, &in, &r, &why) == 0, "frame: %s", why);

   /* the submit */
   CHECK(f.submits == 1, "submits %u", f.submits);
   CHECK(f.last.ccb_handle == c.arena[PRISMRV_VK_ARENA_CCB].handle, "ccb handle");
   CHECK(f.last.ccb_offset == 0x50, "ccb_offset 0x%x (slot 0 -> 0x50)", f.last.ccb_offset);
   CHECK(f.last.hwrc_va == 0xf0010000 && f.last.data0 == 0, "hwrc/data0");
   CHECK(f.last.flags == (PRISMRV_VKICK_FIRST | PRISMRV_VKICK_LAST_IN_SCENE), "first-kick flags 0x%x", f.last.flags);
   CHECK(f.last.num_bos == PRISMRV_VK_ARENA_COUNT - 1, "bo count %u", f.last.num_bos);

   /* code and its binding */
   CHECK(r.code_va >= 0xf4000000 && !(r.code_va & 7), "code va 0x%08x", r.code_va);
   CHECK(memcmp(dev(&f, r.code_va), in.code, in.code_size) == 0, "code bytes");
   CHECK(prismrv_vk_shader_descriptor_va(rd(&f, r.binding_va)) == r.code_va, "binding word 0x%08x does not decode to the code va", rd(&f, r.binding_va));
   /* the fragment binding is the 0x070001b5 object (not the process-constant 0x070001a5 one) and the ISP state list B points at it */
   CHECK(rd(&f, r.binding_va + 16) == 0x070001b5 && rd(&f, r.binding_va + 4) == 2, "fragment binding program words");
   CHECK(r.binding_va == (r.binding_va & ~0x1fu) && (r.binding_va & 0xf0000000) == 0xe0000000, "binding va 0x%08x", r.binding_va);

   /* the entry in the ring */
   {
      uint32_t entry[PRISMRV_VK_ENTRY_BYTES / 4];
      const uint8_t *e = dev(&f, c.arena[PRISMRV_VK_ARENA_CCB].va);
      struct prismrv_vk_state gl = in.gl;
      gl.pds_bg_va = r.bg_va; gl.pds_bg2_va = r.bg2_va; gl.pds_fs_va = r.binding_va; gl.pds_arena2_va = r.arena2_va;
      struct prismrv_vk_kick k;

      gl.stream_va = r.stream_va;
      gl.render_details_va = in.render_details_va;
      prismrv_vk_emit(&gl, &k);
      prismrv_vk_build_entry(&k, &in.hw, entry);
      CHECK(memcmp(e, entry, sizeof(entry)) == 0, "CCB entry differs from the builder output");
      CHECK(rd(&f, c.arena[PRISMRV_VK_ARENA_CCB].va + PRISMRV_VK_TA_BLOCK_OFF + 6 * 4) == r.stream_va, "TA block dw6 is not the stream va");
   }

   /* the stream: every state word's low 28 bits point at an object in the vertex PDS heap holding what the builders produce */
   {
      unsigned n = r.stream_dwords, states = 0;
      CHECK(n == 29, "stream dwords %u", n);
      for (unsigned i = 0; i + 1 < n;) {
         uint32_t w0 = rd(&f, r.stream_va + 4 * i);
         if (w0 == 0xc0000000) break;
         if ((w0 >> 28) == 8) { i += 4; continue; }
         uint32_t va = (w0 & 0x0fffffff) << 4;
         CHECK(va >= 0xec000000 && va < 0xec000000 + 0x8000, "state word %u points at 0x%08x outside the vertex PDS arena", i, va);
         CHECK(dev(&f, va) != NULL, "state word %u points at unmapped 0x%08x", i, va);
         states++;
         i += 2;
      }
      CHECK(states == 10, "state records %u", states);
      /* record by record: each state word must point at the object of ITS kind, holding the right block / fields */
      uint32_t rec_va[10], rec_task[10], rec_cls[10];
      unsigned nr = 0;
      for (unsigned i = 0; i + 1 < n && nr < 10;) {
         uint32_t w0 = rd(&f, r.stream_va + 4 * i);
         if (w0 == 0xc0000000) break;
         if ((w0 >> 28) == 8) { i += 4; continue; }
         rec_va[nr] = (w0 & 0x0fffffff) << 4;
         rec_cls[nr] = w0 >> 28;
         rec_task[nr] = rd(&f, r.stream_va + 4 * i + 4);
         nr++; i += 2;
      }
      struct prismrv_vk_state gl = in.gl;
      gl.pds_bg_va = r.bg_va; gl.pds_bg2_va = r.bg2_va; gl.pds_fs_va = r.binding_va; gl.pds_arena2_va = r.arena2_va;
      struct prismrv_vk_kick k;
      gl.stream_va = r.stream_va;
      gl.render_details_va = in.render_details_va;
      prismrv_vk_emit(&gl, &k);

      /* [0] state list A: object -> block == emitter list A */
      CHECK(rec_task[0] == 0x1a022203 && rd(&f, rec_va[0] + 4) == k.n_a, "record 0 is not state list A");
      CHECK(memcmp(dev(&f, rd(&f, rec_va[0])), k.list_a, k.n_a * 4) == 0, "state list A block");
      /* [1] scissor: class 7, object -> block == the caller's scissor words */
      CHECK(rec_cls[1] == 7 && rec_task[1] == 0x05800202, "record 1 is not the scissor record");
      CHECK(memcmp(dev(&f, rd(&f, rec_va[1])), in.scissor_block, in.n_scissor * 4) == 0, "record 1 does not lead to the scissor block");
      /* [2] state list B */
      CHECK(rec_task[2] == 0x1a022205 && rd(&f, rec_va[2] + 4) == k.n_b, "record 2 is not state list B");
      CHECK(memcmp(dev(&f, rd(&f, rec_va[2])), k.list_b, k.n_b * 4) == 0, "state list B block");
      /* [3] access */
      CHECK(rec_task[3] == 0x1000e103, "record 3 is not the access record");
      CHECK(memcmp(dev(&f, rd(&f, rec_va[3])), in.access_block, in.n_access * 4) == 0, "record 3 does not lead to the access block");
      /* [4] vertex program: class f, object dw0 = the attribute VA */
      CHECK(rec_cls[4] == 0xf && rd(&f, rec_va[4]) == in.attr_va && rd(&f, rec_va[4] + 16) == in.vprog_ctl, "record 4 is not the vertex program");
      /* [5..9] epilogues: header-less, three sharing one pair object, and the fragment-output object */
      CHECK(rec_task[5] == 0x0800e100 && rd(&f, rec_va[5]) == 0x0002ffe2, "epilogue 0");
      CHECK(rec_va[6] == rec_va[7] && rec_va[7] == rec_va[8] && rd(&f, rec_va[6]) == 0x800, "pair epilogues do not share one object");
      CHECK(rec_cls[9] == 6 && rec_task[9] == 0x1a022201 && rd(&f, rec_va[9]) == 0x2000, "fragment-output epilogue");
   }

   CHECK(r.unwired == (PRISMRV_VK_UNWIRED_BACKGROUND_PROGRAM | PRISMRV_VK_UNWIRED_HW_OBJECT_CONTENTS), "unwired flags 0x%x", r.unwired);

   /* second frame: next ring slot, no FIRST flag, fresh allocations */
   {
      struct prismrv_vk_frame_result r2;
      CHECK(prismrv_vk_ctx_frame(&c, &in, &r2, &why) == 0, "frame 2: %s", why);
      CHECK(f.last.ccb_offset == PRISMRV_VK_ENTRY_BYTES + 0x50, "slot 1 offset 0x%x", f.last.ccb_offset);
      CHECK(f.last.flags == PRISMRV_VKICK_LAST_IN_SCENE, "second kick flags 0x%x", f.last.flags);
      CHECK(r2.code_va == r.code_va + in.code_size, "second code lands after the first (0x%08x)", r2.code_va);
   }

   /* refusals */
   {
      struct prismrv_vk_frame_result rr;
      struct prismrv_vk_frame_in bad;

      bad = in; bad.n_attrs = 3;                CHECK(prismrv_vk_ctx_frame(&c, &bad, &rr, &why) == -ENOTSUP, "3 attributes accepted");
      bad = in; bad.n_attrs = 2; bad.attr1_va = in.attr_va + 12; bad.attr1_dwords_m1 = 3; bad.attr1_stride = 28;
      CHECK(prismrv_vk_ctx_frame(&c, &bad, &rr, &why) == 0, "2 attributes refused: %s", why ? why : "?");
      bad = in; bad.code_size = 12;             CHECK(prismrv_vk_ctx_frame(&c, &bad, &rr, &why) == -EINVAL, "code size 12 accepted");
      bad = in; bad.code = NULL;                CHECK(prismrv_vk_ctx_frame(&c, &bad, &rr, &why) == -EINVAL, "no code accepted");
      /* stencil test + partial viewport -> state list B of 20 dwords: never captured, accepted through the fitted control word
       * rule and flagged */
      bad = in; bad.gl.stencil = true; bad.gl.stencil_test = true; bad.gl.vw = 32;
      CHECK(prismrv_vk_ctx_frame(&c, &bad, &rr, &why) == 0 && (rr.unwired & PRISMRV_VK_UNWIRED_EXTRAPOLATED_LIST_B),
            "20-dword list B not accepted as extrapolated (unwired 0x%x)", rr.unwired);
      f.submit_ret = -EIO;
      CHECK(prismrv_vk_ctx_frame(&c, &in, &rr, &why) == -EIO, "submit failure not propagated");
      f.submit_ret = 0;
      /* arenas are finite: keep going until one is full */
      int ret = 0;
      for (unsigned i = 0; i < 400 && !ret; i++)
         ret = prismrv_vk_ctx_frame(&c, &in, &rr, &why);
      CHECK(ret == -ENOSPC, "arena exhaustion gave %d", ret);
   }

   printf("vendor ctx: %d failures\n", fails);
   return fails ? 1 : 0;
}
