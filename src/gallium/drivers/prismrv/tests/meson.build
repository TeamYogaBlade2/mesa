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

libprismrv_fdleak = executable(
  'prismrv_fdleak',
  'prismrv_fdleak.c',
  include_directories: [inc_include, inc_src],
  dependencies: [dependency('egl'), dependency('glesv2')],
  link_args: ['-L' + join_paths(meson.current_build_dir(), '../../../egl')],
  build_by_default: false,
)

if with_egl
  test('prismrv-smoke', libprismrv_smoke,
    env: [
      'LD_PRELOAD=' + libprismrv_noop_drm_shim.full_path(),
      # Use the EGL/gallium just built, not the system Mesa: without
      # these the loader silently falls back to llvmpipe and the test
      # never touches this driver (the tests now check GL_RENDERER).
      '__EGL_VENDOR_LIBRARY_FILENAMES=' + meson.project_build_root() /
         'src/egl/devenv_glvnd_egl.json',
      'LD_LIBRARY_PATH=' + ':'.join([
         meson.project_build_root() / 'src/gallium/targets/dri',
         meson.project_build_root() / 'src/egl',
         meson.project_build_root() / 'src/gallium/drivers/prismrv/drm-shim',
      ]),
      'DRM_SHIM_DEBUG=1',
      'EGL_PLATFORM=surfaceless',
      'MESA_LOADER_DRIVER_OVERRIDE=prismrv',
    ],
    suite: ['prismrv'],
  )

  libprismrv_lifecycle = executable(
    'prismrv_lifecycle',
    'prismrv_lifecycle.c',
    include_directories: [inc_include, inc_src],
    dependencies: [
      dep_thread, idep_mesautil,
      dependency('egl'),
      dependency('glesv2'),
    ],
    build_by_default: false,
  )

  test('prismrv-lifecycle', libprismrv_lifecycle,
    env: [
      'LD_PRELOAD=' + libprismrv_noop_drm_shim.full_path(),
      # Use the EGL/gallium just built, not the system Mesa: without
      # these the loader silently falls back to llvmpipe and the test
      # never touches this driver (the tests now check GL_RENDERER).
      '__EGL_VENDOR_LIBRARY_FILENAMES=' + meson.project_build_root() /
         'src/egl/devenv_glvnd_egl.json',
      'LD_LIBRARY_PATH=' + ':'.join([
         meson.project_build_root() / 'src/gallium/targets/dri',
         meson.project_build_root() / 'src/egl',
         meson.project_build_root() / 'src/gallium/drivers/prismrv/drm-shim',
      ]),
      'EGL_PLATFORM=surfaceless',
      'MESA_LOADER_DRIVER_OVERRIDE=prismrv',
      'MESA_DEBUG=1',
    ],
    timeout: 120,
    suite: ['prismrv'],
  )

  # a screen's file descriptor used to be dup()ed twice, so the screen cache
  # never dropped its entry and every display create/destroy leaked one fd
  test('prismrv-fdleak', libprismrv_fdleak,
    env: [
      'LD_PRELOAD=' + libprismrv_noop_drm_shim.full_path(),
      '__EGL_VENDOR_LIBRARY_FILENAMES=' + meson.project_build_root() /
         'src/egl/devenv_glvnd_egl.json',
      'LD_LIBRARY_PATH=' + ':'.join([
         meson.project_build_root() / 'src/gallium/targets/dri',
         meson.project_build_root() / 'src/egl',
         meson.project_build_root() / 'src/gallium/drivers/prismrv/drm-shim',
      ]),
      'EGL_PLATFORM=surfaceless',
      'MESA_LOADER_DRIVER_OVERRIDE=prismrv',
    ],
    timeout: 60,
    suite: ['prismrv'],
  )
endif
