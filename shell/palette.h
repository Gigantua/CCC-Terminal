#pragma once
#include <array>
#include <cstdio>
#include <string>
#include "vt.h"

namespace palette {

using Table = std::array<uint32_t, 256>;

inline uint32_t standard(uint32_t i) {
    static const uint32_t ansi[16] = { 0x0C0C0C, 0x1F0FC5, 0x0EA113, 0x009CC1, 0xDA3700, 0x981788, 0xDD963A, 0xCCCCCC,
                                       0x767676, 0x5648E7, 0x0CC616, 0xA5F1F9, 0xFF783B, 0x9E00B4, 0xD6D661, 0xF2F2F2 };
    if (i < 16) return ansi[i];
    if (i < 232) {
        static const uint32_t level[6] = { 0, 95, 135, 175, 215, 255 };
        i -= 16;
        return vt::rgb(level[i / 36], level[i / 6 % 6], level[i % 6]);
    }
    uint32_t g = 8 + (i - 232) * 10;
    return vt::rgb(g, g, g);
}

inline const Table& standardTable() {
    static const Table t = [] { Table r; for (uint32_t i = 0; i < 256; ++i) r[i] = standard(i); return r; }();
    return t;
}

inline int hexDigit(char c) { return c >= '0' && c <= '9' ? c - '0' : (c | 0x20) >= 'a' && (c | 0x20) <= 'f' ? (c | 0x20) - 'a' + 10 : -1; }

inline bool hexChannel(const std::string& s, uint32_t& out) {
    if (s.empty() || s.size() > 4) return false;
    uint32_t v = 0;
    for (char c : s) { int d = hexDigit(c); if (d < 0) return false; v = v << 4 | (uint32_t)d; }
    uint32_t max = (1u << (4 * s.size())) - 1;
    out = (v * 255 + max / 2) / max;
    return true;
}

inline bool parse(const std::string& spec, uint32_t& color) {
    uint32_t c[3];
    if (spec.rfind("rgb:", 0) == 0) {
        size_t a = spec.find('/', 4), b = a == std::string::npos ? a : spec.find('/', a + 1);
        if (b == std::string::npos) return false;
        if (!hexChannel(spec.substr(4, a - 4), c[0]) || !hexChannel(spec.substr(a + 1, b - a - 1), c[1]) || !hexChannel(spec.substr(b + 1), c[2])) return false;
    }
    else if (!spec.empty() && spec[0] == '#' && (spec.size() - 1) % 3 == 0 && spec.size() > 1 && spec.size() <= 13) {
        size_t n = (spec.size() - 1) / 3;
        for (int k = 0; k < 3; ++k) if (!hexChannel(spec.substr(1 + k * n, n), c[k])) return false;
    }
    else return false;
    color = vt::rgb(c[0], c[1], c[2]);
    return true;
}

inline std::string format(uint32_t color) {
    char b[32];
    snprintf(b, sizeof(b), "rgb:%02x%02x/%02x%02x/%02x%02x", color & 255, color & 255, color >> 8 & 255, color >> 8 & 255, color >> 16 & 255, color >> 16 & 255);
    return b;
}

inline uint32_t luminance(uint32_t c) { return ((c & 255) * 299 + (c >> 8 & 255) * 587 + (c >> 16 & 255) * 114) / 1000; }

}
