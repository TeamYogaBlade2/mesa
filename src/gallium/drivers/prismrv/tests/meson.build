# Copyright 2026 PrismRV project
# SPDX-License-Identifier: MIT
#
# End-to-end smoke test: runs the real Gallium prismrv driver through
# EGL/surfaceless against the drm-shim backend.  Requires the EGL
# libraries built by this same meson project, so it is only wired up
# when EGL is enabled (see the with_egl guard in CI).

libprismrv_smoke = executable(
  'prismrv_smoke',
  'prismrv_smoke.c',
  include_directories: [inc_include, inc_src],
  dependencies: [
    dep_thread, idep_mesautil,
    dependency('egl'),
    dependency('glesv2'),
  ],
  link_args: ['-L' + join_paths(meson.current_build_dir(), '../../../egl')],
  build_by_default: false,
)

if with_egl
  test('prismrv-smoke', libprismrv_smoke,
    env: [
      'LD_PRELOAD=' + libprismrv_noop_drm_shim.full_path(),
      'DRM_SHIM_DEBUG=1',
      'EGL_PLATFORM=surfaceless',
      'MESA_LOADER_DRIVER_OVERRIDE=prismrv',
    ],
    suite: ['prismrv'],
  )
endif
