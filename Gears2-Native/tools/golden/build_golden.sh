#!/usr/bin/env bash
# Rebuilds tests/golden/layer0_renderer_core_golden.inc from the reference code (S3.5 groundwork).
# Needs: the rexglue SDK source tree at the pinned commit (bd833a2a + patches/rexglue-sdk.diff is fine: none of the files
# used here are patched), Xenia Canary e6cd2e4 sources, fmt, spdlog, simde, xxHash, clang++ with C++23.
# Usage: tools/golden/build_golden.sh <sdk> <canary> <fmt> <spdlog> <simde> <xxhash>   (run from the project root)
set -euo pipefail
SDK=$1; CANARY=$2; FMT=$3; SPDLOG=$4; SIMDE=$5; XXH=$6
ROOT=$(pwd); W=$(mktemp -d)
# Canary's headers include "third_party/fmt/include/..." and "third_party/xxhash/..." from its repo root.
mkdir -p "$W/fake/third_party/fmt"; ln -s "$FMT/include" "$W/fake/third_party/fmt/include"; ln -s "$XXH" "$W/fake/third_party/xxhash"
CF="-std=c++20 -O2 -DNDEBUG -DFMT_HEADER_ONLY -ffp-contract=off -I$W/fake -I$CANARY/src -I$ROOT/tests"
for f in texture_util texture_info texture_info_formats texture_extent xenos; do
  clang++ $CF -c "$CANARY/src/xenia/gpu/$f.cc" -o "$W/c_$f.o"
done
clang++ $CF -c "$ROOT/tools/golden/gn_renderer_core_golden_canary.cpp" -o "$W/gcan.o"
clang++ -std=c++23 -O2 -DNDEBUG -DFMT_HEADER_ONLY -ffp-contract=off -I"$SDK/include" -I"$FMT/include" -I"$SPDLOG/include" \
  -I"$SIMDE" -I"$XXH" -I"$ROOT/src/layer0_xenos/include" -I"$ROOT/tests" \
  "$ROOT/tools/golden/gn_renderer_core_golden.cpp" \
  "$SDK"/src/graphics/pipeline/texture/{util,info,info_formats,extent}.cpp "$SDK/src/core/math_gcc.cpp" \
  "$SDK/src/graphics/xenos.cpp" "$SDK/src/ui/graphics_util.cpp" "$W"/gcan.o "$W"/c_*.o -lpthread -o "$W/gen"
"$W/gen" "$ROOT/tests/golden/layer0_renderer_core_golden.inc"
rm -rf "$W"
