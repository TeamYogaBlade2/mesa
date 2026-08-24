# Copyright 2026 PrismRV project
# SPDX-License-Identifier: MIT

files_prismrv = files(
  'prismrv_batch.c',
  'prismrv_chipinfo.c',
  'prismrv_context.c',
  'prismrv_drmif.c',
  'prismrv_resource.c',
  'prismrv_screen.c',
)

prismrv_c_args = []
prismrv_includes = [
  inc_include,
  inc_src,
  inc_gallium,
  inc_gallium_aux,
  inc_gallium_winsys,
  # local drm-uapi/ holds prismrv_drm.h until it lands in libdrm
  include_directories('.'),
]

libprismrv = static_library(
  'prismrv',
  [files_prismrv],
  c_args : prismrv_c_args,
  include_directories : prismrv_includes,
  dependencies : [dep_libdrm, idep_mesautil, idep_nir],
)

if with_tools.contains('drm-shim')
  subdir('drm-shim')
endif

driver_prismrv = declare_dependency(
  compile_args : [],
  link_with : libprismrv,
)

# DRI target integration (enabled once the driver passes a full meson
# build; see the note in docs/prismrv):
#
# In src/gallium/auxiliary/target-helpers/drm_helper.h:
#   #include "prismrv/drm/prismrv_drm_public.h"
#   DEFINE_LOADER_DRM_ENTRYPOINT(prismrv)
#
# In src/gallium/meson.build:
#   gallium_drivers += [driver_prismrv]   # via driver_lima-style dep
#
# The kernel uAPI header lives in the linux tree at
# include/uapi/drm/prismrv_drm.h; the local copy under drm-uapi/ must be
# kept in sync until it is upstreamed to libdrm.
