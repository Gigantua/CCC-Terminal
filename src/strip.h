#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include "screen.h"

struct Strip {
    static constexpr int NEW_COLS = 3;
    int height = 0, tabTop = 0, textTop = 0, x0 = 0, cellW = 1, tabCols = 0, count = 0, buttonsX = 0, buttonW = 0;

    static int scaled(int v, unsigned dpi) { return (int)((v * (long long)dpi + 48) / 96); }

    static Strip make(int width, unsigned dpi, int cellW, int cellH, int count, int left = 8, bool buttons = true) {
        Strip s;
        if (cellW <= 0 || cellH <= 0) return s;
        s.cellW = cellW;
        s.count = count;
        s.tabTop = scaled(7, dpi);
        s.height = (std::max)(scaled(36, dpi), s.tabTop + cellH + scaled(10, dpi));
        s.textTop = s.tabTop + (s.height - s.tabTop - cellH) / 2;
        s.x0 = scaled(left, dpi);
        s.buttonW = buttons ? scaled(46, dpi) : 0;
        s.buttonsX = width - 3 * s.buttonW;
        int free = (s.buttonsX - s.x0 - scaled(40, dpi)) / cellW - NEW_COLS;
        s.tabCols = std::clamp(free / (std::max)(count, 1), 10, 32);
        return s;
    }

    int cols() const { return (std::max)(0, (buttonsX - x0) / cellW); }
    int colAt(int x) const { return x < x0 ? -1 : (x - x0) / cellW; }
    int tabAt(int x) const {
        int c = colAt(x);
        return c < 0 || c >= count * tabCols || x >= buttonsX ? -1 : c / tabCols;
    }
    bool onClose(int x) const { int c = colAt(x); return tabAt(x) >= 0 && c % tabCols == tabCols - 3; }
    bool onNew(int x) const { int c = colAt(x) - count * tabCols; return c >= 0 && c < NEW_COLS && x < buttonsX; }
    int button(int x) const { return x >= buttonsX && buttonW > 0 ? (std::min)((x - buttonsX) / buttonW, 2) : -1; }
    int tabLeft(int tab) const { return x0 + tab * tabCols * cellW; }
    int tabWidth() const { return (std::max)(tabCols - 1, 1) * cellW; }
    int tabHeight() const { return height - tabTop; }
    bool tornOff(int x, int y, int width) const { return x < 0 || x >= width || y < -height || y >= 2 * height; }
};

struct DragOutline {
    int width = 0, height = 0;
    std::vector<uint32_t> pixels;

    static DragOutline make(const Strip& s, unsigned dpi) {
        DragOutline o;
        o.width = s.tabWidth();
        o.height = s.tabHeight();
        o.pixels.assign((size_t)o.width * o.height, 0);
        float radius = (float)Strip::scaled(6, dpi), stroke = (float)(std::max)(Strip::scaled(2, dpi), 1);
        for (int y = 0; y < o.height; ++y)
            for (int x = 0; x < o.width; ++x) {
                float px = x + 0.5f, py = y + 0.5f;
                float dx = (std::max)((std::max)(radius - px, px - (o.width - radius)), 0.0f);
                float dy = (std::max)((std::max)(radius - py, py - (o.height - radius)), 0.0f);
                float inside = radius - std::sqrt(dx * dx + dy * dy);
                float edge = (std::min)({ px, py, o.width - px, o.height - py });
                float depth = dx > 0 && dy > 0 ? inside : (std::min)(inside, edge);
                float ring = std::clamp(depth + 0.5f, 0.0f, 1.0f) * std::clamp(stroke - depth + 0.5f, 0.0f, 1.0f);
                float fill = std::clamp(depth + 0.5f, 0.0f, 1.0f) * 0.14f;
                float a = (std::max)(ring, fill);
                auto channel = [a](int c) { return (uint32_t)(c * a + 0.5f); };
                o.pixels[(size_t)y * o.width + x] = (uint32_t)(a * 255 + 0.5f) << 24 | channel(58) << 16 | channel(150) << 8 | channel(221);
            }
        return o;
    }
};

struct StripTab {
    std::u32string title;
    bool activity = false;
};

