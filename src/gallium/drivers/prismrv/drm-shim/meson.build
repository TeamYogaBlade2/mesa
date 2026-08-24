# Copyright 2026 PrismRV project
# SPDX-License-Identifier: MIT

libprismrv_noop_drm_shim = shared_library(
  'prismrv_noop_drm_shim',
  'prismrv_noop.c',
  include_directories: [inc_include, inc_src],
  dependencies: [dep_drm_shim, idep_mesautil],
  gnu_symbol_visibility : 'hidden',
  install : true,
)
