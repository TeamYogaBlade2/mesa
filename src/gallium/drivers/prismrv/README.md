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

## Native USSE back end (work in progress)

`prismrv_usse_nir.[ch]` compiles NIR straight to **native SGX544 USSE
words** (not text) through the verified encoders: plain VMAD
(`prismrv_usse_encode.c`) and the vmul/vadd/vmin/vmax group + vmov
(`prismrv_usse_encode_vec.c`).  Every shader is compiled both ways in
`create_*_state`; the native result is kept in `prismrv_shader_state::native`
and printed with `PRISMRV_NATIVE_USSE_DUMP=1`.  It is not yet put in the
command stream (that needs the stock-uKernel program registration and TA kick
ABI).  Only what the encoders can express compiles: the float ALU set
(mov/fneg/fabs/fadd/fsub/fmul/fmin/fmax/ffma), the special functions
frcp/frsq/flog2/fexp2 (as vrcp/vrsq/vlog/vexp), vector construction and
I/O, uniforms and literal constants.  Control flow and texturing do have
no verified encoding yet and fail cleanly; everything outside the list
above is rejected at compile time rather than approximated.

(An earlier version of this file claimed rcp/rsq fail.  They do not -
`prismrv_usse_nir.c` emits `vrcp`/`vrsq` and `prismrv_nir_test` checks the
numbers against NIR's own constant folding.  Worth re-reading the emit
switch rather than such notes when checking coverage.)

The register ABI assumptions are listed in `prismrv_usse_nir.h`;
`prismrv_nir_test` checks the output against NIR's own constant folding.

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

## Building the driver locally (meson is enough; no full Mesa build needed)

The driver, its winsys and its tests build on their own, which is fast enough to run
on every change and cheap enough to run on a tmpfs workspace:

    meson setup /tmp/mesabuild -Dgallium-drivers=prismrv -Dllvm=disabled \
      -Dvulkan-drivers=[] -Dglx=disabled -Dplatforms=[] -Degl=enabled \
      -Dgles2=enabled -Dtools=drm-shim -Dbuild-tests=true
    ninja -C /tmp/mesabuild -j2 \
      src/gallium/drivers/prismrv/libprismrv.a \
      src/gallium/drivers/prismrv/tests/prismrv_usse_encode_test \
      src/gallium/drivers/prismrv/tests/prismrv_usse_vec_test \
      src/gallium/drivers/prismrv/prismrv_nir_test

Measured: the library takes ~12 s at `-j2` and the three test binaries about 110 s
together on eight cores, with peak free memory unaffected. Naming the targets matters -
a bare `ninja` builds all of Mesa, which is where the resource cost would come from.

Then:

    ./src/gallium/drivers/prismrv/tests/prismrv_usse_encode_test   # 1559 vectors
    ./src/gallium/drivers/prismrv/tests/prismrv_usse_vec_test      # 400 + 400
    ./src/gallium/drivers/prismrv/prismrv_nir_test                 # 600 shaders

`prismrv_nir_test` is the strongest of the three: it compiles 600 shaders through the
NIR back end and compares 3600 output values against NIR's own constant folding. It had
never been run in this workspace before meson was available - the encoder tests are the
only ones that work without it.

meson needs the python `mako`, `pyyaml` and `packaging` modules. Only the *setup* needs
them, not the compiler itself.

## Checking the encoders without meson

The two encoder golden tests are self-contained - each needs only
`prismrv_usse_encode.c` / `prismrv_usse_encode_vec.c` and a C compiler, so
they can be built and run in a tree that has no meson (which is the normal
state of this workspace):

    cd src/gallium/drivers/prismrv/tests
    clang -O1 -o /tmp/enc prismrv_usse_encode_test.c ../prismrv_usse_encode.c
    /tmp/enc                      # 1559 golden vectors
    clang -O1 -o /tmp/vec prismrv_usse_vec_test.c \
          ../prismrv_usse_encode.c ../prismrv_usse_encode_vec.c
    /tmp/vec                      # 400 vec2 + 400 vmov vectors

Both print `PASS` and a vector count.  Run these after touching either
encoder; they catch a wrong bit position immediately, which is the class of
bug that otherwise shows up as a shader computing garbage on hardware.

`tests/run_stream_validate.sh` needs the kernel-side validator
(`drivers/gpu/drm/prismrv/prismrv_stream.c` from the linux tree) and cannot
run here - that driver lives on a branch this workspace does not check out.


`drm-uapi/prismrv_drm.h` is a copy of the kernel uAPI header; keep it in
sync with `include/uapi/drm/prismrv_drm.h` in the linux tree.

## CI notes

**Actions is currently disabled on this repository.** `gh workflow run` returns

    HTTP 422: Actions has been disabled for this repository

and the last run of any kind was on `64e66818` (2026-10-08). Fourteen commits have
landed since - all present on the remote - with no run at all, because nothing is
listening. So "push it and let CI build" cannot work until Actions is re-enabled in
the repository settings; re-enabling is the fix, not a change to the workflow.

This matters for the workflow's own advice below: a *red* run and a *missing* run are
different problems. The notes on PR-vs-push asymmetry describe a red run; what is
happening now is no run at all, and `gh run list` showing nothing for your SHA is the
signal.

`.github/workflows/prismrv.yml` has two jobs: a full `meson-build` and a
`syntax-only` pass. The build job's last step checks this driver's emitted command
stream against the kernel validator, which means it has to fetch
`TeamYogaBlade2/linux` at the revision pinned in `tests/LINUX_VALIDATOR_REV`:

    token: ${{ secrets.LINUX_REPO_TOKEN || github.token }}

Observed on `64e66818`: `push` passes, `pull_request` fails, and the failure lands on
exactly that step. **That is not a general rule** - `a78f67d3` passed on `pull_request`,
so "PRs always fail" would be the wrong reading. What it suggests is that the Linux
checkout step depends on something contextual (the secret, or the pinned revision being
reachable), and fails when that is not available rather than when the driver is wrong.

So before investigating the driver, check whether the same SHA passed on `push`:

    gh run list --limit 5 --repo TeamYogaBlade2/mesa \
        --json databaseId,conclusion,event,headSha \
        -q '.[] | "\(.databaseId) \(.conclusion) \(.event) \(.headSha[0:8])"'

Verifying the contract locally instead needs that same Linux revision, and this is worth
knowing before trying: the Linux checkout is a **partial clone with
`remote.origin.partialclonefilter=blob:none`**, so `git show <sha>:<path>`,
`git cat-file blob <sha>:<path>` and `git archive <sha>` all fetch the blob over the
network on demand. Measured here: over 90 s for one file, and a whole-tree `git archive`
timed out at 200 s. Do the extraction from a full clone, or run the check in CI.
