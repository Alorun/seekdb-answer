#!/usr/bin/env bash
set -Eeuo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
HIPVS_ROOT=${HIPVS_ROOT:-/opt/hipvs}
ROCM_ROOT=${ROCM_ROOT:-/opt/rocm}
HIPCC=${HIPCC:-$ROCM_ROOT/bin/hipcc}
OUTPUT=${1:-$HERE/libseekdb_cuvs_bridge.so}
FLAGS=${SK_CUVS_BUILD_FLAGS:-1}
[[ "$FLAGS" =~ ^[0-7]$ ]] || exit 2
[[ -x "$HIPCC" ]] || { echo "Missing hipcc: $HIPCC" >&2; exit 2; }
SDK="$HERE/kernel_build/deps"
INCLUDES=(-I"$SDK/include" -I"$HIPVS_ROOT/include" -I"$ROCM_ROOT/include")
# Extra roots are for offline header locations only; algorithm flags stay fixed.
if [[ -n ${SK_CUVS_INCLUDE_ROOTS:-} ]]; then
  IFS=: read -r -a ROOTS <<< "$SK_CUVS_INCLUDE_ROOTS"
  for ROOT in "${ROOTS[@]}"; do INCLUDES+=(-I"$ROOT"); done
fi
for HEADER in raft/core/resources.hpp rmm/device_uvector.hpp rocwmma/rocwmma.hpp; do
  FOUND=0
  for ((I=0; I<${#INCLUDES[@]}; I++)); do
    test -f "${INCLUDES[I]#-I}/$HEADER" && FOUND=1
  done
  if [[ $FOUND == 0 ]]; then
    echo "Missing offline C++ header: $HEADER; set SK_CUVS_INCLUDE_ROOTS to its existing include root" >&2
    exit 2
  fi
done
mkdir -p "$(dirname "$OUTPUT")"
OBJECT=${SK_CUVS_BUILDER_OBJECT:-$(mktemp "$(dirname "$OUTPUT")/cagra-builder.XXXXXX.o")}
if [[ -z ${SK_CUVS_BUILDER_OBJECT:-} ]]; then
  trap 'rm -f "$OBJECT"' EXIT
  "$HIPCC" -std=c++17 -O3 -fPIC -fvisibility=hidden -fopenmp -pthread \
    --offload-arch=gfx1100 -D__HIP_PLATFORM_AMD__ -DLIBCUDACXX_ENABLE_EXPERIMENTAL_MEMORY_RESOURCE "${INCLUDES[@]}" \
    -c "$HERE/kernel_build/custom_builder.hip" -o "$OBJECT"
fi
SDK_ARGS=()
for INC in "${INCLUDES[@]}"; do SDK_ARGS+=(--include "${INC#-I}"); done
SUPPORT=${SK_CUVS_SUPPORT_ARCHIVE:-$(dirname "$OUTPUT")/.seekdb-cuvs-sdk/libseekdb_build_support.a}
python3 "$SDK/build_support.py" --compiler "$HIPCC" --archive "$SUPPORT" "${SDK_ARGS[@]}"
LIBS=(-lcuvs_c)
for LIB in cuvs; do
  if [[ -f "$HIPVS_ROOT/lib/lib$LIB.so" || -f "$HIPVS_ROOT/lib/lib$LIB.a" ]]; then
    LIBS+=(-l"$LIB")
  fi
done
"$HIPCC" -std=c++17 -shared -fPIC -O2 -fvisibility=hidden -fopenmp -pthread --offload-arch=gfx1100 \
  -D__HIP_PLATFORM_AMD__ -DLIBCUDACXX_ENABLE_EXPERIMENTAL_MEMORY_RESOURCE -DSK_CUVS_BUILD_FLAGS="$FLAGS" \
  "${INCLUDES[@]}" -I"$HERE/kernel_build" \
  -x c++ "$HERE/seekdb_cuvs_bridge.c" -x none "$OBJECT" "$SUPPORT" \
  -L"$HIPVS_ROOT/lib" "${LIBS[@]}" \
  -Wl,-soname,libseekdb_cuvs_bridge.so \
  -Wl,--version-script,"$HERE/kernel_build/exports.map" \
  -Wl,-rpath,"$HIPVS_ROOT/lib:$ROCM_ROOT/lib:$ROCM_ROOT/llvm/lib" -o "$OUTPUT"
echo "BUILT_CUSTOM_GPU_BRIDGE flags=$FLAGS path=$OUTPUT"
