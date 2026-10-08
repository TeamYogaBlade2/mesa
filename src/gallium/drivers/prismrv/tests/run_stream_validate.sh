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
${CC:-cc} -Wall -Werror -I"$T" -I"$HERE/stubs" -I"$HERE/stubs/uapi" \
    -I"$HERE/.." -o "$T/svt" "$T/stream_validate_test.c"
"$T/svt" "$DUMP"
