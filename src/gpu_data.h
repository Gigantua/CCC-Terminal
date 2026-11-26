#pragma once
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

struct GpuCell { uint32_t glyph, fg, bg, underline; };

struct GpuPicture {
    int32_t x0, y0, w, h;
    uint32_t offset, width, srcX, srcY, srcW, srcH;
    int32_t z;
    uint32_t pad;
};

struct GpuConstants {
    uint32_t cellW, cellH, cols, rows;
    uint32_t blinkOn, marginColor, barX, thumbTop;
    uint32_t thumbBottom, barColor, thumbColor, underlineTop;
    uint32_t underlineBottom, strikeTop, strikeBottom, topInset;
    uint32_t stripOffset, stripCols, stripX, stripTextTop;
    uint32_t stripTabTop, stripColor, buttonsX, buttonW;
    uint32_t buttonHover, buttonPressed, iconColor, iconHalf;
    uint32_t iconStroke, maximized, closeColor, tabShape;
    uint32_t pictureCount;
};

inline void saveBmp(const std::string& path, uint32_t w, uint32_t h, const std::vector<uint8_t>& bgraTopDown) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return;
    auto u16 = [&](uint32_t v) { out.put((char)(v & 255)).put((char)(v >> 8 & 255)); };
    auto u32 = [&](uint32_t v) { u16(v & 0xFFFF); u16(v >> 16); };
    uint32_t bytes = w * h * 4;
    out.write("BM", 2);
    u32(54 + bytes); u32(0); u32(54);
    u32(40); u32(w); u32(h); u16(1); u16(32); u32(0); u32(bytes); u32(2835); u32(2835); u32(0); u32(0);
    for (uint32_t y = h; y-- > 0;) out.write(reinterpret_cast<const char*>(bgraTopDown.data() + (size_t)y * w * 4), (std::streamsize)w * 4);
}
