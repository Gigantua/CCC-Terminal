#pragma once
#include "os.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <string>
#include <vector>

namespace ccc {

inline std::string upper(std::string s) { for (char& c : s) c = (char)toupper((unsigned char)c); return s; }
inline std::string lower(std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; }
inline int lowerAscii(char c) { return c >= 'A' && c <= 'Z' ? c + 32 : (unsigned char)c; }
inline int compareNoCase(std::string_view a, std::string_view b) {
    auto [x, y] = std::mismatch(a.begin(), a.end(), b.begin(), b.end(), [](char p, char q) { return lowerAscii(p) == lowerAscii(q); });
    if (x != a.end() && y != b.end()) return lowerAscii(*x) - lowerAscii(*y);
    return a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
}
inline bool equalNoCase(const char* a, const char* b, size_t n) { return std::equal(a, a + n, b, [](char p, char q) { return lowerAscii(p) == lowerAscii(q); }); }
inline bool ieq(std::string_view a, std::string_view b) { return a.size() == b.size() && equalNoCase(a.data(), b.data(), a.size()); }
inline bool isDelim(char c) { return c == ' ' || c == '\t' || c == ',' || c == ';' || c == '='; }
inline bool isAsciiAlpha(char c) { return (unsigned char)((c | 0x20) - 'a') < 26; }
inline bool isSlash(char c) { return std::string_view(os::SLASHES).find(c) != std::string_view::npos; }
inline const char* findChar(const char* p, const char* e, char c) {
    if (e - p > 64) return static_cast<const char*>(std::memchr(p, c, e - p));
    for (; p < e; ++p) if (*p == c) return p;
    return nullptr;
}
inline bool contains(const std::string& s, char c) { return findChar(s.data(), s.data() + s.size(), c) != nullptr; }

class DecimalCounter {
public:
    explicit DecimalCounter(long long start) : value(start) { render(); }
    std::string_view text() const { return std::string_view(digits + begin, sizeof(digits) - begin); }
    long long current() const { return value; }
    void add(long long step) {
        value += step;
        if (step > 0 && step < 10 && value - step >= 0) addSmall((int)step);
        else render();
    }
private:
    char digits[24];
    size_t begin = sizeof(digits);
    long long value;
    void render() {
        char tmp[24];
        size_t n = (size_t)(std::to_chars(tmp, tmp + sizeof(tmp), value).ptr - tmp);
        begin = sizeof(digits) - n;
        std::memcpy(digits + begin, tmp, n);
    }
    void addSmall(int step) {
        size_t k = sizeof(digits) - 1;
        int carry = step;
        for (;;) {
            int d = digits[k] - '0' + carry;
            digits[k] = (char)('0' + d % 10);
            carry = d / 10;
            if (!carry) return;
            if (k == begin) { digits[--begin] = (char)('0' + carry); return; }
            --k;
        }
    }
};
inline bool isBlank(const std::string& s) { return s.find_first_not_of(" \t") == std::string::npos; }
inline std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a, s.find_last_not_of(" \t") + 1 - a);
}
inline std::string argTail(const std::string& s) {
    size_t a = s.find_first_not_of(" \t,;=");
    return a == std::string::npos ? std::string() : s.substr(a, s.find_last_not_of(" \t") + 1 - a);
}
inline size_t tokenEnd(const std::string& s, size_t i = 0) {
    for (bool q = false; i < s.size() && (q || (s[i] != ' ' && s[i] != '\t')); ++i) if (s[i] == '"') q = !q;
    return i;
}
inline std::string crlf(std::string s) {
    for (size_t k = 0; (k = s.find('\n', k)) != std::string::npos; k += 2) s.insert(k, "\r");
    return s;
}

inline uint32_t attrsOf(const std::string& p) { return os::attributes(p); }
inline bool exists(const std::string& p) { return attrsOf(p) != os::ATTR_INVALID; }
inline bool isDir(const std::string& p) { uint32_t a = attrsOf(p); return a != os::ATTR_INVALID && (a & os::ATTR_DIRECTORY); }
inline bool isFile(const std::string& p) { uint32_t a = attrsOf(p); return a != os::ATTR_INVALID && !(a & os::ATTR_DIRECTORY); }
inline std::string afterSlash(const std::string& path) { return path.substr(path.find_last_of(os::SLASH) + 1); }
inline std::string withSlash(const std::string& d) { return d.empty() || isSlash(d.back()) ? d : d + os::SLASH; }
inline std::string upperExtension(const std::string& path) { return upper(path.substr((std::min)(path.size(), path.rfind('.')))); }
inline bool isBatchExtension(const std::string& upperExt) { return upperExt == ".BAT" || upperExt == ".CMD"; }

inline std::filesystem::path fsPath(const std::string& s) { return std::u8string_view(reinterpret_cast<const char8_t*>(s.data()), s.size()); }

inline uint64_t freeSpace(const std::string& path) {
    std::error_code ec;
    auto s = std::filesystem::space(fsPath(path), ec);
    return ec ? 0 : (uint64_t)s.available;
}

inline bool readFile(const std::string& path, std::string& out) {
    os::Handle h = os::openRead(path);
    if (h == os::NO_HANDLE) return false;
    out.clear();
    char buf[1 << 16];
    while (size_t n = os::read(h, buf, sizeof buf)) out.append(buf, n);
    os::close(h);
    return true;
}

inline std::string unquote(std::string s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return s.substr(1, s.size() - 2);
    if (!s.empty() && s.front() == '"') return s.substr(1);
    return s;
}
inline std::vector<std::string> splitArgs(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && isDelim(s[i])) ++i;
        if (i >= s.size()) break;
        std::string t;
        bool q = false;
        while (i < s.size() && (q || !isDelim(s[i]))) { if (s[i] == '"') q = !q; t += s[i++]; }
        out.push_back(t);
    }
    return out;
}
inline std::string fmtGroup(unsigned long long v) {
    static const std::string separator = os::digitGroupSeparator();
    std::string d = std::to_string(v), o;
    for (size_t i = 0; i < d.size(); ++i) { if (i && (d.size() - i) % 3 == 0) o += separator; o += d[i]; }
    return o;
}
inline std::string pad(const std::string& s, size_t w) { return s.size() >= w ? s : std::string(w - s.size(), ' ') + s; }

}
