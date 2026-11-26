#pragma once
#include <cstdint>
#include <string>

namespace base64 {

inline int value(uint32_t ch) {
    return ch >= 'A' && ch <= 'Z' ? (int)(ch - 'A') : ch >= 'a' && ch <= 'z' ? (int)(ch - 'a' + 26) : ch >= '0' && ch <= '9' ? (int)(ch - '0' + 52) : ch == '+' ? 62 : ch == '/' ? 63 : -1;
}

inline std::string decode(const std::string& s) {
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    out.reserve(s.size() * 3 / 4);
    for (unsigned char ch : s) {
        int v = value(ch);
        if (v < 0) continue;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out += (char)(acc >> bits & 255); }
    }
    return out;
}

inline std::string encode(const std::string& s) {
    static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((s.size() + 2) / 3 * 4);
    for (size_t i = 0, n = s.size(); i < n; i += 3) {
        uint32_t v = (uint32_t)(unsigned char)s[i] << 16 | (i + 1 < n ? (uint32_t)(unsigned char)s[i + 1] << 8 : 0) | (i + 2 < n ? (unsigned char)s[i + 2] : 0);
        out += digits[v >> 18];
        out += digits[v >> 12 & 63];
        out += i + 1 < n ? digits[v >> 6 & 63] : '=';
        out += i + 2 < n ? digits[v & 63] : '=';
    }
    return out;
}

}
