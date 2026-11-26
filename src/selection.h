#pragma once
#include <algorithm>
#include <cstring>
#include <string>
#include "../shell/console.h"

namespace cells {

inline bool wrapped(const Console::Frame& f, int row) { return row >= 0 && row < (int)f.wrapped.size() && f.wrapped[row]; }

inline std::string text(const Console::Frame& f, int from, int to) {
    int cols = f.cols;
    std::string out, line;
    int lastRow = (to - 1) / cols;
    for (int row = from / cols; row <= lastRow && from < to; ++row) {
        int a = row == from / cols ? from % cols : 0, b = row == lastRow ? (to - 1) % cols + 1 : cols;
        for (int k = a; k < b; ++k) line += f.text(f.cells[(size_t)row * cols + k]);
        if (row < lastRow && wrapped(f, row)) continue;
        while (!line.empty() && line.back() == ' ') line.pop_back();
        out += line;
        if (row < lastRow) out += "\r\n";
        line.clear();
    }
    return out;
}

inline bool urlChar(uint32_t c) { return c > 0x20 && c < 0x7F && !std::strchr("\"'<>`{}|\\^", (int)c); }

inline uint16_t hyperlink(const Console::Frame& f, int cell, int& from, int& to) {
    int n = (std::min)(f.cols * f.rows, (int)f.attrs.size());
    if (cell < 0 || cell >= n || !f.attrs[cell].link) return 0;
    uint16_t id = f.attrs[cell].link;
    int a = cell, b = cell + 1;
    while (a > 0 && f.attrs[a - 1].link == id) --a;
    while (b < n && f.attrs[b].link == id) ++b;
    from = a;
    to = b;
    return id;
}

inline bool startsWithIgnoringCase(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), s.begin(), [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });
}

inline bool openable(const std::string& uri) {
    static const char* schemes[] = { "https://", "http://", "file://", "ftp://", "mailto:" };
    return std::any_of(std::begin(schemes), std::end(schemes), [&](const char* sc) { return uri.size() > std::strlen(sc) && startsWithIgnoringCase(uri, sc); });
}

inline bool link(const Console::Frame& f, int cell, int& from, int& to) {
    if (hyperlink(f, cell, from, to)) return true;
    int n = f.cols * f.rows;
    if (cell < 0 || cell >= n || !urlChar(f.cells[cell])) return false;
    int a = cell, b = cell + 1;
    while (a > 0 && urlChar(f.cells[a - 1])) --a;
    while (b < n && urlChar(f.cells[b])) ++b;
    std::string s;
    for (int k = a; k < b; ++k) s += (char)f.cells[k];
    static const char* schemes[] = { "https://", "http://", "file://", "ftp://", "mailto:" };
    size_t start = std::string::npos;
    for (const char* sc : schemes)
        for (size_t p = s.find(sc); p != std::string::npos && (int)p <= cell - a; p = s.find(sc, p + 1))
            if (start == std::string::npos || p > start) start = p;
    if (start == std::string::npos) return false;
    size_t end = s.size();
    auto balanced = [&](char open, char close) {
        return s[end - 1] == close && std::count(s.begin() + start, s.begin() + end, open) >= std::count(s.begin() + start, s.begin() + end, close);
    };
    while (end > start && std::strchr(".,;:!?)]", s[end - 1]) && !balanced('(', ')') && !balanced('[', ']')) --end;
    if (end - start <= std::strlen("http://") || cell - a >= (int)end) return false;
    from = a + (int)start;
    to = a + (int)end;
    return true;
}

inline std::string ascii(const Console::Frame& f, int from, int to) {
    std::string s;
    for (int k = from; k < to; ++k) s += (char)f.cells[k];
    return s;
}

inline bool wordChar(uint32_t c) { return c != ' ' && c != '\t' && !std::strchr("\"'`()[]{}<>|,;", (int)(c < 0x80 ? c : 'a')); }

inline bool word(const Console::Frame& f, int cell, int& from, int& last) {
    int to;
    if (link(f, cell, from, to)) { last = to - 1; return true; }
    if (!wordChar(f.cells[cell])) return false;
    auto joined = [&](int a) { return a / f.cols == (a + 1) / f.cols || wrapped(f, a / f.cols); };
    int n = f.cols * f.rows, a = cell, b = cell;
    while (a > 0 && joined(a - 1) && wordChar(f.cells[a - 1])) --a;
    while (b + 1 < n && joined(b) && wordChar(f.cells[b + 1])) ++b;
    while (b > cell && (f.cells[b] == '.' || f.cells[b] == ':')) --b;
    from = a;
    last = b;
    return true;
}

inline void line(const Console::Frame& f, int cell, int& from, int& last) {
    int cols = f.cols, top = cell / cols, bottom = top;
    while (top > 0 && wrapped(f, top - 1)) --top;
    while (bottom + 1 < f.rows && wrapped(f, bottom)) ++bottom;
    last = bottom * cols + cols - 1;
    while (last > top * cols && f.cells[last] == ' ') --last;
    from = top * cols;
}

}

struct Selection {
    int anchor = 0, end = 0, cols = 0;
    bool shown = false, dragging = false;

    bool valid(const Console::Frame& f) const { return shown && cols == f.cols; }
    int first() const { return (std::min)(anchor, end); }
    int last() const { return (std::max)(anchor, end); }

    bool visible(const Console::Frame& f, int& from, int& to) const {
        if (!valid(f)) return false;
        from = (std::max)(first(), 0);
        to = (std::min)(last() + 1, f.cols * f.rows);
        return from < to;
    }

    void set(int a, int b, int frameCols) {
        anchor = a;
        end = b;
        cols = frameCols;
        shown = true;
        dragging = false;
    }

    void start(int cell, int frameCols) {
        anchor = end = cell;
        cols = frameCols;
        dragging = true;
        shown = false;
    }

    void extend(int cell) {
        end = cell;
        if (end != anchor) shown = true;
    }

    void shift(int rows) {
        anchor -= rows * cols;
        end -= rows * cols;
    }
};
