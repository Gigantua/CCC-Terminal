#pragma once
#include <functional>
#include "console.h"
#include "base64.h"
#include "graphics.h"
#include "clipboard_transfer.h"
#include "cursors.h"
#include "palette.h"
#include "pointer.h"

namespace ccc {

struct Cell {
    uint32_t ch;
    vt::Attr attr;
};

class Grid {
public:
    int cols = 0, rows = 0;

    void reset(int c, int r, const Cell& blank) {
        cols = c;
        rows = r;
        cells.assign((size_t)c * r, blank);
        order.resize(r);
        for (int y = 0; y < r; ++y) order[y] = y;
        wraps.assign(r, 0);
        marks.assign(r, 0);
    }

    void resize(int c, int r, const Cell& blank) {
        Grid next;
        next.reset(c, r, blank);
        for (int y = 0; y < (std::min)(r, rows); ++y) {
            std::copy(row(y), row(y) + (std::min)(c, cols), next.row(y));
            next.wrap(y) = wrap(y);
            next.mark(y) = mark(y);
            if (c < cols && c > 0 && next.row(y)[c - 1].ch != Console::WIDE_TAIL && row(y)[c].ch == Console::WIDE_TAIL) next.row(y)[c - 1].ch = ' ';
        }
        *this = std::move(next);
    }

    Cell* row(int y) { return &cells[(size_t)order[y] * cols]; }
    const Cell* row(int y) const { return &cells[(size_t)order[y] * cols]; }
    uint8_t& wrap(int y) { return wraps[order[y]]; }
    uint8_t wrap(int y) const { return wraps[order[y]]; }
    uint8_t& mark(int y) { return marks[order[y]]; }
    uint8_t mark(int y) const { return marks[order[y]]; }

    void clearRows(int from, int to, const Cell& blank) {
        for (int y = from; y < to; ++y) { std::fill(row(y), row(y) + cols, blank); wrap(y) = 0; mark(y) = 0; }
    }

    void fill(int y, int from, int to, const Cell& blank) {
        if (from < 0) from = 0;
        if (to > cols) to = cols;
        if (from >= to) return;
        Cell* r = row(y);
        detachLeft(r, from);
        detachRight(r, to);
        std::fill(r + from, r + to, blank);
    }

    static void detachLeft(Cell* r, int x) {
        if (r[x].ch != Console::WIDE_TAIL) return;
        for (int k = x - 1; k >= 0; --k) {
            bool head = r[k].ch != Console::WIDE_TAIL;
            r[k].ch = ' ';
            if (head) return;
        }
    }

    void detachRight(Cell* r, int x) const { for (int k = x; k < cols && r[k].ch == Console::WIDE_TAIL; ++k) r[k].ch = ' '; }

    void rotateUp(int top, int bottom, int n) { std::rotate(order.begin() + top, order.begin() + top + n, order.begin() + bottom + 1); }
    void rotateDown(int top, int bottom, int n) { std::rotate(order.begin() + top, order.begin() + bottom + 1 - n, order.begin() + bottom + 1); }

private:
    std::vector<Cell> cells;
    std::vector<int> order;
    std::vector<uint8_t> wraps, marks;
};

inline size_t printableRun(const unsigned char* s, size_t n) {
    size_t i = 0;
    const simd::U8x16 base = simd::U8x16::splat(0x20), limit = simd::U8x16::splat(0x5F);
    for (; i + 16 <= n; i += 16)
        if (simd::Mask16 bad = atLeast(simd::U8x16::load(s + i) - base, limit).mask()) return i + bad.first();
    while (i < n && s[i] >= 0x20 && s[i] < 0x7F) ++i;
    return i;
}

inline size_t printableRun(const char16_t* s, size_t n) {
    size_t i = 0;
    const simd::U16x8 base = simd::U16x8::splat(0x20), limit = simd::U16x8::splat(0x5F);
    for (; i + 8 <= n; i += 8)
        if (unsigned bad = atLeast(simd::U16x8::load(s + i) - base, limit).firstSet(); bad < 8) return i + bad;
    while (i < n && s[i] >= 0x20 && s[i] < 0x7F) ++i;
    return i;
}

class Emulator {
public:
    struct Cursor {
        int x = 0, y = 0;
        bool pending = false, origin = false, lineDrawing = false;
        vt::Attr attr;
    };

    Grid main, alt;
    Grid* g = &main;
    Cursor cur;
    bool altActive = false;
    bool logging = false;
    int logRow = 0, logCol = 0;
    int top = 0, bottom = 0;
    bool autowrap = true, wrapMode = true, newlineReturns = true, insert = false;
    int modifyOtherKeys = 0;
    struct Kitty { int flags = 0; std::vector<int> stack; };
    static constexpr int KITTY_ALL = 31;
    static constexpr size_t KITTY_DEPTH = 64;
    Kitty kitty[2];
    int kittyFlags() const { return kitty[screen()].flags; }
    bool cursorVisible = true, appCursor = false, bracketedPaste = false, focusEvents = false, sync = false;
    int mouse = 0;
    bool mouseSgr = false;
    int cursorShape = 0;
    bool inBandResize = false, mousePixels = false, colorSchemeReports = false;
    int cellW = 0, cellH = 0;
    struct Colors {
        palette::Table table = palette::standardTable();
        uint32_t fg = vt::COLOR_DEFAULT, bg = vt::COLOR_DEFAULT, cursor = vt::COLOR_DEFAULT;
        bool changed = false;
    };
    Colors colors;
    static constexpr uint32_t defaultFg = 0xCCCCCC, defaultBg = 0x0C0C0C;
    std::string pointer;
    std::vector<std::u32string> clusters;
    ClipboardTransfer clipboard;
    ExtraCursors cursors;

    std::function<void(const char*, size_t)> onScrollOut;
    std::function<void(const std::string&)> onReply;
    std::function<void(const std::wstring&)> onTitle;
    std::function<void()> onBell;
    std::function<void(int, int)> onProgress;
    std::function<void(const std::string&)> onClipboard;
    std::function<void()> onClipboardRead;
    std::function<void()> onClear;
    std::function<void(const std::string&)> onCwd;
    std::function<void(const std::string&, const std::string&)> onNotify;

    void setCellSize(int w, int h) {
        if (w == cellW && h == cellH) return;
        cellW = w;
        cellH = h;
        reportSize();
    }

    void reportSize() {
        if (!inBandResize) return;
        reply("\x1b[48;" + std::to_string(rows()) + ";" + std::to_string(cols()) + ";" + std::to_string(rows() * cellH) + ";" + std::to_string(cols() * cellW) + "t");
    }

    uint32_t effectiveBg() const { return colors.bg & vt::COLOR_DEFAULT ? defaultBg : colors.bg; }
    bool dark() const { return palette::luminance(effectiveBg()) < 128; }
    void reportColorScheme() { if (colorSchemeReports) reply(std::string("\x1b[?997;") + (dark() ? "1" : "2") + "n"); }

    void reset(int c, int r) {
        cur = Cursor();
        saved = Cursor();
        mainSaved = Cursor();
        main.reset(c, r, blankCell());
        alt.reset(c, r, blankCell());
        g = &main;
        altActive = false;
        resetState(r);
        state = GROUND;
        utfNeed = 0;
        logRow = logCol = 0;
    }

    int cols() const { return g->cols; }
    int rows() const { return g->rows; }

