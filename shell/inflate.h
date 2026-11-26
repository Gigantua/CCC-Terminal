#pragma once
#include <cstdint>
#include <string>

namespace inflate {

class Bits {
public:
    Bits(const uint8_t* data, size_t n) : p(data), end(data + n) {}
    bool failed = false;

    uint32_t take(int n) {
        while (count < n) {
            if (p >= end) { failed = true; return 0; }
            acc |= (uint64_t)*p++ << count;
            count += 8;
        }
        uint32_t v = (uint32_t)(acc & ((1ull << n) - 1));
        acc >>= n;
        count -= n;
        return v;
    }

    void alignToByte() { acc >>= count & 7; count -= count & 7; }

    bool copyBytes(std::string& out, size_t n) {
        while (n && count >= 8) { out += (char)(acc & 255); acc >>= 8; count -= 8; --n; }
        if ((size_t)(end - p) < n) { failed = true; return false; }
        out.append(reinterpret_cast<const char*>(p), n);
        p += n;
        return true;
    }

private:
    const uint8_t* p;
    const uint8_t* end;
    uint64_t acc = 0;
    int count = 0;
};

struct Huffman {
    uint16_t count[16];
    uint16_t symbol[320];

    bool build(const uint8_t* lengths, int n) {
        std::fill(count, count + 16, 0);
        for (int i = 0; i < n; ++i) ++count[lengths[i]];
        int left = 1;
        for (int len = 1; len < 16; ++len) {
            left = (left << 1) - count[len];
            if (left < 0) return false;
        }
        uint16_t offset[16] = {};
        for (int len = 1; len < 15; ++len) offset[len + 1] = (uint16_t)(offset[len] + count[len]);
        for (int s = 0; s < n; ++s)
            if (lengths[s]) symbol[offset[lengths[s]]++] = (uint16_t)s;
        return true;
    }

    int decode(Bits& in) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= (int)in.take(1);
            int n = count[len];
            if (code - n < first) return symbol[index + (code - first)];
            index += n;
            first = (first + n) << 1;
            code <<= 1;
        }
        return -1;
    }
};

inline bool codes(Bits& in, std::string& out, const Huffman& lit, const Huffman& dist, size_t limit) {
    static const uint16_t lengthBase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
    static const uint8_t lengthExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
    static const uint16_t distBase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
    static const uint8_t distExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
    for (;;) {
        int s = lit.decode(in);
        if (s < 0 || in.failed) return false;
        if (s < 256) { out += (char)s; if (out.size() > limit) return false; continue; }
        if (s == 256) return true;
        s -= 257;
        if (s >= 29) return false;
        size_t len = lengthBase[s] + in.take(lengthExtra[s]);
        int d = dist.decode(in);
        if (d < 0 || d >= 30) return false;
        size_t back = distBase[d] + in.take(distExtra[d]);
        if (in.failed || back > out.size() || out.size() + len > limit) return false;
        size_t from = out.size() - back;
        for (size_t k = 0; k < len; ++k) out += out[from + k];
    }
}

inline bool fixedBlock(Bits& in, std::string& out, size_t limit) {
    static const struct Tables { Huffman lit, dist; Tables() {
        uint8_t l[288];
        for (int i = 0; i < 288; ++i) l[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
        lit.build(l, 288);
        uint8_t d[30];
        std::fill(d, d + 30, 5);
        dist.build(d, 30);
    } } tables;
    return codes(in, out, tables.lit, tables.dist, limit);
}

inline bool dynamicBlock(Bits& in, std::string& out, size_t limit) {
    static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    int nlen = (int)in.take(5) + 257, ndist = (int)in.take(5) + 1, ncode = (int)in.take(4) + 4;
    if (nlen > 286 || ndist > 30) return false;
    uint8_t lengths[320] = {};
    for (int i = 0; i < ncode; ++i) lengths[order[i]] = (uint8_t)in.take(3);
    Huffman lencode;
    if (!lencode.build(lengths, 19)) return false;
    int i = 0;
    while (i < nlen + ndist) {
        int s = lencode.decode(in);
        if (s < 0 || in.failed) return false;
        if (s < 16) { lengths[i++] = (uint8_t)s; continue; }
        uint8_t value = 0;
        int repeat;
        if (s == 16) { if (!i) return false; value = lengths[i - 1]; repeat = 3 + (int)in.take(2); }
        else if (s == 17) repeat = 3 + (int)in.take(3);
        else repeat = 11 + (int)in.take(7);
        if (i + repeat > nlen + ndist) return false;
        while (repeat--) lengths[i++] = value;
    }
    Huffman lit, dist;
    if (!lit.build(lengths, nlen) || !dist.build(lengths + nlen, ndist)) return false;
    return codes(in, out, lit, dist, limit);
}

inline bool deflate(Bits& in, std::string& out, size_t limit) {
    for (;;) {
        bool last = in.take(1) != 0;
        uint32_t type = in.take(2);
        bool ok;
        if (type == 0) {
            in.alignToByte();
            uint32_t len = in.take(16), check = in.take(16);
            ok = !in.failed && (len ^ 0xFFFF) == check && out.size() + len <= limit && in.copyBytes(out, len);
        }
        else if (type == 1) ok = fixedBlock(in, out, limit);
        else if (type == 2) ok = dynamicBlock(in, out, limit);
        else ok = false;
        if (!ok || in.failed) return false;
        if (last) return true;
    }
}

inline bool zlib(const uint8_t* data, size_t n, std::string& out, size_t limit) {
    if (n < 2 || (data[0] & 15) != 8 || ((data[0] << 8) | data[1]) % 31 != 0 || (data[1] & 0x20)) return false;
    Bits in(data + 2, n - 2);
    return deflate(in, out, limit);
}

}
