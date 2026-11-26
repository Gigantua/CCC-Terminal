#pragma once
#include <cstdint>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "inflate.h"

namespace png {

struct Picture {
    int width = 0, height = 0;
    std::vector<uint32_t> pixels;
};

inline uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

inline uint8_t paeth(int a, int b, int c) {
    int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return (uint8_t)(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

inline bool unfilter(uint8_t* rows, size_t stride, int height, int bpp) {
    std::vector<uint8_t> prior(stride, 0);
    for (int y = 0; y < height; ++y) {
        uint8_t* row = rows + (size_t)y * (stride + 1);
        uint8_t type = row[0];
        uint8_t* cur = row + 1;
        for (size_t i = 0; i < stride; ++i) {
            int a = i >= (size_t)bpp ? cur[i - bpp] : 0, b = prior[i], c = i >= (size_t)bpp ? prior[i - bpp] : 0;
            switch (type) {
            case 0: break;
            case 1: cur[i] = (uint8_t)(cur[i] + a); break;
            case 2: cur[i] = (uint8_t)(cur[i] + b); break;
            case 3: cur[i] = (uint8_t)(cur[i] + (a + b) / 2); break;
            case 4: cur[i] = (uint8_t)(cur[i] + paeth(a, b, c)); break;
            default: return false;
            }
        }
        std::copy(cur, cur + stride, prior.begin());
    }
    return true;
}

struct Format {
    int depth = 0, type = 0;
    const uint8_t* plte = nullptr;
    size_t plteSize = 0;
    const uint8_t* trns = nullptr;
    size_t trnsSize = 0;

    int channels() const { return type == 0 ? 1 : type == 2 ? 3 : type == 3 ? 1 : type == 4 ? 2 : 4; }

    uint32_t raw(const uint8_t* row, int x, int c) const {
        size_t bit = ((size_t)x * channels() + c) * depth;
        if (depth == 16) return (uint32_t)row[bit / 8] << 8 | row[bit / 8 + 1];
        if (depth == 8) return row[bit / 8];
        return (uint32_t)(row[bit / 8] >> (8 - depth - bit % 8)) & ((1u << depth) - 1);
    }

    uint32_t to8(uint32_t v) const { return depth == 16 ? v >> 8 : depth == 8 ? v : v * 255 / ((1u << depth) - 1); }

    uint32_t key(int i) const { return (uint32_t)trns[i * 2] << 8 | trns[i * 2 + 1]; }

    uint32_t pixel(const uint8_t* row, int x) const {
        auto rgba = [](uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | g << 8 | b << 16 | a << 24; };
        switch (type) {
        case 0: {
            uint32_t g = raw(row, x, 0);
            return rgba(to8(g), to8(g), to8(g), trnsSize >= 2 && g == key(0) ? 0 : 255);
        }
        case 2: {
            uint32_t r = raw(row, x, 0), g = raw(row, x, 1), b = raw(row, x, 2);
            return rgba(to8(r), to8(g), to8(b), trnsSize >= 6 && r == key(0) && g == key(1) && b == key(2) ? 0 : 255);
        }
        case 3: {
            uint32_t i = raw(row, x, 0);
            if (i * 3 + 2 >= plteSize) return 0;
            return rgba(plte[i * 3], plte[i * 3 + 1], plte[i * 3 + 2], i < trnsSize ? trns[i] : 255);
        }
        case 4: { uint32_t g = to8(raw(row, x, 0)); return rgba(g, g, g, to8(raw(row, x, 1))); }
        default: return rgba(to8(raw(row, x, 0)), to8(raw(row, x, 1)), to8(raw(row, x, 2)), to8(raw(row, x, 3)));
        }
    }
};

inline bool decode(const std::string& file, Picture& out, size_t maxPixels) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(file.data());
    size_t n = file.size();
    static const uint8_t signature[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (n < 8 || !std::equal(signature, signature + 8, p)) return false;
    Format f;
    int width = 0, height = 0, interlace = 0;
    std::string idat;
    for (size_t at = 8; at + 12 <= n;) {
        uint32_t len = be32(p + at);
        if (len > n - at - 12) return false;
        const uint8_t* kind = p + at + 4;
        const uint8_t* body = p + at + 8;
        if (!std::memcmp(kind, "IHDR", 4) && len >= 13) {
            width = (int)be32(body);
            height = (int)be32(body + 4);
            f.depth = body[8];
            f.type = body[9];
            interlace = body[12];
        }
        else if (!std::memcmp(kind, "PLTE", 4)) { f.plte = body; f.plteSize = len; }
        else if (!std::memcmp(kind, "tRNS", 4)) { f.trns = body; f.trnsSize = len; }
        else if (!std::memcmp(kind, "IDAT", 4)) idat.append(reinterpret_cast<const char*>(body), len);
        else if (!std::memcmp(kind, "IEND", 4)) break;
        at += 12 + (size_t)len;
    }
    bool depthOk = f.depth == 8 || f.depth == 16 || ((f.type == 0 || f.type == 3) && (f.depth == 1 || f.depth == 2 || f.depth == 4));
    if (width <= 0 || height <= 0 || (size_t)width * height > maxPixels || !depthOk || f.type == 1 || f.type == 5 || f.type > 6) return false;
    int bits = f.channels() * f.depth, bpp = (std::max)(1, bits / 8);
    static const int startX[8] = { 0, 4, 0, 2, 0, 1, 0, 0 }, startY[8] = { 0, 0, 4, 0, 2, 0, 1, 0 }, stepX[8] = { 8, 8, 4, 4, 2, 2, 1, 1 }, stepY[8] = { 8, 8, 8, 4, 4, 2, 2, 1 };
    int first = interlace ? 0 : 7, last = interlace ? 7 : 8;
    auto passW = [&](int k) { return (width - startX[k] + stepX[k] - 1) / stepX[k]; };
    auto passH = [&](int k) { return (height - startY[k] + stepY[k] - 1) / stepY[k]; };
    size_t expected = 0;
    for (int k = first; k < last; ++k)
        if (passW(k) > 0 && passH(k) > 0) expected += (size_t)passH(k) * (((size_t)passW(k) * bits + 7) / 8 + 1);
    std::string raw;
    if (!inflate::zlib(reinterpret_cast<const uint8_t*>(idat.data()), idat.size(), raw, expected) || raw.size() < expected) return false;
    out.width = width;
    out.height = height;
    out.pixels.assign((size_t)width * height, 0);
    uint8_t* data = reinterpret_cast<uint8_t*>(raw.data());
    for (int k = first; k < last; ++k) {
        int pw = passW(k), ph = passH(k);
        if (pw <= 0 || ph <= 0) continue;
        size_t stride = ((size_t)pw * bits + 7) / 8;
        if (!unfilter(data, stride, ph, bpp)) return false;
        for (int y = 0; y < ph; ++y) {
            const uint8_t* row = data + (size_t)y * (stride + 1) + 1;
            for (int x = 0; x < pw; ++x) out.pixels[(size_t)(startY[k] + y * stepY[k]) * width + startX[k] + x * stepX[k]] = f.pixel(row, x);
        }
        data += (size_t)ph * (stride + 1);
    }
    return true;
}

}