    void resize(int c, int r) {
        if (c == main.cols && r == main.rows) return;
        Cursor& mainCursor = altActive ? mainSaved : cur;
        int excess = mainCursor.y - (r - 1);
        if (excess > 0) {
            if (logging) for (int y = 0; y < excess; ++y) scrollOutRow(y);
            main.rotateUp(0, main.rows - 1, excess);
            mainCursor.y -= excess;
            if (logging) logRow = (std::max)(0, logRow - excess);
        }
        if (altActive && cur.y > r - 1) {
            int k = cur.y - (r - 1);
            alt.rotateUp(0, alt.rows - 1, k);
            cur.y -= k;
        }
        main.resize(c, r, blankCell());
        alt.resize(c, r, blankCell());
        cur.x = (std::min)(cur.x, c - 1);
        cur.y = (std::min)(cur.y, r - 1);
        cur.pending = false;
        top = 0;
        bottom = r - 1;
        if (logRow >= r) { logRow = r - 1; logCol = 0; }
        cursors.clear();
        reportSize();
    }

    void writeBytes(const char* p, size_t n, unsigned page) {
        utf8 = page == os::UTF8_PAGE;
        feed(reinterpret_cast<const unsigned char*>(p), n);
    }

    void writeWide(const char16_t* p, size_t n) { feed(p, n); }

    void answerClipboard(const std::string& text, bool granted) { clipboard.answer(text, granted); }

    bool announcePaste(const std::string& text) { return clipboard.announcePaste(text); }

    int animate() { return images.animate(); }

    bool autowrapOn() const { return autowrap && wrapMode; }

    void setCursor(int x, int y) {
        cur.x = std::clamp(x, 0, cols() - 1);
        cur.y = std::clamp(y, 0, rows() - 1);
        cur.pending = false;
    }

    void touched(int y, int x) {
        if (!logging || altActive || y > logRow) return;
        if (y < logRow || x < logCol) { logRow = y; logCol = 0; }
    }

    void putCell(int x, int y, uint32_t ch, const vt::Attr& a) {
        touched(y, x);
        Cell* r = g->row(y);
        Grid::detachLeft(r, x);
        g->detachRight(r, x + 1);
        r[x] = { ch, a };
    }

    Cell blankCell() const {
        vt::Attr a;
        a.fg = cur.attr.fg;
        a.bg = cur.attr.bg;
        a.flags = cur.attr.flags & vt::REVERSE;
        return { ' ', a };
    }

    void eraseRows(int from, int to) {
        for (int y = (std::max)(from, 0); y < (std::min)(to, rows()); ++y) {
            touched(y, 0);
            g->fill(y, 0, cols(), blankCell());
            g->wrap(y) = g->mark(y) = 0;
        }
    }

    void eraseInRow(int y, int from, int to) {
        touched(y, from);
        g->fill(y, from, to, blankCell());
    }

    std::string pendingText(int lastRow) const {
        std::string out;
        if (!logging) return out;
        vt::Attr running;
        for (int y = logRow; y <= lastRow && y < main.rows; ++y) {
            appendRow(out, main, y, y == logRow ? logCol : 0, running);
            if (y < lastRow && !main.wrap(y)) out += "\r\n";
        }
        closeRun(out, running);
        return out;
    }

    int lastUsedRow() const {
        int last = altActive ? mainSaved.y : cur.y;
        for (int y = main.rows - 1; y > last; --y) {
            const Cell* r = main.row(y);
            for (int x = 0; x < main.cols; ++x)
                if (r[x].ch != ' ' || r[x].attr != vt::Attr()) return y;
        }
        return last;
    }

    void snapshot(const Console::Frame& f) {
        cur = Cursor();
        for (int y = 0; y < (std::min)(f.rows, main.rows); ++y) {
            Cell* r = main.row(y);
            for (int x = 0; x < (std::min)(f.cols, main.cols); ++x) {
                uint32_t ch = f.cells[(size_t)y * f.cols + x];
                if (ch >= Console::CLUSTER_BASE && ch != Console::WIDE_TAIL) ch = intern(f.clusters[ch - Console::CLUSTER_BASE]);
                r[x] = { ch, f.attrs[(size_t)y * f.cols + x] };
            }
        }
        if (f.cursorRow >= 0) { cur.y = f.cursorRow; cur.x = f.cursorCol; }
        else { cur.y = main.rows - 1; cur.x = 0; }
        logRow = cur.y;
        logCol = cur.x;
    }

    void render(Console::Frame& f) const {
        f.cols = cols();
        f.rows = rows();
        size_t n = (size_t)f.cols * f.rows;
        f.cells.resize(n);
        f.attrs.resize(n);
        f.wrapped.resize((size_t)f.rows);
        f.blinking = false;
        for (int y = 0; y < f.rows; ++y) {
            const Cell* r = g->row(y);
            f.wrapped[y] = g->wrap(y);
            for (int x = 0; x < f.cols; ++x) {
                f.cells[(size_t)y * f.cols + x] = r[x].ch;
                f.attrs[(size_t)y * f.cols + x] = r[x].attr;
                if (r[x].attr.flags & vt::BLINK) f.blinking = true;
            }
        }
        f.clusters = clusters;
        f.cursorRow = cursorVisible ? cur.y : -1;
        f.cursorCol = cursorVisible ? cur.x : -1;
        f.thumbPos = 1;
        f.thumbSize = 1;
        f.pictures.clear();
        images.pictures(screen(), f.pictures);
        f.extraCursors.clear();
        for (auto [cell, shape] : cursors.shapes) f.extraCursors.push_back({ cell, shape });
        f.extraTextColor = cursors.textColor;
        f.extraCursorColor = cursors.cursorColor;
        images.placeholderPictures(screen(), rows(), cols(), (std::max)(cellW, 1), (std::max)(cellH, 1), [this](int x, int y, vt::Attr& a) {
            const Cell& c = g->row(y)[x];
            a = c.attr;
            return clusterOf(c.ch);
        }, f.pictures);
        f.palette.assign(colors.changed ? colors.table.begin() : colors.table.end(), colors.table.end());
        f.fg = colors.fg;
        f.bg = colors.bg;
        f.cursorColor = colors.cursor;
        f.pointer = pointer;
    }

    void scrollRegionUp(int t, int b, int n) {
        n = (std::min)(n, b - t + 1);
        if (n <= 0) return;
        if (t == 0 && logging && g == &main && !altActive) {
            for (int y = 0; y < n; ++y) scrollOutRow(y);
            logRow = (std::max)(0, logRow - n);
        }
        else touched(t, 0);
        g->rotateUp(t, b, n);
        g->clearRows(b - n + 1, b + 1, blankCell());
        images.scroll(screen(), t, b, n, t == 0 && b == rows() - 1);
    }

    void scrollRegionDown(int t, int b, int n) {
        n = (std::min)(n, b - t + 1);
        if (n <= 0) return;
        touched(t, 0);
        g->rotateDown(t, b, n);
        g->clearRows(t, t + n, blankCell());
        images.scroll(screen(), t, b, -n, t == 0 && b == rows() - 1);
    }

