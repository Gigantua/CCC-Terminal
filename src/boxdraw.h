#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace boxdraw {

inline bool drawable(char32_t cp) {
    return (cp >= 0x2500 && cp <= 0x259F) || (cp >= 0xE0B0 && cp <= 0xE0B3) || (cp >= 0x1FB00 && cp <= 0x1FB3B);
}

inline int lightStroke(int cellW) {
    return (std::max)(1, (int)std::lround(cellW / 7.0));
}

class Tile {
public:
    Tile(int w, int h, uint8_t* alpha) : w(w), h(h), light(lightStroke(w)), alpha(alpha) {
        std::memset(alpha, 0, (size_t)w * h);
    }

    void fill(int x0, int y0, int x1, int y1, uint8_t a = 255) {
        x0 = (std::max)(x0, 0);
        y0 = (std::max)(y0, 0);
        x1 = (std::min)(x1, w);
        y1 = (std::min)(y1, h);
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) alpha[y * w + x] = (std::max)(alpha[y * w + x], a);
    }

    template <class Inside>
    void cover(Inside inside) {
        constexpr int S = 8;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                int hits = 0;
                for (int sy = 0; sy < S; ++sy)
                    for (int sx = 0; sx < S; ++sx) hits += inside(x + (sx + 0.5f) / S, y + (sy + 0.5f) / S);
                alpha[y * w + x] = (std::max)(alpha[y * w + x], (uint8_t)(hits * 255 / (S * S)));
            }
    }

    const int w, h, light;

private:
    uint8_t* alpha;
};

inline int thickness(int light, int weight) {
    return weight == 2 ? 2 * light : weight == 3 ? 3 * light : light;
}

inline int bandStart(int size, int stroke) {
    return (size - stroke) / 2;
}

template <class Fill>
void drawArmsAlong(int along, int across, int light, int a0, int a1, int c0, int c1, Fill fill) {
    int doubleStart = bandStart(along, 3 * light);
    int cross = (std::max)(c0, c1);
    for (int side = 0; side < 2; ++side) {
        int weight = side ? a1 : a0;
        if (!weight) continue;
        bool through = (side ? a0 : a1) != 0;
        int t = weight == 3 ? light : thickness(light, weight);
        for (int k = 0; k < (weight == 3 ? 2 : 1); ++k) {
            int r0 = weight == 3 ? bandStart(across, 3 * light) + 2 * k * light : bandStart(across, t);
            int start, end;
            if (cross == 3) {
                bool inner = weight == 3 ? (k ? c1 : c0) == 3 : c0 && c1 && !through;
                start = inner ? doubleStart + 2 * light : doubleStart;
                end = inner ? doubleStart + light : doubleStart + 3 * light;
            } else {
                int stop = cross ? thickness(light, cross) : thickness(light, weight);
                start = bandStart(along, stop);
                end = start + stop;
            }
            if (side) fill(start, along, r0, r0 + t);
            else fill(0, end, r0, r0 + t);
        }
    }
}

inline void drawArms(Tile& tile, int l, int r, int u, int d) {
    drawArmsAlong(tile.w, tile.h, tile.light, l, r, u, d, [&](int a0, int a1, int c0, int c1) { tile.fill(a0, c0, a1, c1); });
    drawArmsAlong(tile.h, tile.w, tile.light, u, d, l, r, [&](int a0, int a1, int c0, int c1) { tile.fill(c0, a0, c1, a1); });
}

inline void drawDashes(Tile& tile, int count, int weight, bool vertical) {
    int along = vertical ? tile.h : tile.w, across = vertical ? tile.w : tile.h;
    int t = thickness(tile.light, weight), c0 = bandStart(across, t);
    int gap = (std::max)(1, (int)std::lround(along / (count * 3.0)));
    for (int i = 0; i < count; ++i) {
        int s0 = i * along / count + gap / 2, s1 = (i + 1) * along / count - (gap - gap / 2);
        if (vertical) tile.fill(c0, s0, c0 + t, s1);
        else tile.fill(s0, c0, s1, c0 + t);
    }
}

inline float length(float x, float y) {
    return std::sqrt(x * x + y * y);
}

inline void drawArc(Tile& tile, int sx, int sy) {
    float half = tile.light * 0.5f;
    float cx = bandStart(tile.w, tile.light) + half, cy = bandStart(tile.h, tile.light) + half;
    float r = (std::min)(sx > 0 ? tile.w - cx : cx, sy > 0 ? tile.h - cy : cy);
    tile.cover([&](float x, float y) {
        float qx = (x - cx) * sx, qy = (y - cy) * sy;
        float horizontal = qx >= r ? std::fabs(qy) : length(qx - r, qy);
        float vertical = qy >= r ? std::fabs(qx) : length(qx, qy - r);
        float arc = qx <= r && qy <= r ? std::fabs(length(qx - r, qy - r) - r) : 1e9f;
        return (std::min)({ horizontal, vertical, arc }) <= half;
    });
}

inline float segmentDistance(float px, float py, float ax, float ay, float bx, float by) {
    float abx = bx - ax, aby = by - ay;
    float t = std::clamp(((px - ax) * abx + (py - ay) * aby) / (abx * abx + aby * aby), 0.0f, 1.0f);
    return length(px - ax - abx * t, py - ay - aby * t);
}

inline float lineDistance(float px, float py, float ax, float ay, float bx, float by) {
    float abx = bx - ax, aby = by - ay;
    return std::fabs((px - ax) * aby - (py - ay) * abx) / length(abx, aby);
}

