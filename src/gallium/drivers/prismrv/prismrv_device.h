/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * prismrv_device.h — SGX family device description and screen state.
 *
 * The driver is written for the whole PowerVR SGX 5xx series, not just
 * the SGX544: a prismrv_chip_info table describes each core type and a
 * runtime revision read from the kernel selects feature flags, mirroring
 * the kernel-side design in drivers/gpu/drm/prismrv.
 */
#ifndef PRISMRV_DEVICE_H_
#define PRISMRV_DEVICE_H_

#include <inttypes.h>
#include <stdint.h>

#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "pipe/p_context.h"
#include "pipe/p_state.h"

/* SGX core types (kernel prismrv_device.h PRISMRV_CORE_*) */
#define PRISMRV_CORE_SGX530   0x0130
#define PRISMRV_CORE_SGX531   0x0131
#define PRISMRV_CORE_SGX535   0x0135
#define PRISMRV_CORE_SGX540   0x0140
#define PRISMRV_CORE_SGX543   0x0143
#define PRISMRV_CORE_SGX544   0x0144
#define PRISMRV_CORE_SGX545   0x0145

/* kernel uapi: include/uapi/drm/prismrv_drm.h */
struct drm_prismrv_gem_create {
   uint64_t size;
   uint32_t flags;
   uint32_t handle;
};

struct drm_prismrv_gem_mmap_offset {
   uint32_t handle;
   uint32_t flags;
   uint64_t offset;
};

struct drm_prismrv_submit {
   uint32_t cmd_handle;
   uint32_t cmd_size;
   uint32_t num_in_fences;
   uint64_t in_fences;
   uint32_t num_bos;
   uint64_t bos;
   uint32_t out_fence_fd;
   uint32_t cmd_type;
};

struct drm_prismrv_get_param {
   uint32_t param;
   uint32_t pad;
   uint64_t value;
};

enum prismrv_cmd_type {
   PRISMRV_CMD_TA = 0,
   PRISMRV_CMD_TRANSFER,
   PRISMRV_CMD_2D,
   PRISMRV_CMD_POWER,
   PRISMRV_CMD_CONTEXTSUSPEND,
   PRISMRV_CMD_CLEANUP,
   PRISMRV_CMD_GETMISCINFO,
   PRISMRV_CMD_PROCESS_QUEUES,
   PRISMRV_CMD_DATABREAKPOINT,
   PRISMRV_CMD_SETHWPERFSTATUS,
   PRISMRV_CMD_COUNT,
};

#define PRISMRV_PARAM_GPU_ID         1
#define PRISMRV_PARAM_CORE_COUNT     2
#define PRISMRV_PARAM_UKERNEL_SIZE   3
#define PRISMRV_PARAM_ERRATA         4

/*
 * Per-core feature description.  Selected by the compatible string the
 * kernel bound against plus the RTL revision read at runtime; adding
 * another SGX variant means adding one entry here.
 */
struct prismrv_chip_info {
   const char *name;          /* "sgx544" ... */
   uint32_t    core_id;       /* PRISMRV_CORE_* */
   unsigned    num_cores;     /* MP cores */
   bool        has_isp2;              /* second ISP pipe */
   bool        has_multi_event_kick;  /* EVENT_KICK2-style kick */
   bool        has_pbe_mipmap;        /* PBE mipmap writeback */
   unsigned    max_rt_width;
   unsigned    max_rt_height;
};

extern const struct prismrv_chip_info prismrv_sgx544_info;

const struct prismrv_chip_info *
prismrv_core_lookup(uint32_t core_id);

struct prismrv_screen {
   struct pipe_screen base;
   int fd;

   const struct prismrv_chip_info *info;
   uint32_t core_revision;    /* raw EUR_CR_CORE_REVISION */

   /* cached kernel params */
   uint64_t errata_mask;
};

struct prismrv_resource {
   struct pipe_resource base;
   uint32_t gem_handle;
   void *cpu_map;
   uint64_t mmap_offset;
   uint32_t gpu_va;
};

static inline struct prismrv_screen *
to_prismrv_screen(struct pipe_screen *pscreen)
{
   return (struct prismrv_screen *)pscreen;
}

static inline struct prismrv_context *
to_prismrv_context(struct pipe_context *pctx)
{
   return (struct prismrv_context *)pctx;
}

static inline struct prismrv_resource *
to_prismrv_resource(struct pipe_resource *pres)
{
   return (struct prismrv_resource *)pres;
}

#endif /* PRISMRV_DEVICE_H_ */