    int screen() const { return altActive ? 1 : 0; }

private:
    enum State : uint8_t { GROUND, ESCAPE, ESCAPE_INTER, CSI, OSC, OSC_ESC, STRING, STRING_ESC, APC, APC_ESC };
    graphics::Store images;
    std::string apc;
    State state = GROUND;
    Cursor saved, mainSaved;
    bool utf8 = false;
    int utfNeed = 0;
    uint32_t utfCode = 0;
    uint32_t lastPrinted = ' ';
    int lastX = -1, lastY = -1;
    char32_t intermediate = 0, marker = 0;
    int count = 0;
    uint32_t params[vt::MAX_PARAMS] = {};
    uint32_t sub = 0;
    bool paramStarted = false;
    std::u32string osc;
    struct StreamMark { size_t at, replayFrom; bool reset; };
    std::vector<StreamMark> marks;
    std::unordered_map<std::u32string, uint32_t> clusterIds;
    std::vector<Colors> colorStack;
    std::vector<std::wstring> titles;
    std::vector<std::string> pointerStack;
    bool outputMarkPending = false;
    std::wstring title;
    std::string notifyTitle, notifyBody;
    static constexpr size_t STACK_DEPTH = 10;

    uint32_t intern(const std::u32string& s) {
        auto it = clusterIds.find(s);
        if (it != clusterIds.end()) return it->second;
        if (clusters.size() >= 65536) { clusters.clear(); clusterIds.clear(); }
        uint32_t id = Console::CLUSTER_BASE + (uint32_t)clusters.size();
        clusters.push_back(s);
        clusterIds.emplace(s, id);
        return id;
    }

    std::u32string clusterOf(uint32_t id) const {
        if (id >= Console::CLUSTER_BASE && id != Console::WIDE_TAIL && id - Console::CLUSTER_BASE < clusters.size()) return clusters[id - Console::CLUSTER_BASE];
        return std::u32string(1, (char32_t)id);
    }

    static void appendSgr(std::string& out, const vt::Attr& a) {
        out += "\x1b[0";
        static const struct { uint16_t flag; const char* code; } flags[] = {
            { vt::BOLD, ";1" }, { vt::DIM, ";2" }, { vt::ITALIC, ";3" }, { vt::UNDERLINE, ";4" }, { vt::BLINK, ";5" },
            { vt::REVERSE, ";7" }, { vt::INVISIBLE, ";8" }, { vt::STRIKE, ";9" },
        };
        for (auto& f : flags) {
            if (!(a.flags & f.flag)) continue;
            out += f.code;
            if (f.flag == vt::UNDERLINE && a.underline() > vt::UNDERLINE_SINGLE) out += ":" + std::to_string(a.underline());
        }
        auto color = [&](uint32_t c, int base) {
            if (c & vt::COLOR_DEFAULT) return;
            if (c & vt::COLOR_INDEXED) {
                uint32_t i = c & 255;
                if (i < 8 && base < 50) out += ";" + std::to_string(base + i);
                else if (i < 16 && base < 50) out += ";" + std::to_string(base + 60 + i - 8);
                else out += ";" + std::to_string(base + 8) + ";5;" + std::to_string(i);
            }
            else out += ";" + std::to_string(base + 8) + ";2;" + std::to_string(c & 255) + ";" + std::to_string(c >> 8 & 255) + ";" + std::to_string(c >> 16 & 255);
        };
        color(a.fg, 30);
        color(a.bg, 40);
        color(a.underlineColor, 50);
        out += 'm';
    }

    static void switchAttr(std::string& out, vt::Attr& running, const vt::Attr& to) {
        if (to == running) return;
        if (to.link != running.link) out += "\x1b]8;;" + vt::linkUri(to.link) + "\x1b\\";
        vt::Attr plain = to, was = running;
        plain.link = was.link = 0;
        if (plain != was) appendSgr(out, to);
        running = to;
    }

    static void closeRun(std::string& out, vt::Attr& running) { switchAttr(out, running, vt::Attr()); }

    void appendRow(std::string& out, const Grid& grid, int y, int from, vt::Attr& running) const {
        const Cell* r = grid.row(y);
        int end = grid.cols;
        if (from == 0 && (grid.mark(y) & 1)) out += "\x1b]133;A\x1b\\";
        if (from == 0 && (grid.mark(y) & 2)) out += "\x1b]133;C\x1b\\";
        if (!grid.wrap(y)) while (end > from && r[end - 1].ch == ' ' && r[end - 1].attr == vt::Attr()) --end;
        appendCells(out, r, from, end, running);
    }

    void appendCells(std::string& out, const Cell* r, int from, int end, vt::Attr& running) const {
        for (int x = from; x < end; ++x) {
            if (r[x].ch == Console::WIDE_TAIL) continue;
            switchAttr(out, running, r[x].attr);
            uint32_t ch = r[x].ch;
            if (ch >= Console::CLUSTER_BASE) {
                std::u32string c = clusterOf(ch);
                if (c[0] < Console::SIZED_ROW) for (char32_t cp : c) if (cp <= 0x10FFFF) unicode::appendUtf8(out, cp);
            }
            else if (ch < 0x80) out += (char)ch;
            else unicode::appendUtf8(out, ch);
        }
    }

    void scrollOutRow(int y) {
        if (!onScrollOut || y < logRow) return;
        std::string out;
        vt::Attr running;
        appendRow(out, main, y, y == logRow ? logCol : 0, running);
        closeRun(out, running);
        if (!main.wrap(y)) out += "\r\n";
        logCol = 0;
        onScrollOut(out.data(), out.size());
    }

    bool streamable() const {
        return logging && onScrollOut && !altActive && g == &main && top == 0 && bottom == rows() - 1 && autowrapOn() && newlineReturns &&
               !insert && !cur.lineDrawing && !cur.pending && !cur.origin;
    }

    bool blankFrom(int x, int y) const {
        for (; y < rows(); ++y, x = 0) {
            const Cell* r = main.row(y);
            for (int k = x; k < cols(); ++k)
                if (r[k].ch != ' ' || r[k].attr != vt::Attr()) return false;
        }
        return true;
    }

    template <class T>
    size_t streamLines(const T* s, size_t n, size_t& scanned) {
        int need = rows();
        scanned = 0;
        if (n < (size_t)need || !streamable()) return 0;
        marks.resize(need + 1);
        size_t count = 0, i = 0, replayFrom = SIZE_MAX;
        bool reset = false;
        while (i < n) {
            i += printableRun(s + i, n - i);
            if (i >= n) break;
            T c = s[i];
            if (c == '\n' || c == '\t') {
                if (c == '\n') marks[count++ % (need + 1)] = { i + 1, replayFrom, reset };
                ++i;
                continue;
            }
            if (c == '\r' && i + 1 < n && s[i + 1] == '\n') { ++i; continue; }
            if (const T* q = c == 0x1B ? vt::sgrEnd(s + i, s + n) : nullptr) {
                size_t e = q - s;
                if (e - i == 3 || (e - i == 4 && s[i + 2] == '0')) { replayFrom = e; reset = true; }
                else if (replayFrom == SIZE_MAX) replayFrom = i;
                i = e;
                continue;
            }
            break;
        }
        scanned = (std::max)(i, (size_t)1);
        if (count <= (size_t)need) return 0;
        if (!blankFrom(cur.x, cur.y) || !blankFrom(0, cur.y + 1)) return 0;
        const StreamMark& cut = marks[(count - need - 1) % (need + 1)];
        vt::Attr cutAttr = cur.attr;
        if (cut.reset) vt::resetKeepingLink(cutAttr);
        const T* at = s + cut.at;
        for (const T* k = std::find(s + (std::min)(cut.replayFrom, cut.at), at, T(0x1B)); k < at; k = std::find(k, at, T(0x1B))) {
            const T* q = vt::sgrEnd(k, at);
            vt::applySgrSequence(cutAttr, k, q);
            k = q;
        }
        std::string out;
        out.reserve(cut.at + 256);
        vt::Attr running;
        for (int y = logRow; y < cur.y; ++y) {
            appendRow(out, main, y, y == logRow ? logCol : 0, running);
            if (!main.wrap(y)) out += "\r\n";
        }
        appendCells(out, main.row(cur.y), cur.y == logRow ? logCol : 0, cur.x, running);
        switchAttr(out, running, cur.attr);
        if constexpr (sizeof(T) == 1) {
            if (!out.empty()) onScrollOut(out.data(), out.size());
            onScrollOut(reinterpret_cast<const char*>(s), cut.at);
            out.clear();
        }
        else {
            size_t base = out.size();
            out.resize(base + cut.at);
            char* d = out.data() + base;
            for (size_t k = 0; k < cut.at; ++k) d[k] = (char)s[k];
        }
        vt::Attr closing = cutAttr;
        closeRun(out, closing);
        if (!out.empty()) onScrollOut(out.data(), out.size());
        main.clearRows(0, rows(), blankCell());
        images.clearScreen(0);
        cur.x = cur.y = 0;
        cur.attr = cutAttr;
        logRow = logCol = 0;
        lastX = lastY = -1;
        lastPrinted = ' ';
        return cut.at;
    }

