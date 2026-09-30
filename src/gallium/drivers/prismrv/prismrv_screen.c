/*
 * Copyright 2026 PrismRV project
 * SPDX-License-Identifier: MIT
 *
 * prismrv_screen.c — pipe_screen implementation.
 */
#include "prismrv_screen.h"

#include <fcntl.h>
#include <unistd.h>
#include <inttypes.h>

#include "util/ralloc.h"
#include "util/u_debug.h"
#include "util/u_memory.h"
#include "util/u_screen.h"

#include "prismrv_chipinfo.h"
#include "prismrv_context.h"
#include "prismrv_drmif.h"
#include "prismrv_resource.h"
#include "prismrv_fence.h"
#include "prismrv_program.h"

static const char *
prismrv_screen_get_name(struct pipe_screen *pscreen)
{
   struct prismrv_screen *screen = to_prismrv_screen(pscreen);
   return screen->info->name;
}

static const char *
prismrv_screen_get_vendor(struct pipe_screen *pscreen)
{
   return "PrismRV";
}

static const char *
prismrv_screen_get_device_vendor(struct pipe_screen *pscreen)
{
   return "Imagination Technologies";
}

static void
prismrv_screen_destroy(struct pipe_screen *pscreen)
{
   struct prismrv_screen *screen = to_prismrv_screen(pscreen);

   prismrv_resource_screen_fini(screen);
   close(screen->fd);
   ralloc_free(screen);
}

static struct pipe_context *
prismrv_screen_context_create(struct pipe_screen *pscreen, void *priv,
                              unsigned flags)
{
   return prismrv_context_create(pscreen, priv, flags);
}

static bool
prismrv_screen_is_format_supported(struct pipe_screen *pscreen,
                                   enum pipe_format format,
                                   enum pipe_texture_target target,
                                   unsigned sample_count,
                                   unsigned storage_sample_count,
                                   unsigned usage)
{
   /*
    * Only targets that resource_create() can actually allocate.
    * Advertising support for cube maps, 2D arrays, 3D textures etc.
    * here while resource_create() returns NULL for them causes
    * silent errors in the state tracker.
    */
   switch (target) {
   case PIPE_TEXTURE_2D:
   case PIPE_TEXTURE_RECT:
   case PIPE_BUFFER:
      break;
   default:
      return false;
   }

   /* No MSAA */
   if (sample_count > 1 || storage_sample_count > 1)
      return false;

   /*
    * Allow-list of bind flags this driver really implements.  Anything
    * else (depth/stencil, shader buffers/images, streamout, ...) must be
    * refused, otherwise the state tracker believes it can use them.
    */
   const unsigned supported_binds = PIPE_BIND_RENDER_TARGET |
      PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_VERTEX_BUFFER |
      PIPE_BIND_INDEX_BUFFER | PIPE_BIND_CONSTANT_BUFFER |
      PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_SCANOUT | PIPE_BIND_SHARED |
      PIPE_BIND_CUSTOM;
   if (usage & ~supported_binds)
      return false;

   if (target == PIPE_BUFFER) {
      /* plain data: vertex fetch formats only, no RT/sampler use */
      if (usage & (PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW |
                   PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_SCANOUT))
         return false;
      if (usage & PIPE_BIND_VERTEX_BUFFER) {
         switch (format) {
         case PIPE_FORMAT_R32_FLOAT:
         case PIPE_FORMAT_R32G32_FLOAT:
         case PIPE_FORMAT_R32G32B32_FLOAT:
         case PIPE_FORMAT_R32G32B32A32_FLOAT:
         case PIPE_FORMAT_R8G8B8A8_UNORM:
         case PIPE_FORMAT_R8G8B8_UNORM:
            break;
         default:
            return false;
         }
      }
      return true;
   }

   if (usage & (PIPE_BIND_RENDER_TARGET | PIPE_BIND_DISPLAY_TARGET |
                PIPE_BIND_SCANOUT)) {
      switch (format) {
      case PIPE_FORMAT_B8G8R8A8_UNORM:
      case PIPE_FORMAT_R8G8B8A8_UNORM:
         break;
      default:
         return false;
      }
   }
   if (usage & PIPE_BIND_SAMPLER_VIEW) {
      /* the executor reads 4 x f32 per texel; the sampler-view path
       * converts these 8-bit formats on upload */
      switch (format) {
      case PIPE_FORMAT_B8G8R8A8_UNORM:
      case PIPE_FORMAT_R8G8B8A8_UNORM:
         break;
      default:
         return false;
      }
   }
   /* vertex/index/constant buffers are buffer-only */
   if (usage & (PIPE_BIND_VERTEX_BUFFER | PIPE_BIND_INDEX_BUFFER |
                PIPE_BIND_CONSTANT_BUFFER))
      return false;

   return true;
}

