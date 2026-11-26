#pragma once
#include <algorithm>
#include "../shell/console.h"
#include "../shell/palette.h"
#include "glyphs.h"
#include "gpu_vulkan.h"

namespace screen {

constexpr uint32_t CURSOR_BEAM = 1, BLINK = 2, STRIKE = 4, CURSOR_BAR = 16, CURSOR_HOLLOW = 32;

inline uint32_t resolveColor(uint32_t c, uint32_t fallback, bool bright, const uint32_t* table) {
    if (c & vt::COLOR_DEFAULT) return fallback & 0xFFFFFF;
    if (c & vt::COLOR_INDEXED) {
        uint32_t i = c & 255;
        return table[bright && i < 8 ? i + 8 : i];
    }
    return c & 0xFFFFFF;
}

inline uint32_t halve(uint32_t c) { return (c >> 1) & 0x7F7F7F; }

struct Highlight {
    int cursorRow = -1, cursorCol = -1;
    int selFrom = -1, selTo = -1;
    int linkFrom = -1, linkTo = -1;
    bool barCursor = false, beamCursor = false, hollowCursor = false;
};

struct View {
    Console::Frame frame;
    Highlight highlight;
    uint32_t fg = 0, bg = 0;
};

inline GpuConstants constants(const View& v, const FontMetrics& m, int pixelW, int pixelH, int barWidth, bool blinkOn) {
    GpuConstants k = {};
    k.cellW = m.cellW;
    k.cellH = m.cellH;
    k.cols = v.frame.cols;
    k.rows = v.frame.rows;
    k.blinkOn = blinkOn;
    k.underlineTop = m.underlineTop;
    k.underlineBottom = m.underlineBottom;
    k.strikeTop = m.strikeTop;
    k.strikeBottom = m.strikeBottom;
    k.marginColor = (v.frame.bg & vt::COLOR_DEFAULT ? v.bg : v.frame.bg) & 0xFFFFFF;
    k.barX = pixelW - barWidth;
    k.barColor = k.marginColor;
    if (v.frame.thumbSize < 1.0) {
        double thumbH = (std::max)(20.0, pixelH * v.frame.thumbSize);
        k.thumbTop = (uint32_t)((pixelH - thumbH) * v.frame.thumbPos);
        k.thumbBottom = k.thumbTop + (uint32_t)thumbH;
        k.barColor = 0x1E1E1E;
        k.thumbColor = 0x787878;
    }
    return k;
}

inline void pictureSize(const Picture& q, const FontMetrics& m, int& w, int& h) {
    if (q.destW) { w = q.destW; h = q.destH; return; }
    w = q.cols ? q.cols * m.cellW : q.rows && q.srcH ? q.rows * m.cellH * q.srcW / q.srcH : q.srcW;
    h = q.rows ? q.rows * m.cellH : q.cols && q.srcW ? q.cols * m.cellW * q.srcH / q.srcW : q.srcH;
}

inline uint32_t uploadPictures(const Console::Frame& f, const FontMetrics& m, Gpu& gpu) {
    std::vector<const Bitmap*> shown;
    size_t needed = 0;
    for (const Picture& q : f.pictures)
        if (q.bitmap && q.srcW > 0 && q.srcH > 0 && std::find(shown.begin(), shown.end(), q.bitmap.get()) == shown.end()) {
            shown.push_back(q.bitmap.get());
            needed += q.bitmap->pixels.size();
        }
    if (shown.empty()) return 0;
    auto layout = [&](std::unordered_map<uint64_t, uint32_t>& resident, std::vector<uint32_t>& offsets) {
        size_t top = 0;
        for (const Bitmap* b : shown) if (auto it = resident.find(b->key); it != resident.end()) top = (std::max)(top, it->second + b->pixels.size());
        offsets.clear();
        for (const Bitmap* b : shown) {
            auto it = resident.find(b->key);
            offsets.push_back(it != resident.end() ? it->second : (uint32_t)std::exchange(top, top + b->pixels.size()));
        }
        return top;
    };
    std::vector<uint32_t> offsets;
    size_t top = layout(gpu.resident(Gpu::TEXELS), offsets);
    if (top > 2 * needed) { gpu.resident(Gpu::TEXELS).clear(); top = layout(gpu.resident(Gpu::TEXELS), offsets); }
    uint32_t* out = static_cast<uint32_t*>(gpu.upload(Gpu::TEXELS, top * 4));
    std::unordered_map<uint64_t, uint32_t>& resident = gpu.resident(Gpu::TEXELS);
    if (resident.empty()) layout(resident, offsets);
    std::unordered_map<uint64_t, uint32_t> kept;
    for (size_t k = 0; k < shown.size(); ++k) {
        if (!resident.count(shown[k]->key)) std::memcpy(out + offsets[k], shown[k]->pixels.data(), shown[k]->pixels.size() * 4);
        kept[shown[k]->key] = offsets[k];
    }
    resident = std::move(kept);
    std::vector<GpuPicture> list;
    for (const Picture& q : f.pictures) {
        if (!q.bitmap || q.srcW <= 0 || q.srcH <= 0) continue;
        int w, h;
        pictureSize(q, m, w, h);
        if (w <= 0 || h <= 0) continue;
        uint32_t offset = offsets[std::find(shown.begin(), shown.end(), q.bitmap.get()) - shown.begin()];
        list.push_back({ q.col * m.cellW + q.offX, q.row * m.cellH + q.offY, w, h, offset, (uint32_t)q.bitmap->width,
                         (uint32_t)q.srcX, (uint32_t)q.srcY, (uint32_t)q.srcW, (uint32_t)q.srcH, q.z, 0 });
    }
    if (list.empty()) return 0;
    std::memcpy(gpu.upload(Gpu::PICTURES, list.size() * sizeof(GpuPicture)), list.data(), list.size() * sizeof(GpuPicture));
    return (uint32_t)list.size();
}

inline bool placeholder(uint32_t id, const Console::Frame& f) {
    constexpr uint32_t PLACEHOLDER = 0x10EEEE;
    if (id < Console::CLUSTER_BASE) return id == PLACEHOLDER;
    return id != Console::WIDE_TAIL && f.clusters[id - Console::CLUSTER_BASE][0] == PLACEHOLDER;
}

inline uint32_t extraCursor(int shape, const Highlight& h, uint32_t& fg, uint32_t& bg, const Console::Frame& f, const uint32_t* table) {
    if (shape == ExtraCursors::FOLLOW) shape = h.beamCursor ? ExtraCursors::BEAM : h.barCursor ? ExtraCursors::UNDERLINE : ExtraCursors::BLOCK;
    uint32_t color = f.extraCursorColor, text = f.extraTextColor;
    bool custom = color != ExtraCursors::REVERSE && !(color & vt::COLOR_DEFAULT);
    if (shape == ExtraCursors::BLOCK) {
        if (custom) { uint32_t cellBg = bg; bg = resolveColor(color, bg, false, table); fg = text == ExtraCursors::REVERSE || (text & vt::COLOR_DEFAULT) ? cellBg : resolveColor(text, fg, false, table); }
        else std::swap(fg, bg);
        return 0;
    }
    if (custom) fg = resolveColor(color, fg, false, table);
    return shape == ExtraCursors::BEAM ? CURSOR_BEAM : CURSOR_BAR;
}

inline int styleOf(const vt::Attr& a) { return ((a.flags & vt::BOLD) ? 1 : 0) | ((a.flags & vt::ITALIC) ? 2 : 0); }

inline uint32_t shaderFlags(const vt::Attr& a) {
    return ((a.flags & vt::BLINK) ? BLINK : 0) | ((a.flags & vt::STRIKE) ? STRIKE : 0);
}

inline bool buildCells(const Console::Frame& f, const Highlight& h, uint32_t defaultFg, uint32_t defaultBg, GlyphCache& glyphs, GpuCell* out) {
    int cursor = h.cursorRow >= 0 ? h.cursorRow * f.cols + h.cursorCol : -1;
    std::vector<uint8_t> extra(f.extraCursors.empty() ? 0 : (size_t)f.cols * f.rows);
    for (const auto& c : f.extraCursors) if (c.cell >= 0 && (size_t)c.cell < extra.size()) extra[c.cell] = (uint8_t)c.shape;
    const uint32_t* table = f.palette.size() == 256 ? f.palette.data() : palette::standardTable().data();
    if (!(f.fg & vt::COLOR_DEFAULT)) defaultFg = f.fg;
    if (!(f.bg & vt::COLOR_DEFAULT)) defaultBg = f.bg;
    size_t n = (size_t)f.cols * f.rows;
    uint32_t previous = 0;
    for (size_t i = 0; i < n; ++i) {
        const vt::Attr& a = f.attrs[i];
        uint32_t fg = resolveColor(a.fg, defaultFg, (a.flags & vt::BOLD) != 0, table);
        uint32_t bg = resolveColor(a.bg, defaultBg, false, table);
        if (a.flags & vt::DIM) fg = halve(fg);
        if (a.flags & vt::REVERSE) std::swap(fg, bg);
        if (a.flags & vt::INVISIBLE) fg = bg;
        bool atCursor = (int)i == cursor;
        bool lineCursor = h.barCursor || h.beamCursor || h.hollowCursor;
        bool inverted = (atCursor && !lineCursor) != ((int)i >= h.selFrom && (int)i < h.selTo);
        if (inverted) std::swap(fg, bg);
        if (atCursor && !(f.cursorColor & vt::COLOR_DEFAULT)) {
            if (lineCursor) fg = f.cursorColor & 0xFFFFFF;
            else { fg = defaultBg & 0xFFFFFF; bg = f.cursorColor & 0xFFFFFF; }
        }
        uint32_t id = f.cells[i];
        uint32_t glyph;
        if (id == Console::WIDE_TAIL) glyph = (previous & 0xFFFFFF) ? previous + 1 : 0;
        else glyph = placeholder(id, f) ? 0 : glyphs.glyph(id, f, styleOf(a));
        if (glyphs.overflow()) return false;
        previous = glyph;
        uint32_t style = (int)i >= h.linkFrom && (int)i < h.linkTo ? (std::max)(a.underline(), (int)vt::UNDERLINE_SINGLE) : a.underline();
        uint32_t underline = style ? (uint32_t)style << 24 | ((a.underlineColor & vt::COLOR_DEFAULT) ? fg : resolveColor(a.underlineColor, fg, false, table)) : 0;
        uint32_t cursorShape = !atCursor ? 0 : h.hollowCursor ? CURSOR_HOLLOW : h.barCursor ? CURSOR_BAR : h.beamCursor ? CURSOR_BEAM : 0;
        if (!extra.empty() && extra[i]) cursorShape |= extraCursor(extra[i], h, fg, bg, f, table);
        out[i] = { glyph | (shaderFlags(a) | cursorShape) << 24, fg, bg, underline };
    }
    return true;
}

}