    template <class T>
    void feed(const T* s, size_t n) {
        size_t i = 0, retry = 0;
        while (i < n) {
            if (state == APC && utfNeed == 0) {
                size_t j = i;
                while (j < n && s[j] != 0x1B && s[j] != 0x07 && s[j] < 0x80) ++j;
                if (j > i) {
                    if (apc.size() + (j - i) <= graphics::MAX_PAYLOAD) {
                        if constexpr (sizeof(T) == 1) apc.append(reinterpret_cast<const char*>(s + i), j - i);
                        else for (size_t k = i; k < j; ++k) apc += (char)s[k];
                    }
                    i = j;
                    continue;
                }
            }
            if (state == GROUND && utfNeed == 0) {
                if (i >= retry && n - i > (size_t)rows() * 8) {
                    size_t scanned = 0, streamed = streamLines(s + i, n - i, scanned);
                    if (streamed) { i += streamed; continue; }
                    retry = i + scanned;
                }
                size_t j = i + printableRun(s + i, n - i);
                if (j > i) {
                    if (insert || cur.lineDrawing) for (size_t k = i; k < j; ++k) print(s[k]);
                    else printAscii(s + i, j - i);
                    i = j;
                    continue;
                }
            }
            uint32_t c = s[i++];
            if constexpr (sizeof(T) == 1) {
                if (utfNeed) {
                    if ((c & 0xC0) == 0x80) {
                        utfCode = utfCode << 6 | (c & 0x3F);
                        if (--utfNeed) continue;
                        c = utfCode;
                    }
                    else { utfNeed = 0; codepoint(0xFFFD); }
                }
                else if (c >= 0x80) {
                    if (!utf8) c = unicode::oemByte((unsigned char)c);
                    else if (c >= 0xC2 && c < 0xF5) { utfNeed = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1; utfCode = c & (0x3F >> utfNeed); continue; }
                    else c = 0xFFFD;
                }
            }
            else {
                if (utfNeed) {
                    utfNeed = 0;
                    if (c >= 0xDC00 && c < 0xE000) c = 0x10000 + ((utfCode - 0xD800) << 10) + (c - 0xDC00);
                    else { codepoint(0xFFFD); }
                }
                else if (c >= 0xD800 && c < 0xDC00) { utfNeed = 1; utfCode = c; continue; }
            }
            codepoint(c);
        }
    }

    template <class T>
    void printAscii(const T* s, size_t n) {
        size_t i = 0;
        while (i < n) {
            if (cur.pending) { print(s[i++]); continue; }
            Cell* r = g->row(cur.y);
            int x = cur.x, c = cols();
            size_t k = (std::min)((size_t)(c - x), n - i);
            touched(cur.y, x);
            Grid::detachLeft(r, x);
            g->detachRight(r, x + (int)k);
            const vt::Attr a = cur.attr;
            for (size_t j = 0; j < k; ++j) r[x + j] = { (uint32_t)s[i + j], a };
            i += k;
            lastX = x + (int)k - 1;
            lastY = cur.y;
            cur.x = x + (int)k;
            if (cur.x >= c) {
                cur.x = c - 1;
                if (autowrapOn()) cur.pending = true;
                else if (i < n) { r[c - 1] = { (uint32_t)s[n - 1], a }; i = n; }
            }
        }
        lastPrinted = (uint32_t)s[n - 1];
    }

    bool attaches(uint32_t cp) const {
        if (lastX < 0 || lastY != cur.y) return false;
        if (unicode::isZeroWidth(cp) || cp == unicode::ZWJ || cp == unicode::VS_EMOJI || cp == unicode::VS_TEXT) return true;
        std::u32string c = clusterOf(g->row(lastY)[lastX].ch);
        return c.back() == unicode::ZWJ || (unicode::isSkinTone(cp) && unicode::isWide(c[0])) ||
               (unicode::isRegionalIndicator(cp) && c.size() == 1 && unicode::isRegionalIndicator(c[0]));
    }

    void attach(uint32_t cp) {
        Cell& cell = g->row(lastY)[lastX];
        std::u32string c = clusterOf(cell.ch);
        if (c.size() >= 16) return;
        bool wasWide = lastX + 1 < cols() && g->row(lastY)[lastX + 1].ch == Console::WIDE_TAIL;
        c += (char32_t)cp;
        cell.ch = intern(c);
        bool wide = cp == unicode::VS_EMOJI || unicode::isRegionalIndicator(cp);
        if (wide && !wasWide && lastX + 1 < cols() && cur.x == lastX + 1 && !cur.pending) {
            g->row(lastY)[lastX + 1] = { Console::WIDE_TAIL, cell.attr };
            cur.x = lastX + 2;
            if (cur.x >= cols()) { cur.x = cols() - 1; cur.pending = autowrapOn(); }
        }
    }

    void wrapLine() {
        g->wrap(cur.y) = 1;
        cur.x = 0;
        index();
    }

    void print(uint32_t cp) {
        static const uint16_t dec[31] = { 0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0, 0x00B1, 0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C, 0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534, 0x252C, 0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7 };
        if (cur.lineDrawing && cp >= 0x60 && cp <= 0x7E) cp = dec[cp - 0x60];
        if (cp >= 0x300 && attaches(cp)) { attach(cp); return; }
        if (cp >= 0x300 && unicode::isZeroWidth(cp)) return;
        int w = unicode::isWide(cp) ? 2 : 1;
        if (w == 2 && cols() < 2) w = 1;
        if (cur.pending) {
            cur.pending = false;
            if (autowrapOn()) wrapLine();
        }
        if (w == 2 && cur.x == cols() - 1) {
            if (autowrapOn()) { putCell(cur.x, cur.y, ' ', cur.attr); wrapLine(); }
            else cur.x = cols() - 2;
        }
        if (insert) insertCells(w);
        putCell(cur.x, cur.y, cp, cur.attr);
        if (w == 2) { touched(cur.y, cur.x + 1); g->row(cur.y)[cur.x + 1] = { Console::WIDE_TAIL, cur.attr }; if (cur.x + 2 < cols() && g->row(cur.y)[cur.x + 2].ch == Console::WIDE_TAIL) g->row(cur.y)[cur.x + 2].ch = ' '; }
        lastX = cur.x;
        lastY = cur.y;
        lastPrinted = cp;
        cur.x += w;
        if (cur.x >= cols()) {
            cur.x = cols() - 1;
            cur.pending = autowrapOn();
        }
    }

