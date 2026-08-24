#!/bin/bash
# NIR→USSE test build script (meson-less compile testing)
set -e
cd /home/ai-agent/work/mesa
VENV_PY=/tmp/mesa-venv/bin/python
BUILD=/tmp/prismrv-nir-build
mkdir -p "$BUILD"

FLAGS="-std=c11 -O0 -g -DHAVE_ENDIAN_H -DHAVE_PTHREAD -DHAVE_STRUCT_TIMESPEC -D_GNU_SOURCE -DNDEBUG -DHAVE_SYSCONF=1"
INC="-Isrc/compiler/nir/generated -Isrc/compiler/generated -Isrc -Isrc/compiler/nir -Isrc/compiler -Isrc/util -Iinclude -Iinclude/c11"

# 1) generate nir headers/sources if missing
[ -f src/compiler/generated/builtin_types.h ] || (cd src/compiler && $VENV_PY builtin_types_h.py generated/builtin_types.h)
[ -f src/compiler/generated/builtin_types.c ] || (cd src/compiler && $VENV_PY builtin_types_c.py generated/builtin_types.c)
[ -f src/compiler/nir/generated/nir_opcodes.h ] || (cd src/compiler/nir && $VENV_PY nir_opcodes_h.py > generated/nir_opcodes.h)
[ -f src/compiler/nir/generated/nir_opcodes.c ] || (cd src/compiler/nir && $VENV_PY nir_opcodes_c.py > generated/nir_opcodes.c)
[ -f src/compiler/nir/generated/nir_builder_opcodes.h ] || (cd src/compiler/nir && $VENV_PY nir_builder_opcodes_h.py > generated/nir_builder_opcodes.h)
[ -f src/compiler/nir/generated/nir_intrinsics.h ] || (cd src/compiler/nir && $VENV_PY nir_intrinsics_h.py --out generated/nir_intrinsics.h)
[ -f src/compiler/nir/generated/nir_intrinsics_indices.h ] || (cd src/compiler/nir && $VENV_PY nir_intrinsics_indices_h.py --out generated/nir_intrinsics_indices.h)
[ -f src/compiler/nir/generated/nir_intrinsics.c ] || (cd src/compiler/nir && $VENV_PY nir_intrinsics_c.py --out generated/nir_intrinsics.c)
[ -f src/compiler/nir/generated/nir_constant_expressions.c ] || (cd src/compiler/nir && $VENV_PY nir_constant_expressions.py > generated/nir_constant_expressions.c)
[ -f src/compiler/nir/generated/nir_opt_algebraic.c ] || (cd src/compiler/nir && $VENV_PY nir_algebraic.py > generated/nir_opt_algebraic.c)

# 2) build libnir from all nir .c files + the generated ones
for c in src/compiler/nir/*.c src/compiler/nir/generated/nir_opcodes.c src/compiler/nir/generated/nir_intrinsics.c; do
  o="$BUILD/$(basename $c .c).o"
  [ -f "$o" ] && [ "$o" -nt "$c" ] && continue
  clang $FLAGS $INC -c "$c" -o "$o" &
done
wait

# glsl types (needs generated/builtin_types.*)
clang $FLAGS -Isrc -Isrc/util/format/generated -Isrc/compiler/generated -Isrc/compiler -Isrc/util -Iinclude \
  -c src/compiler/glsl_types.c -o "$BUILD/glsl_types.o" &
clang $FLAGS -Isrc -Isrc/util/format/generated -Isrc/compiler/generated -Isrc/compiler -Isrc/util -Iinclude \
  -c src/compiler/generated/builtin_types.c -o "$BUILD/builtin_types.o" &
clang $FLAGS -Isrc/util/format/generated -Isrc -Iinclude -c src/util/u_cpu_detect.c -o "$BUILD/u_cpu_detect.o" &
clang $FLAGS -DHAVE_DIRENT_D_TYPE -DHAVE_POSIX_MEMALIGN -Isrc -Iinclude -c src/util/os_misc.c -o "$BUILD/os_misc.o" &
clang++ -std=c++17 $INC -Isrc -Iinclude -c src/util/u_qsort.cpp -o "$BUILD/u_qsort.o" &
wait

ar rcs "$BUILD/libnir.a" "$BUILD"/*.o

# 3) link the test
clang $FLAGS $INC \
  src/gallium/drivers/prismrv/prismrv_test_nir.c \
  src/gallium/drivers/prismrv/prismrv_program.c \
  src/compiler/shader_enums.c \
  src/util/ralloc.c src/util/u_math.c src/util/blob.c \
  src/util/u_idalloc.c src/util/u_debug.c src/util/string_buffer.c \
  src/util/hash_table.c src/util/set.c src/util/u_vector.c \
  src/util/half_float.c src/util/simple_mtx.c src/util/u_call_once.c \
  src/util/os_time.c src/util/os_file.c src/util/slab.c \
  src/util/u_dynarray.c src/util/u_printf.c \
  -Isrc/util/format src/util/format/generated/u_format_table.c src/util/format/u_format.c \
  "$BUILD/libnir.a" "$BUILD/glsl_types.o" "$BUILD/builtin_types.o" \
  src/util/hash_table.c src/util/set.c src/util/u_vector.c \
  src/util/format/u_format.c \
  "$BUILD/u_cpu_detect.o" "$BUILD/os_misc.o" "$BUILD/u_qsort.o" \
  -o /tmp/prismrv_nir_test -lpthread -lm
echo "build OK: /tmp/prismrv_nir_test"
