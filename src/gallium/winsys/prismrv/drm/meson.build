# Copyright 2026 PrismRV project
# SPDX-License-Identifier: MIT

libprismrvwinsys = static_library(
  'prismrvwinsys',
  files('prismrv_drm_winsys.c'),
  include_directories : [
    inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
  ],
  dependencies : [dep_libdrm],
)

driver_prismrv = declare_dependency(
  compile_args : ['-DGALLIUM_PRISMRV'],
  link_with : libprismrvwinsys,
)