    void index() {
        if (cur.y == bottom) scrollRegionUp(top, bottom, 1);
        else if (cur.y < rows() - 1) ++cur.y;
        if (std::exchange(outputMarkPending, false)) g->mark(cur.y) |= 2;
    }

    void reverseIndex() {
        if (cur.y == top) scrollRegionDown(top, bottom, 1);
        else if (cur.y > 0) --cur.y;
    }

    void control(uint32_t c) {
        switch (c) {
        case 0x08:
            if (cur.pending) cur.pending = false;
            else if (cur.x > 0) --cur.x;
            break;
        case 0x09: cur.x = (std::min)(cols() - 1, (cur.x / 8 + 1) * 8); cur.pending = false; break;
        case 0x0A: case 0x0B: case 0x0C:
            cur.pending = false;
            index();
            if (newlineReturns) cur.x = 0;
            break;
        case 0x0D: cur.x = 0; cur.pending = false; break;
        case 0x07: if (onBell) onBell(); break;
        case 0x0E: case 0x0F: break;
        case 0x18: case 0x1A: state = GROUND; break;
        case 0x1B: state = ESCAPE; intermediate = 0; break;
        }
    }

    void codepoint(uint32_t c) {
        switch (state) {
        case GROUND:
            if (c < 0x20) control(c);
            else if (c != 0x7F) print(c);
            return;
        case ESCAPE:
            if (c < 0x20) { control(c); return; }
            if (c < 0x30) { intermediate = c; state = ESCAPE_INTER; return; }
            state = GROUND;
            escape(c);
            return;
        case ESCAPE_INTER:
            if (c < 0x20) { control(c); return; }
            if (c < 0x30) return;
            state = GROUND;
            if (intermediate == '(') cur.lineDrawing = c == '0';
            return;
        case CSI:
            if (c >= '0' && c <= '9') {
                if (!paramStarted) { paramStarted = true; if (count == 0) count = 1; }
                params[count - 1] = (std::min)(params[count - 1] * 10 + (c - '0'), 65535u);
                return;
            }
            if (c == ';' || c == ':') {
                if (count == 0) count = 1;
                if (count < vt::MAX_PARAMS) { if (c == ':') sub |= 1u << count; params[count++] = 0; }
                paramStarted = true;
                return;
            }
            if (c >= 0x3C && c <= 0x3F) { marker = c; return; }
            if (c >= 0x20 && c <= 0x2F) { intermediate = c; return; }
            if (c < 0x20) { control(c); return; }
            state = GROUND;
            if (c >= 0x40 && c <= 0x7E) csi(c);
            return;
        case OSC:
            if (c == 0x07) { state = GROUND; oscDispatch(); return; }
            if (c == 0x1B) { state = OSC_ESC; return; }
            if (osc.size() < 1 << 20) osc += (char32_t)c;
            return;
        case OSC_ESC:
            state = GROUND;
            oscDispatch();
            if (c != '\\') { state = ESCAPE; codepoint(c); }
            return;
        case STRING:
            if (c == 0x1B) state = STRING_ESC;
            else if (c == 0x07) state = GROUND;
            return;
        case STRING_ESC:
            state = c == '\\' ? GROUND : STRING;
            return;
        case APC:
            if (c == 0x1B) state = APC_ESC;
            else if (c == 0x07) { state = GROUND; apcDispatch(); }
            else if (apc.size() < graphics::MAX_PAYLOAD) unicode::appendUtf8(apc, c);
            return;
        case APC_ESC:
            state = GROUND;
            apcDispatch();
            if (c != '\\') { state = ESCAPE; codepoint(c); }
            return;
        }
    }

    void apcDispatch() {
        if (apc.empty() || apc[0] != 'G') return;
        graphics::Cursor at{ cur.x, cur.y, (std::max)(cellW, 1), (std::max)(cellH, 1), screen() };
        graphics::Effect e = images.handle(apc.substr(1), at);
        apc.clear();
        if (!e.reply.empty()) reply(e.reply);
        if (!e.placed || !e.moveCursor) return;
        cur.pending = false;
        for (int k = 1; k < e.rows; ++k) index();
        cur.x = (std::min)(cur.x + e.cols, cols() - 1);
    }

    void escape(uint32_t c) {
        switch (c) {
        case '[': state = CSI; count = 0; params[0] = 0; sub = 0; marker = 0; intermediate = 0; paramStarted = false; break;
        case ']': state = OSC; osc.clear(); break;
        case 'P': case 'X': case '^': state = STRING; break;
        case '_': state = APC; apc.clear(); break;
        case '7': saved = cur; break;
        case '8': restoreCursor(saved); break;
        case 'D': cur.pending = false; index(); break;
        case 'E': cur.pending = false; index(); cur.x = 0; break;
        case 'M': cur.pending = false; reverseIndex(); break;
        case 'c': fullReset(); break;
        }
    }

    void restoreCursor(const Cursor& c) {
        cur = c;
        clampCursor();
    }

    void fullReset() {
        int r = rows();
        if (altActive) switchAlt(false, false);
        cur = Cursor();
        eraseRows(0, r);
        resetState(r);
    }

    void resetState(int r) {
        colors = Colors();
        images.reset();
        colorStack.clear();
        titles.clear();
        top = 0;
        bottom = r - 1;
        autowrap = true;
        insert = false;
        cursorVisible = true;
        appCursor = bracketedPaste = focusEvents = sync = mouseSgr = false;
        modifyOtherKeys = 0;
        kitty[0] = kitty[1] = Kitty();
        mouse = cursorShape = 0;
        inBandResize = mousePixels = colorSchemeReports = false;
        pointer.clear();
        pointerStack.clear();
        cursors.clear();
        clipboard.reset();
        clipboard.reply = [this](const std::string& s) { reply(s); };
        clipboard.store = [this](const std::string& s) { if (onClipboard) onClipboard(s); };
        clipboard.ask = [this] { if (onClipboardRead) onClipboardRead(); };
    }

    void clampCursor() {
        cur.x = (std::min)(cur.x, cols() - 1);
        cur.y = (std::min)(cur.y, rows() - 1);
    }

    int param(int i, int fallback) const { return i < count && params[i] ? (int)params[i] : fallback; }

    void moveTo(int x, int y) {
        if (cur.origin) { y += top; y = std::clamp(y, top, bottom); }
        setCursor(x, y);
    }

    void insertCells(int n) {
        Cell* r = g->row(cur.y);
        int c = cols();
        n = (std::min)(n, c - cur.x);
        touched(cur.y, cur.x);
        std::move_backward(r + cur.x, r + c - n, r + c);
        std::fill(r + cur.x, r + cur.x + n, blankCell());
        if (r[c - 1].ch != Console::WIDE_TAIL && c >= 2 && r[c - 1].ch != ' ' && unicode::isWide(r[c - 1].ch)) r[c - 1].ch = ' ';
    }

    void deleteCells(int n) {
        Cell* r = g->row(cur.y);
        int c = cols();
        n = (std::min)(n, c - cur.x);
        touched(cur.y, cur.x);
        if (r[cur.x].ch == Console::WIDE_TAIL && cur.x > 0) r[cur.x - 1].ch = ' ';
        std::move(r + cur.x + n, r + c, r + cur.x);
        std::fill(r + c - n, r + c, blankCell());
        if (r[cur.x].ch == Console::WIDE_TAIL) r[cur.x].ch = ' ';
    }

