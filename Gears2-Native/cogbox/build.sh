#!/bin/sh
# CogBox: cross-compile CogBox.exe (64-bit Windows) on Linux with MinGW. Usage: ./build.sh [debug]
set -e
cd "$(dirname "$0")"
VER=$(grep -o '"[0-9.]*"' src/app.h | head -1 | tr -d '"')
SDL=${SDL3_DIR:-/tmp/claude-0/sdl/SDL3-3.4.18/x86_64-w64-mingw32}
# The library music's XMA decoder (src/gamesound.cpp): FFmpeg's libavcodec + libavutil, LGPL 2.1, built by
# tools/build_ffmpeg_xma.sh with the xma1 / xma2 decoders only (~1 MB of DLLs). Linked at run time (LoadLibrary), never
# statically: without the DLLs CogBox runs and the library is silent. Without FFMPEG_DIR's headers the music is left out.
FF=${FFMPEG_DIR:-third_party/ffmpeg-xma}   # (not committed: .gitignore)
[ -f $FF/include/libavcodec/avcodec.h ] || [ -n "$FFMPEG_DIR" ] || FF=/tmp/claude-0/ffmpeg-xma   # (where SDL3_DIR's default lives)
# GitHub #151: 0.4.1 - 0.4.3 were built from a clone without these headers and shipped without library music. A build
# without them now stops, unless COGBOX_NO_FFMPEG=1 asks for a silent library on purpose.
if [ -f $FF/include/libavcodec/avcodec.h ]; then FFDEF="-isystem $FF/include"
elif [ "$COGBOX_NO_FFMPEG" = 1 ]; then FFDEF="-DCOGBOX_NO_FFMPEG"
else echo "build.sh: no FFmpeg headers in $FF (tools/build_ffmpeg_xma.sh) - set FFMPEG_DIR, or COGBOX_NO_FFMPEG=1 for no music" >&2; exit 1; fi
CXX=x86_64-w64-mingw32-g++
OUT=build; mkdir -p $OUT/obj
OPT="-O2"; [ "$1" = debug ] && OPT="-O0 -g"
IFS=. read A B C <<X
$VER
X
printf '#define VERSION_RC %s,%s,%s,0\n#define VERSION_STR "%s"\n' "$A" "$B" "${C:-0}" "$VER" > res/version_rc.h
x86_64-w64-mingw32-windres -I res res/cogbox.rc -O coff -o $OUT/obj/cogbox.res.o
FLAGS="$OPT -std=c++17 -Wall -Wextra -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -Ithird_party/imgui -I$SDL/include -Isrc -I../src/layer2_ue3/include -I../src/layer0_xenos/include $FFDEF"
objs=""
for f in third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp \
         third_party/imgui/imgui_impl_sdl3.cpp third_party/imgui/imgui_impl_sdlrenderer3.cpp; do
  o=$OUT/obj/$(basename $f .cpp).o; objs="$objs $o"
  [ -f $o ] && [ $o -nt $f ] || $CXX $OPT -std=c++17 -w -Ithird_party/imgui -I$SDL/include -c $f -o $o
done
o=$OUT/obj/miniz.o; objs="$objs $o"
[ -f $o ] || x86_64-w64-mingw32-gcc $OPT -w -c third_party/miniz/miniz.c -o $o
for f in src/*.cpp ../src/layer2_ue3/prebuild_lzx.cpp ../src/layer0_xenos/achievements_data.cpp \
         ../src/layer0_xenos/achievements_browser_model.cpp ../src/layer0_xenos/achievements_popup_logic.cpp; do   # (the LZX decoder: Gears2-Native's own, MIT)
  case $f in src/hello.cpp) continue;; esac
  o=$OUT/obj/$(basename $f .cpp).o; objs="$objs $o"
  $CXX $FLAGS -c $f -o $o
done
$CXX -mwindows -static -static-libgcc -static-libstdc++ $([ "$1" = debug ] || echo -s) -o $OUT/CogBox.exe $objs $OUT/obj/cogbox.res.o \
  $SDL/lib/libSDL3.dll.a -lshell32 -lole32 -luuid -lwinhttp -lbcrypt -ladvapi32 -ldxgi -lcomdlg32
cp $SDL/bin/SDL3.dll $OUT/ 2>/dev/null || true
cp $FF/bin/avcodec-*.dll $FF/bin/avutil-*.dll $OUT/ 2>/dev/null || true
ls -la $OUT/CogBox.exe
