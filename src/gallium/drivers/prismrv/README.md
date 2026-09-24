# PrismRV Gallium driver (PowerVR SGX 5xx)

Initial gallium driver for Imagination PowerVR SGX cores, currently
targeting the SGX544 in the MediaTek MT6589 through the prismrv DRM
kernel driver (drivers/gpu/drm/prismrv in the linux tree).

## Multi-core design

`prismrv_chipinfo.c` holds a per-core feature table (SGX530/540/544
today). The kernel exposes the raw `EUR_CR_CORE_REVISION` register via
`PRISMRV_PARAM_GPU_ID`; the screen selects a table row at create time,
so adding another variant is one table entry.

## Build

The driver compiles standalone for syntax/type checking without meson:

    clang -fsyntax-only -std=c11 -DHAVE_ENDIAN_H -D_GNU_SOURCE \
      -Isrc -Isrc/gallium/include -Isrc/gallium/auxiliary \
      -Isrc/util -Iinclude src/gallium/drivers/prismrv/*.c

A full meson build additionally needs the driver registered in
`src/gallium/meson.build` and the DRI target hooked up
(`drm_helper.h`, see notes in `meson.build`) — deliberately deferred
until the implementation is functional.

`drm-uapi/prismrv_drm.h` is a local copy of the kernel uAPI header;
keep it in sync with the linux tree.
