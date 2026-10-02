# PrismRV Gallium driver (PowerVR SGX 5xx)

Initial gallium driver for Imagination PowerVR SGX cores, currently
targeting the SGX544 in the MediaTek MT6589 through the prismrv DRM
kernel driver (drivers/gpu/drm/prismrv in the linux tree).

## Multi-core design

`prismrv_chipinfo.c` holds a per-core feature table (SGX530/540/544
today). The kernel exposes `EUR_CR_CORE_ID` and `EUR_CR_CORE_REVISION`
via `PRISMRV_PARAM_CORE_ID` / `PRISMRV_PARAM_CORE_REVISION`; the screen
selects a table row at create time,
so adding another variant is one table entry.

## Command stream ABI: read this first

The driver emits the **PrismRV command stream**
(`PRISMRV_CMD_ABI_STREAM_V1`, documented at the top of
`prismrv_context.c`), executed by the PrismRV emulator / a custom
uKernel.  The Linux driver does not interpret it, and the stock vendor
uKernel does not understand it.  The screen refuses to start unless the
kernel reports `PRISMRV_UAPI_VERSION` 3 and this command ABI, and every
address inside a stream is a GPU VA obtained from `GEM_CREATE`.  This is
therefore not yet a driver for unmodified SGX hardware firmware.

## Security contract (render node)

The kernel treats every submit as hostile: it accepts only TA jobs,
snapshots and validates the command stream and the TA packets, restricts
shaders to the closed text subset the backend emits, and rejects any
address outside the listed BOs (see `PRISMRV_STREAM_MAX_BYTES` and the
STREAM_V1 contract in the UAPI header).  If the backend ever emits a new
mnemonic or packet, the kernel validator (`prismrv_stream.c`) must learn
it first.  `tests/run_stream_validate.sh <linux-tree> <dump>` checks this
against real Mesa output (dump with `PRISMRV_SHIM_STREAM_DUMP`).

`PIPE_BIND_DISPLAY_TARGET` is advertised for colour formats only so the
DRI frontend creates configs; there is no window-system presentation or
export path yet, so only surfaceless/pbuffer rendering is meaningful.

## Known semantic gaps

* Sampler state (wrap, filter, mip filter) is carried in `SET_TEXTURE`
  but the PrismRV executor still samples nearest + clamp from level 0;
  mip chains are allocated but only level 0 is addressed.
* CPU access to a resource waits for the whole context's GPU work (no
  per-resource tracking), and the kernel does cache maintenance when a
  job retires; neither is exercised by the no-op drm-shim.

## Supported subset

Single-basic-block GLSL ES 1.00 style shaders (no branches, loops or
calls; float ALU mov/neg/add/sub/mul/fma/rcp/rsq; 2D nearest texturing in
the fragment stage; 4 vertex attributes, 4 vec4 uniforms, one varying).
Everything else fails shader compilation instead of being approximated.
`glClear` is a CPU clear; depth/stencil, MSAA, instancing and indirect
draws are not advertised.  Viewport/scissor/blend/depth/raster state is
emitted as opcodes 8-12 but has no effect until the executor implements
them.

## Build

Build with meson (from a Mesa checkout):

    meson setup build -Dgallium-drivers=prismrv -Dllvm=disabled \
      -Dvulkan-drivers=[] -Dglx=disabled -Dplatforms=[] -Degl=enabled \
      -Dgles2=enabled -Dtools=drm-shim -Dbuild-tests=true
    ninja -C build

Unit tests: `prismrv_nir_test` executes the generated USSE text on a tiny
interpreter and checks the numbers; `meson test --suite prismrv` runs the
EGL smoke/lifecycle tests on the drm-shim.

`drm-uapi/prismrv_drm.h` is a copy of the kernel uAPI header; keep it in
sync with `include/uapi/drm/prismrv_drm.h` in the linux tree.