struct StripHover {
    int tab = -1;
    bool close = false, plus = false;
};

namespace strip {

inline uint32_t mix(uint32_t a, uint32_t b, float t) {
    uint32_t out = 0;
    for (int s = 0; s < 24; s += 8) {
        float x = (float)(a >> s & 255) + ((float)(b >> s & 255) - (float)(a >> s & 255)) * t;
        out |= (uint32_t)(x + 0.5f) << s;
    }
    return out;
}

inline uint32_t contrast(uint32_t bg) {
    float y = 0.2126f * (bg & 255) + 0.7152f * (bg >> 8 & 255) + 0.0722f * (bg >> 16 & 255);
    return y < 128 ? 0xFFFFFF : 0x000000;
}

inline uint32_t barColor(uint32_t bg) { return mix(bg & 0xFFFFFF, contrast(bg), 0.09f); }

inline std::u32string codepoints(const std::string& utf8) {
    std::u32string out;
    for (size_t i = 0; i < utf8.size();) out += (char32_t)unicode::decodeUtf8(utf8, i);
    return out;
}

inline void place(Console::Frame& f, int col, int last, const std::u32string& text, uint32_t fg, uint32_t bg) {
    for (size_t i = 0; i < text.size() && col <= last; ++i) {
        char32_t cp = text[i];
        int w = Console::clusterWidth(&cp, 1);
        if (w <= 0) continue;
        if (col + w - 1 > last) break;
        f.cells[col] = cp < 0x20 ? ' ' : cp;
        f.attrs[col].fg = fg;
        f.attrs[col].bg = bg;
        if (w == 2) { f.cells[col + 1] = Console::WIDE_TAIL; f.attrs[col + 1] = f.attrs[col]; }
        col += w;
    }
}

inline std::u32string fit(const std::u32string& title, int cols) {
    if ((int)title.size() <= cols) return title;
    return cols > 1 ? title.substr(0, cols - 1) + U"\x2026" : title.substr(0, cols);
}

inline void layout(Console::Frame& f, const Strip& s, const std::vector<StripTab>& tabs, int active, const StripHover& hover, uint32_t termBg) {
    uint32_t base = termBg & 0xFFFFFF, ink = contrast(base), bar = barColor(base);
    uint32_t hot = mix(base, ink, 0.17f), hotter = mix(base, ink, 0.26f);
    uint32_t dim = mix(base, ink, 0.62f), bright = mix(base, ink, 0.92f), accent = vt::rgb(58, 150, 221);
    f.cols = s.cols();
    f.rows = 1;
    f.clusters.clear();
    f.cells.assign(f.cols, ' ');
    vt::Attr blank;
    blank.fg = bright;
    blank.bg = bar;
    f.attrs.assign(f.cols, blank);
    for (int i = 0; i < (int)tabs.size(); ++i) {
        int c0 = i * s.tabCols, last = (std::min)(c0 + s.tabCols - 2, f.cols - 1);
        if (c0 >= f.cols) break;
        bool on = i == active, hovered = hover.tab == i;
        uint32_t bg = on ? base : hovered ? hot : bar, fg = on || hovered ? bright : dim;
        for (int c = c0; c <= last; ++c) { f.attrs[c].bg = bg; f.attrs[c].fg = fg; }
        if (tabs[i].activity && !on) place(f, c0, c0, U"\x2022", accent, bg);
        place(f, c0 + 1, (std::min)(last - 3, f.cols - 1), fit(tabs[i].title, s.tabCols - 5), fg, bg);
        int close = c0 + s.tabCols - 3;
        if (close < f.cols) place(f, close, close, U"\x00D7", hovered && hover.close ? bright : dim, hovered && hover.close ? hotter : bg);
    }
    int plus = (int)tabs.size() * s.tabCols;
    for (int c = plus; c < plus + Strip::NEW_COLS && c < f.cols; ++c) f.attrs[c].bg = hover.plus ? hot : bar;
    if (plus + 1 < f.cols) place(f, plus + 1, plus + 1, U"+", hover.plus ? bright : dim, hover.plus ? hot : bar);
}

}
