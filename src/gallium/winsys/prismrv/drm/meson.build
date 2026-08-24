# Copyright 2026 PrismRV project
# SPDX-License-Identifier: MIT

libprismrvwinsys = static_library(
  'prismrvwinsys',
  files('prismrv_drm_winsys.c'),
  include_directories : [
    inc_src, inc_include, inc_gallium, inc_gallium_aux,
  ],
  dependencies : [dep_libdrm],
)

if with_gallium_prismrv
  driver_prismrv = declare_dependency(
    compile_args : [],
    link_with : [libprismrvwinsys, libprismrv],
  )
endif