    void switchAlt(bool on, bool saveCursor) {
        if (on == altActive) return;
        if (on) {
            mainSaved = cur;
            g = &alt;
            altActive = true;
            if (saveCursor) { alt.clearRows(0, alt.rows, blankCell()); images.clearScreen(1); }
            cursors.clear();
        }
        else {
            g = &main;
            altActive = false;
            if (saveCursor) restoreCursor(mainSaved);
            else clampCursor();
        }
        top = 0;
        bottom = rows() - 1;
        lastX = lastY = -1;
    }

    void reply(const std::string& s) { if (onReply && !s.empty()) onReply(s); }

    int modeState(int mode, bool priv) const {
        if (!priv) return mode == 4 ? (insert ? 1 : 2) : mode == 20 ? (newlineReturns ? 1 : 2) : 0;
        switch (mode) {
        case 1: return appCursor ? 1 : 2;
        case 6: return cur.origin ? 1 : 2;
        case 7: return autowrap ? 1 : 2;
        case 25: return cursorVisible ? 1 : 2;
        case 47: case 1047: case 1049: return altActive ? 1 : 2;
        case 1000: case 1002: case 1003: return mouse == mode ? 1 : 2;
        case 1004: return focusEvents ? 1 : 2;
        case 1006: return mouseSgr ? 1 : 2;
        case 2004: return bracketedPaste ? 1 : 2;
        case 2026: return sync ? 1 : 2;
        case 1016: return mousePixels ? 1 : 2;
        case 2027: return 3;
        case 2031: return colorSchemeReports ? 1 : 2;
        case 2048: return inBandResize ? 1 : 2;
        case 5522: return clipboard.pasteEvents ? 1 : 2;
        case 12: return 2;
        }
        return 0;
    }

    void setMode(int mode, bool priv, bool on) {
        if (!priv) {
            if (mode == 4) insert = on;
            return;
        }
        switch (mode) {
        case 1: appCursor = on; break;
        case 6: cur.origin = on; moveTo(0, 0); break;
        case 7: autowrap = on; if (!on) cur.pending = false; break;
        case 25: cursorVisible = on; break;
        case 47: case 1047: switchAlt(on, false); break;
        case 1049:
            if (on && !altActive) { switchAlt(true, true); cur.pending = false; }
            else if (!on && altActive) switchAlt(false, true);
            break;
        case 1000: case 1002: case 1003: mouse = on ? mode : 0; break;
        case 1004: focusEvents = on; break;
        case 1006: mouseSgr = on; break;
        case 2004: bracketedPaste = on; break;
        case 2026: sync = on; break;
        case 1016: mousePixels = on; break;
        case 2031: colorSchemeReports = on; break;
        case 2048: inBandResize = on; reportSize(); break;
        case 5522: clipboard.pasteEvents = on; break;
        }
    }

    void kittyCommand() {
        Kitty& k = kitty[screen()];
        if (marker == '?') reply("\x1b[?" + std::to_string(k.flags) + "u");
        else if (marker == '>') {
            if (k.stack.size() >= KITTY_DEPTH) k.stack.erase(k.stack.begin());
            k.stack.push_back(k.flags);
            k.flags = param(0, 0) & KITTY_ALL;
        }
        else if (marker == '<') {
            size_t n = (size_t)(std::max)(param(0, 1), 1);
            if (n > k.stack.size()) { k.stack.clear(); k.flags = 0; return; }
            k.flags = k.stack[k.stack.size() - n];
            k.stack.resize(k.stack.size() - n);
        }
        else if (marker == '=') {
            int f = param(0, 0) & KITTY_ALL, how = param(1, 1);
            k.flags = how == 1 ? f : how == 2 ? k.flags | f : how == 3 ? k.flags & ~f : k.flags;
        }
    }

    void csi(uint32_t c) {
        bool priv = marker == '?';
        if (marker && marker != '?' && c != 'c' && c != 'm' && c != 'u' && c != 'q') return;
        int n = param(0, 1);
        switch (c) {
        case '@': if (!intermediate) insertCells(n); break;
        case 'A': { int lim = cur.y >= top ? top : 0; setCursor(cur.x, (std::max)(cur.y - n, lim)); break; }
        case 'B': case 'e': { int lim = cur.y <= bottom ? bottom : rows() - 1; setCursor(cur.x, (std::min)(cur.y + n, lim)); break; }
        case 'C': case 'a': setCursor(cur.x + n, cur.y); break;
        case 'D': setCursor(cur.x - n, cur.y); break;
        case 'E': setCursor(0, (std::min)(cur.y + n, cur.y <= bottom ? bottom : rows() - 1)); break;
        case 'F': setCursor(0, (std::max)(cur.y - n, cur.y >= top ? top : 0)); break;
        case 'G': case '`': setCursor(n - 1, cur.y); break;
        case 'H': case 'f': moveTo(param(1, 1) - 1, param(0, 1) - 1); break;
        case 'd': moveTo(cur.x, n - 1); break;
        case 'I': for (int k = 0; k < n; ++k) control(0x09); break;
        case 'Z': setCursor(cur.x > 0 ? ((cur.x - 1) / 8) * 8 : 0, cur.y); break;
        case 'J': {
            int mode = param(0, 0);
            if (mode == 0) { eraseInRow(cur.y, cur.x, cols()); eraseRows(cur.y + 1, rows()); }
            else if (mode == 1) { eraseRows(0, cur.y); eraseInRow(cur.y, 0, cur.x + 1); }
            else if (mode == 2) {
                if (logging && !altActive && onScrollOut) {
                    std::string text = pendingText(lastUsedRow());
                    if (!text.empty()) { text += "\r\n"; onScrollOut(text.data(), text.size()); }
                }
                eraseRows(0, rows());
                images.clearScreen(screen());
                cursors.clear();
                if (logging && !altActive) { logRow = 0; logCol = 0; }
            }
            else if (mode == 3) { images.clearScreen(screen()); cursors.clear(); if (logging && !altActive && onClear) onClear(); }
            break;
        }
        case 'K': {
            int mode = param(0, 0);
            if (mode == 0) eraseInRow(cur.y, cur.x, cols());
            else if (mode == 1) eraseInRow(cur.y, 0, cur.x + 1);
            else eraseInRow(cur.y, 0, cols());
            break;
        }
        case 'X': eraseInRow(cur.y, cur.x, cur.x + n); break;
        case 'P': if (intermediate == '#') pushColors(); else deleteCells(n); break;
        case 'Q': if (intermediate == '#') popColors(param(0, 0)); break;
        case 'R': if (intermediate == '#') reply("\x1b[?" + std::to_string(colorStack.size()) + ";" + std::to_string(colorStack.size()) + "#Q"); break;
        case 'L': if (cur.y >= top && cur.y <= bottom) { scrollRegionDown(cur.y, bottom, n); cur.x = 0; } break;
        case 'M': if (cur.y >= top && cur.y <= bottom) { deleteLines(cur.y, bottom, n); cur.x = 0; } break;
        case 'S': if (!priv) scrollRegionUp(top, bottom, n); break;
        case 'T': if (!priv) scrollRegionDown(top, bottom, n); break;
        case 'b': for (int k = 0; k < (std::min)(n, 65535); ++k) print(lastPrinted); break;
        case 'c':
            if (marker == '>') reply("\x1b[>0;10;1c");
            else if (!marker) reply("\x1b[?61;4;6;7;14;21;22;23;24;28;32;42c");
            break;
        case 'h': case 'l':
            for (int i = 0; i < (std::max)(count, 1); ++i) setMode((int)params[i], priv, c == 'h');
            break;
        case 'm':
            if (!marker) vt::applySgr(cur.attr, params, count, sub);
            else if (marker == '>' && param(0, 0) == 4) modifyOtherKeys = count > 1 ? param(1, 0) : 0;
            break;
        case 'n':
            if (priv && param(0, 0) == 996) reply(std::string("\x1b[?997;") + (dark() ? "1" : "2") + "n");
            else if (param(0, 0) == 5) reply("\x1b[0n");
            else if (param(0, 0) == 6) {
                int y = cur.y + 1 - (cur.origin ? top : 0);
                reply("\x1b[" + std::string(priv ? "?" : "") + std::to_string(y) + ";" + std::to_string(cur.x + 1) + "R");
            }
            break;
        case 'p':
            if (intermediate == '$') {
                int mode = param(0, 0);
                reply("\x1b[" + std::string(priv ? "?" : "") + std::to_string(mode) + ";" + std::to_string(modeState(mode, priv)) + "$y");
            }
            else if (intermediate == '!') fullReset();
            break;
        case 'q':
            if (intermediate == ' ' && marker == '>') reply(cursors.command(params, count, sub, cols(), rows()));
            else if (intermediate == ' ') cursorShape = param(0, 0);
            else if (marker == '>') reply("\x1bP>|ccc\x1b\\");
            break;
        case 'r':
            if (!priv) {
                int t = param(0, 1) - 1, b = param(1, rows()) - 1;
                if (t < b && b < rows()) { top = t; bottom = b; }
                else { top = 0; bottom = rows() - 1; }
                moveTo(0, 0);
            }
            break;
        case 's': if (!priv && !marker) saved = cur; break;
        case 'u':
            if (!marker) restoreCursor(saved);
            else kittyCommand();
            break;
        case 't': windowOp(); break;
        }
    }

