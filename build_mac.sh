#!/bin/sh
set -e
cd "$(dirname "$0")"
sdk=${VULKAN_SDK:-$(ls -d "$HOME"/VulkanSDK/*/macOS | sort -V | tail -1)}
gen=obj/macos
mkdir -p bin "$gen"
"$sdk/bin/glslangValidator" -D -V -S vert -e main --vn cells_vs_spv -o "$gen/cells_vs_spv.h" shaders/cells_vs.hlsl > /dev/null
"$sdk/bin/glslangValidator" -D -V -S frag -e main --sbb 1 --vn cells_ps_spv -o "$gen/cells_ps_spv.h" shaders/cells_ps.hlsl > /dev/null
[ "$gen/cascadia_mono.h" -nt fonts/CascadiaMono.ttf ] || xxd -i -n cascadia_mono fonts/CascadiaMono.ttf > "$gen/cascadia_mono.h"
${CXX:-clang++} -std=c++20 -O2 -x objective-c++ -arch arm64 -arch x86_64 -mmacosx-version-min=13.0 -Wno-deprecated-anon-enum-enum-conversion \
    -I"$gen" -I"$sdk/include" -o bin/ccc src/main.cpp -x none "$sdk/lib/MoltenVK.xcframework/macos-arm64_x86_64/libMoltenVK.a" \
    -framework Cocoa -framework Metal -framework QuartzCore -framework IOSurface -framework IOKit -framework CoreText -framework CoreGraphics -liconv "$@"
