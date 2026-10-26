#!/bin/sh
# Cloud-side syntax check of our C++ (not a build: link errors still need the PC build). Needs the rexglue SDK source
# and its header-only deps (fmt, spdlog, simde) and Gears 2's layer 0 include folder. Usage: tools/syntax_check.sh file.cpp...
D=${DEPS:-/tmp/claude-0}
L0=${LAYER0_INCLUDE:-/mnt/user-data/uploads/Gears2-Native/src/layer0_xenos/include}
for f in "$@"; do
  echo "== $f"
  clang++ -std=c++23 -fsyntax-only -mavx2 -DSPDLOG_FMT_EXTERNAL -I"$D/rexsdk/include" -I"$D/fmt_src/include" \
    -I"$D/spdlog_src/include" -I"$D/simde_src" -I"$L0" -Isrc/layer2_ue3/include "$f" 2>&1 | grep -E "error" | head -20
done