    void windowOp() {
        switch (param(0, 0)) {
        case 14: if (cellW) reply("\x1b[4;" + std::to_string(rows() * cellH) + ";" + std::to_string(cols() * cellW) + "t"); break;
        case 16: if (cellW) reply("\x1b[6;" + std::to_string(cellH) + ";" + std::to_string(cellW) + "t"); break;
        case 18: reply("\x1b[8;" + std::to_string(rows()) + ";" + std::to_string(cols()) + "t"); break;
        case 22:
            if (titles.size() >= STACK_DEPTH) titles.erase(titles.begin());
            titles.push_back(title);
            break;
        case 23:
            if (titles.empty()) break;
            setTitle(titles.back());
            titles.pop_back();
            break;
        }
    }

    void setTitle(const std::wstring& t) {
        title = t;
        if (onTitle) onTitle(t);
    }

    void pushColors() {
        if (colorStack.size() >= STACK_DEPTH) colorStack.erase(colorStack.begin());
        colorStack.push_back(colors);
    }

    void popColors(int index) {
        if (colorStack.empty()) return;
        size_t at = index > 0 ? (std::min)((size_t)index, colorStack.size()) - 1 : colorStack.size() - 1;
        bool darkBefore = dark();
        colors = colorStack[at];
        colorStack.resize(at);
        if (dark() != darkBefore) reportColorScheme();
    }

    void deleteLines(int t, int b, int n) {
        n = (std::min)(n, b - t + 1);
        touched(t, 0);
        g->rotateUp(t, b, n);
        g->clearRows(b - n + 1, b + 1, blankCell());
        images.scroll(screen(), t, b, n, false);
    }

    static std::vector<std::string> split(const std::string& s, char sep) {
        std::vector<std::string> out;
        size_t from = 0;
        for (;;) {
            size_t at = s.find(sep, from);
            out.push_back(s.substr(from, at == std::string::npos ? std::string::npos : at - from));
            if (at == std::string::npos) return out;
            from = at + 1;
        }
    }

    static bool number(const std::string& s, int& n) {
        if (s.empty() || s.size() > 5) return false;
        n = 0;
        for (char c : s) { if (c < '0' || c > '9') return false; n = n * 10 + (c - '0'); }
        return true;
    }

    void oscReply(const std::string& body) { reply("\x1b]" + body + "\x1b\\"); }

    uint32_t dynamicColor(int which) const {
        uint32_t fg = colors.fg & vt::COLOR_DEFAULT ? defaultFg : colors.fg;
        if (which == 10) return fg;
        if (which == 11) return effectiveBg();
        return colors.cursor & vt::COLOR_DEFAULT ? fg : colors.cursor;
    }

    void setDynamicColor(int which, uint32_t c) {
        bool darkBefore = dark();
        (which == 10 ? colors.fg : which == 11 ? colors.bg : colors.cursor) = c;
        if (dark() != darkBefore) reportColorScheme();
    }

    void dynamicColors(int first, const std::string& text) {
        std::vector<std::string> specs = split(text, ';');
        for (size_t i = 0; i < specs.size() && first + (int)i <= 12; ++i) {
            int which = first + (int)i;
            uint32_t c;
            if (specs[i] == "?") oscReply(std::to_string(which) + ";" + palette::format(dynamicColor(which)));
            else if (palette::parse(specs[i], c)) setDynamicColor(which, c);
        }
    }

    void paletteColors(const std::string& text) {
        std::vector<std::string> parts = split(text, ';');
        for (size_t i = 0; i + 1 < parts.size(); i += 2) {
            int index;
            uint32_t c;
            if (!number(parts[i], index) || index > 255) continue;
            if (parts[i + 1] == "?") oscReply("4;" + parts[i] + ";" + palette::format(colors.table[index]));
            else if (palette::parse(parts[i + 1], c)) { colors.table[index] = c; colors.changed = true; }
        }
    }

    void resetPalette(const std::string& text) {
        int index;
        if (text.empty()) colors.table = palette::standardTable();
        else for (const std::string& part : split(text, ';')) if (number(part, index) && index < 256) colors.table[index] = palette::standard(index);
        colors.changed = colors.table != palette::standardTable();
    }

    void kittyColors(const std::string& text) {
        std::string answer;
        for (const std::string& item : split(text, ';')) {
            size_t eq = item.find('=');
            std::string key = item.substr(0, eq), value = eq == std::string::npos ? std::string() : item.substr(eq + 1);
            static const struct { const char* name; int which; } dynamic[] = { { "foreground", 10 }, { "background", 11 }, { "cursor", 12 } };
            int index = -1, which = 0;
            for (auto& d : dynamic) if (key == d.name) which = d.which;
            if (!which && (!number(key, index) || index > 255)) index = -1;
            uint32_t c;
            if (value == "?") {
                std::string shown = which ? palette::format(dynamicColor(which)) : index >= 0 ? palette::format(colors.table[index]) : "";
                answer += ";" + key + "=" + shown;
            }
            else if (which && value.empty()) setDynamicColor(which, vt::COLOR_DEFAULT);
            else if (which && palette::parse(value, c)) setDynamicColor(which, c);
            else if (index >= 0) {
                colors.table[index] = value.empty() || !palette::parse(value, c) ? palette::standard(index) : c;
                colors.changed = colors.table != palette::standardTable();
            }
        }
        if (!answer.empty()) oscReply("21" + answer);
    }

