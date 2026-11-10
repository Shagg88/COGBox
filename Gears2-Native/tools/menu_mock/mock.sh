#!/bin/sh
# Offline mock-up of the in-game menu (Linux / WSL, g++ + Python PIL). Usage: mock.sh <W> <H> <screenshot.png> <out.png> [pad|keys]
set -e
cd "$(dirname "$0")/../.."
I=cogbox/third_party/imgui; O=${TMPDIR:-/tmp}/menu_mock; mkdir -p $O
g++ -O1 tools/menu_mock/mock.cpp src/layer0_xenos/menu_look.cpp $I/imgui.cpp $I/imgui_draw.cpp $I/imgui_widgets.cpp $I/imgui_tables.cpp \
  -Isrc/layer0_xenos/include -I$I -o $O/mock
$O/mock "$1" "$2" assets/fonts $O/o.raw "${5:-pad}"
python3 -I tools/menu_mock/mock.py $O/o.raw "$1" "$2" "$3" "$4"