inline void drawDiagonals(Tile& tile, bool rising, bool falling) {
    float half = tile.light * 0.5f, w = (float)tile.w, h = (float)tile.h;
    tile.cover([&](float x, float y) {
        return (rising && lineDistance(x, y, 0, h, w, 0) <= half) || (falling && lineDistance(x, y, 0, 0, w, h) <= half);
    });
}

inline void drawPowerline(Tile& tile, bool pointsLeft, bool solid) {
    float w = (float)tile.w, h = (float)tile.h, half = tile.light * 0.5f;
    tile.cover([&](float x, float y) {
        float fx = pointsLeft ? w - x : x;
        if (solid) return fx <= w * (1 - std::fabs(2 * y / h - 1));
        return (std::min)(segmentDistance(fx, y, 0, 0, w, h / 2), segmentDistance(fx, y, w, h / 2, 0, h)) <= half;
    });
}

inline int eighth(int size, int k) {
    return (size * k + 4) / 8;
}

inline void drawBlock(Tile& tile, char32_t cp) {
    int w = tile.w, h = tile.h;
    if (cp == 0x2580) tile.fill(0, 0, w, eighth(h, 4));
    else if (cp <= 0x2588) tile.fill(0, eighth(h, 8 - (int)(cp - 0x2580)), w, h);
    else if (cp <= 0x258F) tile.fill(0, 0, eighth(w, 8 - (int)(cp - 0x2588)), h);
    else if (cp == 0x2590) tile.fill(eighth(w, 4), 0, w, h);
    else if (cp <= 0x2593) tile.fill(0, 0, w, h, (uint8_t)(64 * (cp - 0x2590)));
    else if (cp == 0x2594) tile.fill(0, 0, w, eighth(h, 1));
    else if (cp == 0x2595) tile.fill(eighth(w, 7), 0, w, h);
    else {
        static const uint8_t quadrants[10] = { 4, 8, 1, 13, 9, 7, 11, 2, 6, 14 };
        int mask = quadrants[cp - 0x2596], mx = eighth(w, 4), my = eighth(h, 4);
        if (mask & 1) tile.fill(0, 0, mx, my);
        if (mask & 2) tile.fill(mx, 0, w, my);
        if (mask & 4) tile.fill(0, my, mx, h);
        if (mask & 8) tile.fill(mx, my, w, h);
    }
}

inline void drawSextant(Tile& tile, char32_t cp) {
    int mask = (int)(cp - 0x1FB00) + 1;
    if (mask >= 21) ++mask;
    if (mask >= 42) ++mask;
    int mx = eighth(tile.w, 4);
    for (int row = 0; row < 3; ++row) {
        int y0 = (tile.h * row + 1) / 3, y1 = (tile.h * (row + 1) + 1) / 3;
        if (mask >> (2 * row) & 1) tile.fill(0, y0, mx, y1);
        if (mask >> (2 * row + 1) & 1) tile.fill(mx, y0, tile.w, y1);
    }
}

static const char ARMS[] =
    "1100" "2200" "0011" "0022" "0000" "0000" "0000" "0000" "0000" "0000" "0000" "0000" "0101" "0201" "0102" "0202"
    "1001" "2001" "1002" "2002" "0110" "0210" "0120" "0220" "1010" "2010" "1020" "2020" "0111" "0211" "0121" "0112"
    "0122" "0221" "0212" "0222" "1011" "2011" "1021" "1012" "1022" "2021" "2012" "2022" "1101" "2101" "1201" "2201"
    "1102" "2102" "1202" "2202" "1110" "2110" "1210" "2210" "1120" "2120" "1220" "2220" "1111" "2111" "1211" "2211"
    "1121" "1112" "1122" "2121" "1221" "2112" "1212" "2221" "2212" "2122" "1222" "2222" "0000" "0000" "0000" "0000"
    "3300" "0033" "0301" "0103" "0303" "3001" "1003" "3003" "0310" "0130" "0330" "3010" "1030" "3030" "0311" "0133"
    "0333" "3011" "1033" "3033" "3301" "1103" "3303" "3310" "1130" "3330" "3311" "1133" "3333" "0000" "0000" "0000"
    "0000" "0000" "0000" "0000" "1000" "0010" "0100" "0001" "2000" "0020" "0200" "0002" "1200" "0012" "2100" "0021";
static_assert(sizeof(ARMS) == 128 * 4 + 1, "one LRUD entry per box-drawing code point");

inline void drawBoxLines(Tile& tile, char32_t cp) {
    int i = (int)(cp - 0x2500);
    if (i >= 0x04 && i <= 0x0B) drawDashes(tile, i < 0x08 ? 3 : 4, (i & 1) + 1, (i & 2) != 0);
    else if (i >= 0x4C && i <= 0x4F) drawDashes(tile, 2, (i & 1) + 1, (i & 2) != 0);
    else if (i >= 0x6D && i <= 0x70) drawArc(tile, i == 0x6D || i == 0x70 ? 1 : -1, i <= 0x6E ? 1 : -1);
    else if (i >= 0x71 && i <= 0x73) drawDiagonals(tile, i != 0x72, i != 0x71);
    else {
        const char* a = ARMS + 4 * i;
        drawArms(tile, a[0] - '0', a[1] - '0', a[2] - '0', a[3] - '0');
    }
}

inline void draw(char32_t cp, int w, int h, uint8_t* alpha) {
    Tile tile(w, h, alpha);
    if (cp <= 0x257F) drawBoxLines(tile, cp);
    else if (cp <= 0x259F) drawBlock(tile, cp);
    else if (cp <= 0xE0B3) drawPowerline(tile, cp >= 0xE0B2, (cp & 1) == 0);
    else drawSextant(tile, cp);
}

}