    void kittyNotification(const std::string& text) {
        size_t semi = text.find(';');
        std::string meta = text.substr(0, semi), payload = semi == std::string::npos ? std::string() : text.substr(semi + 1);
        bool done = true, encoded = false, body = false;
        for (const std::string& kv : split(meta, ':')) {
            if (kv == "d=0") done = false;
            else if (kv == "e=1") encoded = true;
            else if (kv == "p=body") body = true;
            else if (kv.rfind("p=", 0) == 0 && kv != "p=title") return;
        }
        if (encoded) payload = base64::decode(payload);
        (body ? notifyBody : notifyTitle) += payload;
        if (!done) return;
        if (onNotify) onNotify(notifyTitle, notifyBody);
        notifyTitle.clear();
        notifyBody.clear();
    }

    void conEmuCommand(const std::string& text) {
        size_t semi = text.find(';');
        int sub;
        if (semi == std::string::npos || !number(text.substr(0, semi), sub)) { if (onNotify) onNotify("", text); return; }
        std::string rest = text.substr(semi + 1);
        if (sub == 4 && onProgress) {
            int fields[2] = { 0, 0 }, k = 0;
            for (size_t i = 0; i < rest.size() && k < 2; ++i) {
                if (rest[i] == ';') ++k;
                else if (rest[i] >= '0' && rest[i] <= '9') fields[k] = (std::min)(fields[k] * 10 + (rest[i] - '0'), 1000);
            }
            onProgress(fields[0], (std::min)(fields[1], 100));
        }
        else if (sub == 9 && onCwd) {
            if (rest.size() >= 2 && rest.front() == '"' && rest.back() == '"') rest = rest.substr(1, rest.size() - 2);
            onCwd(rest);
        }
    }

    void pointerShape(const std::string& text) {
        char op = text.empty() ? 0 : text[0];
        std::string names = op == '=' || op == '>' || op == '<' || op == '?' ? text.substr(1) : text;
        if (op == '?') {
            std::string answer;
            for (const std::string& n : split(names, ',')) {
                std::string value = n == "__current__" ? (pointer.empty() ? std::string("text") : pointer) : n == "__default__" || n == "__grabbed__" ? "text" : findPointer(n) ? "1" : "0";
                answer += (answer.empty() ? "" : ",") + value;
            }
            oscReply("22;" + answer);
            return;
        }
        if (op == '<') {
            pointer = pointerStack.empty() ? std::string() : pointerStack.back();
            if (!pointerStack.empty()) pointerStack.pop_back();
            return;
        }
        std::string chosen;
        for (const std::string& n : split(names, ',')) if (chosen.empty() && findPointer(n)) chosen = n;
        if (op == '>') {
            if (pointerStack.size() >= STACK_DEPTH) pointerStack.erase(pointerStack.begin());
            pointerStack.push_back(pointer);
        }
        pointer = chosen;
    }

    void sizedText(const std::string& body) {
        size_t semi = body.find(';');
        std::u32string text;
        for (size_t i = semi == std::string::npos ? body.size() : semi + 1; i < body.size();) {
            uint32_t cp = unicode::decodeUtf8(body, i);
            if (cp >= 0x20 && cp != 0x7F) text += (char32_t)cp;
        }
        Console::Sizing z;
        int w = 0;
        for (const std::string& kv : split(body.substr(0, semi), ':')) {
            int value = 0;
            if (kv.size() < 3 || kv[1] != '=' || !number(kv.substr(2), value)) continue;
            switch (kv[0]) {
            case 'w': w = std::clamp(value, 0, 7); break;
            case 's': z.s = std::clamp(value, 1, 7); break;
            case 'n': z.n = std::clamp(value, 0, 15); break;
            case 'd': z.d = std::clamp(value, 0, 15); break;
            case 'v': z.v = std::clamp(value, 0, 2); break;
            case 'h': z.h = std::clamp(value, 0, 2); break;
            }
        }
        if (!z.d || z.n >= z.d) z.n = z.d = 0;
        if (!w && !z.scaled()) { for (char32_t cp : text) print(cp); return; }
        if (w) { z.w = w; placeBlock(text, z); return; }
        for (char32_t cp : text) { z.w = unicode::isWide(cp) ? 2 : 1; placeBlock(std::u32string(1, cp), z); }
    }

    void placeBlock(const std::u32string& text, const Console::Sizing& z) {
        int width = z.w * z.s;
        if (text.empty() || width > cols() || z.s > rows()) return;
        if (cur.pending) { cur.pending = false; if (autowrapOn()) wrapLine(); }
        if (cur.x + width > cols()) {
            if (autowrapOn()) { eraseInRow(cur.y, cur.x, cols()); wrapLine(); }
            else cur.x = cols() - width;
        }
        int below = cur.y + z.s - 1 - bottom;
        if (below > 0) { scrollRegionUp(top, bottom, below); cur.y -= below; }
        std::u32string head = std::u32string(1, z.head()) + text;
        for (int r = 0; r < z.s && cur.y + r < rows(); ++r) {
            putCell(cur.x, cur.y + r, intern(r ? std::u32string(1, Console::SIZED_ROW + (char32_t)r) + head : head), cur.attr);
            Cell* row = g->row(cur.y + r);
            for (int k = 1; k < width; ++k) row[cur.x + k] = { Console::WIDE_TAIL, cur.attr };
            g->detachRight(row, cur.x + width);
        }
        lastX = cur.x;
        lastY = cur.y;
        cur.x += width;
        if (cur.x >= cols()) { cur.x = cols() - 1; cur.pending = autowrapOn(); }
    }

    void oscDispatch() {
        size_t semi = osc.find(U';');
        int num = 0;
        for (size_t i = 0; i < (semi == std::u32string::npos ? osc.size() : semi); ++i) {
            if (osc[i] < '0' || osc[i] > '9') return;
            num = num * 10 + (int)(osc[i] - '0');
        }
        std::string text;
        if (semi != std::u32string::npos) for (size_t i = semi + 1; i < osc.size(); ++i) unicode::appendUtf8(text, osc[i]);
        switch (num) {
        case 0: case 2: {
            std::wstring w;
            for (size_t i = semi == std::u32string::npos ? osc.size() : semi + 1; i < osc.size(); ++i) unicode::appendUtf16(w, osc[i]);
            setTitle(w);
            break;
        }
        case 4: paletteColors(text); break;
        case 104: resetPalette(text); break;
        case 10: case 11: case 12: dynamicColors(num, text); break;
        case 110: case 111: case 112: setDynamicColor(num - 100, vt::COLOR_DEFAULT); break;
        case 21: kittyColors(text); break;
        case 7: if (onCwd) onCwd(text); break;
        case 8: cur.attr.link = vt::oscLink("8;" + text); break;
        case 9: conEmuCommand(text); break;
        case 22: pointerShape(text); break;
        case 52: clipboard.osc52(text); break;
        case 5522: clipboard.osc5522(text); break;
        case 66: sizedText(text); break;
        case 99: kittyNotification(text); break;
        case 133:
            if (!text.empty() && text[0] == 'A') g->mark(cur.y) |= 1;
            else if (!text.empty() && text[0] == 'C') { if (cur.x || cur.pending) outputMarkPending = true; else g->mark(cur.y) |= 2; }
            break;
        case 777: {
            std::vector<std::string> parts = split(text, ';');
            if (parts.size() >= 2 && parts[0] == "notify" && onNotify) onNotify(parts[1], parts.size() > 2 ? parts[2] : std::string());
            break;
        }
        }
    }
};

}