static void
prismrv_init_shader_caps(struct pipe_screen *screen)
{
   /* Limits of the NIR backend (prismrv_program.c): straight-line code,
    * 4 vertex attributes, 4 vec4 uniforms, one varying, 8 textures. */
   struct pipe_shader_caps *caps =
      (struct pipe_shader_caps *)&screen->shader_caps[MESA_SHADER_VERTEX];

   caps->max_instructions =
   caps->max_alu_instructions = 256;
   caps->max_control_flow_depth = 0;
   caps->max_inputs = PRISMRV_MAX_VS_ATTRIBS;
   caps->max_outputs = 2;               /* position + one varying */
   caps->max_const_buffer0_size = PRISMRV_MAX_UNIFORM_VEC4 * 16;
   caps->max_const_buffers = 1;
   caps->max_temps = 128;
   caps->supported_irs = 1u << PIPE_SHADER_IR_NIR;

   caps = (struct pipe_shader_caps *)
      &screen->shader_caps[MESA_SHADER_FRAGMENT];
   caps->max_instructions =
   caps->max_alu_instructions = 256;
   caps->max_tex_instructions =
   caps->max_tex_indirections = 8;
   caps->max_control_flow_depth = 0;
   caps->max_inputs = 1;                /* one varying */
   caps->max_outputs = 1;
   caps->max_const_buffer0_size = PRISMRV_MAX_UNIFORM_VEC4 * 16;
   caps->max_const_buffers = 1;
   caps->max_temps = 128;
   caps->max_texture_samplers =
   caps->max_sampler_views = 8;
   caps->supported_irs = 1u << PIPE_SHADER_IR_NIR;
}

struct pipe_screen *
prismrv_screen_create(int fd, const struct pipe_screen_config *config,
                      struct renderonly *ro)
{
   (void)ro;
   struct prismrv_screen *screen;
   struct pipe_caps *caps;
   uint64_t core_id_raw, core_rev_raw, errata_raw;

   screen = rzalloc(NULL, struct prismrv_screen);
   if (!screen)
      return NULL;

   screen->fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
   if (screen->fd < 0) {
      ralloc_free(screen);
      return NULL;
   }

   /*
    * Query the two separate identification registers (UAPI v2).
    *
    * Both queries must succeed.  UINT64_MAX means ioctl failure
    * (kernel too old, wrong driver, or UAPI version mismatch) and
    * is treated as a hard error rather than silently continuing
    * with wrong feature flags.
    */
   uint64_t uapi_ver = 0;
   int e_ver = prismrv_drm_get_param(screen->fd, PRISMRV_PARAM_UAPI_VERSION,
                                     &uapi_ver);
   if (e_ver || uapi_ver != PRISMRV_UAPI_VERSION) {
      fprintf(stderr, "prismrv: kernel UAPI version %" PRIu64 " (%d), "
              "need %d\n", uapi_ver, e_ver, PRISMRV_UAPI_VERSION);
      close(screen->fd);
      ralloc_free(screen);
      return NULL;
   }
   if (prismrv_drm_get_param(screen->fd, PRISMRV_PARAM_CMD_ABI,
                             &screen->cmd_abi) ||
       screen->cmd_abi != PRISMRV_CMD_ABI_STREAM_V1) {
      fprintf(stderr, "prismrv: unsupported kernel command ABI\n");
      close(screen->fd);
      ralloc_free(screen);
      return NULL;
   }

   int e_id = prismrv_drm_get_param(screen->fd, PRISMRV_PARAM_CORE_ID,
                                    &core_id_raw);
   int e_rev = prismrv_drm_get_param(screen->fd, PRISMRV_PARAM_CORE_REVISION,
                                     &core_rev_raw);
   if (e_id || e_rev) {
      fprintf(stderr, "prismrv: GET_PARAM(CORE_ID/CORE_REVISION) failed "
              "— kernel driver too old or UAPI mismatch\n");
      close(screen->fd);
      ralloc_free(screen);
      return NULL;
   }

