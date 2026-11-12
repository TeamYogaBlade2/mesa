#!/bin/sh
# Host test: run the Linux driver's command-stream validator
# (drivers/gpu/drm/prismrv/prismrv_stream.c) over streams that Mesa really
# produced (dumped by the drm-shim), plus hostile variants of them.
#
# usage: run_stream_validate.sh <linux-tree> <stream-dump>
set -e
LINUX=${1:?linux tree}
DUMP=${2:?dump file}
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cp "$LINUX/drivers/gpu/drm/prismrv/prismrv_stream.c" "$T/"
cp "$HERE/stubs/prismrv_device.h" "$T/"
cp "$HERE/stream_validate_test.c" "$T/"
# -I"$HERE/../../../../../include/drm-uapi" is Mesa's DRM uAPI directory, which
# is where drm.h lives. prismrv_drm.h does `#include "drm.h"`, so without this the
# validator build fails with
#
#     prismrv_drm.h:9:10: fatal error: drm.h: No such file or directory
#
# even though the stub prismrv_drm.h is present: the stub supplies the prismrv
# header, not the DRM header that one includes. Pointing at the source root alone
# is not enough - the file is one level down in include/drm-uapi.
${CC:-cc} -Wall -Werror -I"$T" -I"$HERE/stubs" -I"$HERE/stubs/uapi" \
    -I"$HERE/.." -I"$HERE/../../../../../include/drm-uapi" \
    -o "$T/svt" "$T/stream_validate_test.c"
"$T/svt" "$DUMP"
