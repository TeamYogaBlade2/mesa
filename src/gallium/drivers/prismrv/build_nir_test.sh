#!/bin/bash
# Generate NIR headers/sources needed for prismrv driver compilation.
# Full build is done by CI (GitHub Actions PrismRV-CI workflow).
set -e
VENV_PY=/tmp/mesa-venv/bin/python
cd /home/ai-agent/work/mesa

[ -f src/compiler/generated/builtin_types.h ] || (cd src/compiler && $VENV_PY builtin_types_h.py generated/builtin_types.h)
[ -f src/compiler/nir/generated/nir_opcodes.h ] || (cd src/compiler/nir && $VENV_PY nir_opcodes_h.py > generated/nir_opcodes.h)
[ -f src/compiler/nir/generated/nir_builder_opcodes.h ] || (cd src/compiler/nir && $VENV_PY nir_builder_opcodes_h.py > generated/nir_builder_opcodes.h)
[ -f src/compiler/nir/generated/nir_intrinsics.h ] || (cd src/compiler/nir && $VENV_PY nir_intrinsics_h.py --out generated/nir_intrinsics.h)
[ -f src/compiler/nir/generated/nir_intrinsics_indices.h ] || (cd src/compiler/nir && $VENV_PY nir_intrinsics_indices_h.py --out generated/nir_intrinsics_indices.h)
echo "generated headers OK"