   screen->core_id       = (uint32_t)core_id_raw;
   screen->core_revision = (uint32_t)core_rev_raw;

   /*
    * The core_id low 16 bits carry the core type (e.g. 0x0144 for
    * SGX544).  The upper 16 bits are the designer field which is the
    * same for all SGX variants and is not used for table lookup.
    */
   screen->info = prismrv_core_lookup(screen->core_id & 0xffff);
   if (!screen->info) {
      fprintf(stderr, "prismrv: unsupported SGX core 0x%04x "
              "(EUR_CR_CORE_ID=0x%08x) — add a chipinfo entry\n",
              screen->core_id & 0xffff, screen->core_id);
      close(screen->fd);
      ralloc_free(screen);
      return NULL;
   }

   if (prismrv_drm_get_param(screen->fd, PRISMRV_PARAM_ERRATA, &errata_raw)) {
      fprintf(stderr, "prismrv: GET_PARAM(ERRATA) failed\n");
      close(screen->fd);
      ralloc_free(screen);
      return NULL;
   }
   screen->errata_mask = errata_raw;

   debug_printf("prismrv: %s core_id=0x%08x rev=0x%08x (errata %#x)\n",
                screen->info->name, screen->core_id, screen->core_revision,
                (uint32_t)screen->errata_mask);

   screen->base.destroy = prismrv_screen_destroy;
   screen->base.get_name = prismrv_screen_get_name;
   screen->base.get_vendor = prismrv_screen_get_vendor;
   screen->base.get_device_vendor = prismrv_screen_get_device_vendor;
   screen->base.context_create = prismrv_screen_context_create;
   screen->base.is_format_supported = prismrv_screen_is_format_supported;

   prismrv_init_shader_caps(&screen->base);
   for (unsigned i = 0; i < ARRAY_SIZE(screen->base.nir_options); i++)
      screen->base.nir_options[i] = prismrv_get_nir_options();

   caps = (struct pipe_caps *)&screen->base.caps;
   u_init_pipe_screen_caps(&screen->base, 1);

   caps->npot_textures = true;
   caps->blend_equation_separate = true;
   caps->uma = true;
   caps->max_render_targets = 1;
   caps->max_texture_2d_size = screen->info->max_rt_width;
   caps->max_texture_3d_levels = 0;    /* no 3D textures on SGX5xx */
   caps->max_texture_cube_levels = 0;

   /*
    * Correct the u_init_pipe_screen_caps() defaults that over-claim:
    * the backend only handles triangles/lines/points, a small NIR
    * subset (no loops/branches), and keeps at most 8 vertex buffers.
    * Advertising more makes st/mesa hand us state we cannot honour.
    */
   caps->supported_prim_modes =
      (1u << MESA_PRIM_TRIANGLES) | (1u << MESA_PRIM_LINES) |
      (1u << MESA_PRIM_POINTS);
   caps->supported_prim_modes_with_restart = 0;
   caps->glsl_feature_level = 100;
   caps->glsl_feature_level_compatibility = 100;
   caps->max_vertex_buffers = ARRAY_SIZE(((struct prismrv_context *)0)->
                                         vertex_buffers);
   caps->max_varyings = 1;
   /* what the draw path cannot honour must not be advertised */
   caps->primitive_restart = false;
   caps->primitive_restart_fixed_index = false;
   caps->vertex_element_instance_divisor = false;
   caps->vs_instanceid = false;
   caps->start_instance = false;
   caps->draw_indirect = false;
   caps->multi_draw_indirect = false;
   caps->multi_draw_indirect_params = false;
   caps->user_vertex_buffers = false;
   caps->texture_swizzle = false;
   caps->occlusion_query = false;
   caps->depth_clip_disable = false;
   caps->anisotropic_filter = false;
   caps->cube_map_array = false;
   caps->texture_buffer_objects = false;
   caps->texture_multisample = false;
   caps->seamless_cube_map = false;
   caps->max_stream_output_buffers = 0;
   caps->max_viewports = 1;
   caps->max_texture_array_layers = 0;
   caps->glsl_feature_level = 100;
   caps->glsl_feature_level_compatibility = 100;

   prismrv_resource_screen_init(screen);
   prismrv_fence_screen_init(screen);

   return &screen->base;
}
