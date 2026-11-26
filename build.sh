#!/bin/sh
set -e
cd "$(dirname "$0")"
gen=obj/linux
mkdir -p bin "$gen"
glslangValidator -D -V -S vert -e main --vn cells_vs_spv -o "$gen/cells_vs_spv.h" shaders/cells_vs.hlsl > /dev/null
glslangValidator -D -V -S frag -e main --sbb 1 --vn cells_ps_spv -o "$gen/cells_ps_spv.h" shaders/cells_ps.hlsl > /dev/null
[ "$gen/cascadia_mono.h" -nt fonts/CascadiaMono.ttf ] || xxd -i -n cascadia_mono fonts/CascadiaMono.ttf > "$gen/cascadia_mono.h"
${CXX:-g++} -std=c++20 -O2 -pthread -static-libstdc++ -static-libgcc -I"$gen" $(pkg-config --cflags freetype2 fontconfig) -o bin/ccc src/main.cpp \
    -lX11 -lvulkan $(pkg-config --libs freetype2 fontconfig) "$@"
