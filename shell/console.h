#pragma once
#include <algorithm>
#include <bit>
#include <atomic>
#include <climits>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "bitmap.h"
#include "simd.h"
#include "unicode.h"
#include "vt.h"

constexpr uint32_t TERM_FG = 0xFFCCCCCC, TERM_BG = 0xFF0C0C0C;

class Console {
public:
    static constexpr size_t CHUNK = 4 << 20;
    static constexpr size_t MIN_READ = 64 << 10;
    static constexpr size_t MAX_READ = 1 << 20;
    static constexpr uint64_t MARK_STRIDE = 4 << 10;
    static constexpr size_t OWNED_BUDGET = 64 << 20;
    static constexpr size_t MAX_SEGMENTS = 1 << 16;
    static constexpr int MAX_LINE = 1 << 16;
    static constexpr int MAX_CLUSTER = 16;
    static constexpr int MAX_WALK_LINES = 4096;
    static constexpr int NO_WRAP_STRIDE = 1 << 20;
    static constexpr uint32_t WIDE_TAIL = 0x1FFFFF, CLUSTER_BASE = 0x200000;

    void (*onChange)() = nullptr;
    std::atomic<bool> lineWrap{ true };

    static std::shared_ptr<void> newChunk() {
        struct Pool { std::mutex m; std::vector<char*> free; };
        static Pool* pool = new Pool;
        char* p = nullptr;
        { std::lock_guard<std::mutex> g(pool->m); if (!pool->free.empty()) { p = pool->free.back(); pool->free.pop_back(); } }
        return std::shared_ptr<void>(p ? p : new char[CHUNK], [](void* q) {
            std::lock_guard<std::mutex> g(pool->m);
            if (pool->free.size() < 8) pool->free.push_back(static_cast<char*>(q));
            else delete[] static_cast<char*>(q);
        });
    }

    void write(const char* data, size_t n) {
        {
            auto lock = priorityLock();
            while (n) {
                if (ownUsed == CHUNK) { own = newChunk(); ownBase = static_cast<char*>(own.get()); ownUsed = 0; }
                size_t k = (std::min)(n, CHUNK - ownUsed);
                char* dst = ownBase + ownUsed;
                std::memcpy(dst, data, k);
                ownUsed += k;
                ingest(dst, k, own);
                data += k;
                n -= k;
            }
        }
        notify();
    }
    void write(const std::string& s) { write(s.data(), s.size()); }

    void adopt(const char* data, size_t n, const std::shared_ptr<void>& keep) {
        { auto lock = priorityLock(); ingest(data, n, keep); }
        notify();
    }

    void redirect(const char* data, size_t n, std::shared_ptr<void> keep) {
        if (!n) return;
        {
            auto lock = priorityLock();
            pendingCR = false;
            tailLive = false;
            segments.push_back({ data, n, end, std::move(keep), false });
            end += n;
            ++generation;
            trim();
        }
        notify();
    }

    void clear() { { auto lock = priorityLock(); clearLocked(); } notify(); }

    class Feed {
    public:
        explicit Feed(Console& c) : con(c) {}
        char* space(size_t& capacity) {
            if (!buf || CHUNK - used < MIN_READ) { buf = newChunk(); base = static_cast<char*>(buf.get()); used = 0; }
            capacity = (std::min)(CHUNK - used, MAX_READ);
            return base + used;
        }
        void commit(size_t n) { con.adopt(base + used, n, buf); used += n; }
        std::shared_ptr<void> take(size_t n) { used += n; return buf; }
    private:
        Console& con;
        std::shared_ptr<void> buf;
        char* base = nullptr;
        size_t used = 0;
    };

    uint64_t totalBytes() const { return end; }

    struct View { uint64_t line = 0; int skip = 0; bool follow = true; };
    View view;

    struct Frame {
        int cols = 0, rows = 0;
        std::vector<uint32_t> cells;
        std::vector<vt::Attr> attrs;
        std::vector<std::u32string> clusters;
        std::vector<uint8_t> wrapped;
        int cursorRow = -1, cursorCol = -1;
        int markRow = -1, markCol = -1;
        double thumbPos = 1, thumbSize = 1;
        bool blinking = false;
        std::vector<uint32_t> palette;
        uint32_t fg = vt::COLOR_DEFAULT, bg = vt::COLOR_DEFAULT, cursorColor = vt::COLOR_DEFAULT;
        std::string pointer;
        std::vector<Picture> pictures;
        int nextFrameMs = -1;
        struct ExtraCursor { int32_t cell, shape; };
        std::vector<ExtraCursor> extraCursors;
        uint32_t extraTextColor = vt::COLOR_DEFAULT, extraCursorColor = vt::COLOR_DEFAULT;

        std::string text(uint32_t id) const {
            std::string out;
            if (id == WIDE_TAIL) return out;
            if (id >= CLUSTER_BASE && clusters[id - CLUSTER_BASE][0] < SIZED_ROW) { for (char32_t cp : clusters[id - CLUSTER_BASE]) if (cp < SIZED) unicode::appendUtf8(out, cp); }
            else unicode::appendUtf8(out, id);
            return out;
        }
    };

    void layout(Frame& f, int cols, int rows, const std::string& input, int inputCursor = -1) {
        auto lock = priorityLock();
        int stride = strideFor(cols);
        f.cols = cols;
        f.rows = rows;
        f.cells.assign((size_t)cols * rows, ' ');
        f.attrs.assign((size_t)cols * rows, vt::Attr());
        f.wrapped.assign((size_t)rows, 0);
        f.clusters.clear();
        f.blinking = false;
        f.palette.clear();
        f.fg = f.bg = f.cursorColor = vt::COLOR_DEFAULT;
        f.pointer.clear();
        f.pictures.clear();
        f.extraCursors.clear();
        f.nextFrameMs = -1;
        std::unordered_map<std::u32string, uint32_t> interned;
        tail = input.empty() ? nullptr : &input;
        cursorTarget = tailText() == &screenTail ? -1 : (int64_t)(end + (inputCursor < 0 ? input.size() : (size_t)inputCursor));
        uint64_t first = firstOffset();
        if (view.line < first) { view.line = first; view.skip = 0; }
        if (view.follow) anchorAtEnd(cols, rows);

        int row = -view.skip;
        uint64_t g = view.line;
        vt::Attr attr = attrAt(g);
        f.cursorRow = f.cursorCol = -1;
        f.markRow = f.markCol = -1;
        auto put = [&](int col, const char32_t* s, int n, int width, const vt::Attr& a) {
            int y = row + floorDiv(col, stride), x = floorMod(col, stride);
            if (y < 0 || y >= rows || x >= cols) return;
            uint32_t id = s[0];
            if (n > 1) {
                std::u32string key(s, n);
                auto it = interned.find(key);
                if (it == interned.end()) {
                    it = interned.emplace(key, CLUSTER_BASE + (uint32_t)f.clusters.size()).first;
                    f.clusters.push_back(std::move(key));
                }
                id = it->second;
            }
            size_t i = (size_t)y * cols + x;
            f.cells[i] = id;
            f.attrs[i] = a;
            if (width == 2 && x + 1 < cols) { f.cells[i + 1] = WIDE_TAIL; f.attrs[i + 1] = a; }
            if (a.flags & vt::BLINK) f.blinking = true;
        };
        for (int walked = 0; row < rows && walked < MAX_WALK_LINES + rows; ++walked) {
            LineEnd e;
            cursorFound = markFound = INT_MIN;
            uint64_t next = walkLine(g, cols, stride, row, put, e, attr);
            if (markFound != INT_MIN) {
                int mr = row + floorDiv(markFound, stride);
                if (mr >= 0 && mr < rows) { f.markRow = mr; f.markCol = floorMod(markFound, stride); }
            }
            if (stride == cols)
                for (int y = (std::max)(row, 0), last = (std::min)(row + e.rows(stride) - 1, rows); y < last; ++y) f.wrapped[y] = 1;
            if (e.last) {
                int at = cursorFound != INT_MIN ? cursorFound : e.col;
                int cr = row + floorDiv(at, stride), cc = floorMod(at, stride);
                if (at == e.col && e.pendingWrap) { cr -= 1; cc = cols - 1; }
                if (cr >= 0 && cr < rows && cc < cols) { f.cursorRow = cr; f.cursorCol = cc; }
                break;
            }
            row += e.rows(stride);
            g = next;
        }

        uint64_t span = (std::max)(tailEnd() - first, (uint64_t)1);
        f.thumbSize = std::clamp((double)cols * rows / span, 0.02, 1.0);
        f.thumbPos = view.follow ? 1.0 : std::clamp((double)(view.line - first) / span, 0.0, 1.0);
        tail = nullptr;
        cursorTarget = -1;
    }

    void freeze(int cols, int rows) {
        auto lock = priorityLock();
        if (view.follow) anchorAtEnd(cols, rows);
        view.follow = false;
    }

    int scroll(int delta, int cols, int rows) {
        auto lock = priorityLock();
        if (view.follow) anchorAtEnd(cols, rows);
        view.follow = false;
        int stride = strideFor(cols);
        uint64_t first = firstOffset();
        int asked = delta;
        while (delta < 0) {
            if (view.skip > 0) { int k = (std::min)(view.skip, -delta); view.skip -= k; delta += k; continue; }
            if (view.line <= first) break;
            view.line = prevLineStart(view.line);
            view.skip = (std::max)(0, lineRows(view.line, cols, stride) - 1);
            delta += 1;
        }
        while (delta > 0) {
            LineEnd e;
            vt::Attr a;
            uint64_t next = walkLine(view.line, cols, stride, INT_MIN, ignoreCell, e, a);
            if (view.skip + 1 < e.rows(stride)) ++view.skip;
            else if (!e.last) { view.line = next; view.skip = 0; }
            else break;
            --delta;
        }
        snapToFollow(cols, rows);
        return asked - delta;
    }

    void seek(double f, int cols, int rows) {
        auto lock = priorityLock();
        uint64_t first = firstOffset();
        uint64_t g = first + (uint64_t)(std::clamp(f, 0.0, 1.0) * (end - first));
        view.line = g >= end ? end : lineStartAt(g);
        view.skip = 0;
        view.follow = false;
        snapToFollow(cols, rows);
    }

    void follow() { auto lock = priorityLock(); view.follow = true; }

    void setScreenTail(std::string text) { auto lock = priorityLock(); screenTail = std::move(text); }

    static constexpr uint64_t NOT_FOUND = UINT64_MAX;

    uint64_t find(const std::string& needle, uint64_t from, bool older) {
        auto lock = priorityLock();
        size_t n = needle.size();
        if (!n) return NOT_FOUND;
        auto lower = [](int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; };
        std::string want;
        for (char c : needle) want += (char)lower((unsigned char)c);
        uint64_t first = firstOffset(), found = NOT_FOUND;
        std::string ring(n, '\0');
        Reader r(*this, first);
        uint64_t g = first;
        for (int c; (c = r.next()) >= 0; ++g) {
            ring[(size_t)(g % n)] = (char)lower(c);
            if (g + 1 < first + n) continue;
            uint64_t start = g + 1 - n;
            if (older ? start >= from : start <= from) { if (older) break; continue; }
            bool hit = true;
            for (size_t k = 0; k < n && hit; ++k) hit = ring[(size_t)((start + k) % n)] == want[k];
            if (!hit) continue;
            found = start;
            if (!older) break;
        }
        return found;
    }

    void reveal(uint64_t g, int cols, int rows) {
        {
            auto lock = priorityLock();
            view.line = lineStartAt(g);
            view.skip = 0;
            view.follow = false;
            markTarget = (int64_t)g;
        }
        scroll(-(rows / 3), cols, rows);
        auto lock = priorityLock();
        keepAboveBottom(cols, rows);
    }

    void clearMark() { auto lock = priorityLock(); markTarget = -1; }

    void markOutput() {
        auto lock = priorityLock();
        if (outputs.empty() || outputs.back() < end) outputs.push_back(end);
    }

    std::string lastOutput() {
        auto lock = priorityLock();
        if (outputs.empty()) return "";
        uint64_t from = outputs.back(), to = end;
        auto next = std::upper_bound(prompts.begin(), prompts.end(), from);
        if (next != prompts.end()) to = *next;
        std::string text;
        Reader r(*this, (std::max)(from, firstOffset()));
        vt::EscapeParser ep;
        vt::Attr ignored;
        for (uint64_t p = r.pos(); p < to; ++p) {
            int b = r.next();
            if (b < 0) break;
            if (ep.active()) ep.feed((unsigned char)b, ignored);
            else if (b == 0x1B) ep.begin();
            else if (b != '\r') text += (char)b;
        }
        while (!text.empty() && text.back() == '\n') text.pop_back();
        return text;
    }

    void markPrompt() {
        auto lock = priorityLock();
        uint64_t at = lineStartAt(end);
        if (prompts.empty() || prompts.back() < at) prompts.push_back(at);
    }

    bool jumpPrompt(bool older, int cols, int rows) {
        uint64_t target;
        {
            auto lock = priorityLock();
            if (view.follow) anchorAtEnd(cols, rows);
            uint64_t top = view.line;
            auto at = older ? std::lower_bound(prompts.begin(), prompts.end(), top) : std::upper_bound(prompts.begin(), prompts.end(), top);
            if (!older) while (at != prompts.end() && lineStartAt(*at) <= top) ++at;
            if (older ? at == prompts.begin() : at == prompts.end()) return false;
            target = older ? *(at - 1) : *at;
            view.line = lineStartAt(target);
            view.skip = 0;
            keepAboveBottom(cols, rows);
        }
        ++generation;
        return true;
    }

    static constexpr char32_t SIZED = 0x110000, SIZED_ROW = 0x150000;

    struct Sizing {
        int w = 1, s = 1, n = 0, d = 0, v = 0, h = 0, row = 0;
        bool scaled() const { return s > 1 || d; }
        char32_t head() const { return SIZED + (char32_t)(w | s << 3 | n << 6 | d << 10 | v << 14 | h << 16); }
    };

    static bool sizingOf(const char32_t* s, size_t n, Sizing& z) {
        size_t at = s[0] >= SIZED_ROW && s[0] < SIZED_ROW + 8 && n > 1 ? 1 : 0;
        if (s[at] <= SIZED || s[at] >= SIZED_ROW) return false;
        uint32_t bits = (uint32_t)(s[at] - SIZED);
        z.w = bits & 7;
        z.s = (std::max)(1u, bits >> 3 & 7);
        z.n = bits >> 6 & 15;
        z.d = bits >> 10 & 15;
        z.v = bits >> 14 & 3;
        z.h = bits >> 16 & 3;
        z.row = at ? (int)(s[0] - SIZED_ROW) : 0;
        return true;
    }

    static int clusterWidth(const char32_t* s, size_t n) {
        if (Sizing z; sizingOf(s, n, z)) return z.w * z.s;
        if (unicode::isRegionalIndicator(s[0])) return 2;
        for (size_t k = 1; k < n; ++k) if (s[k] == unicode::VS_EMOJI) return 2;
        return unicode::isWide(s[0]) ? 2 : 1;
    }

    std::unique_lock<std::mutex> priorityLock() {
        waiting.fetch_add(1, std::memory_order_relaxed);
        for (int i = 0; !mutex.try_lock(); ++i) { if (i < 20000) simd::pause(); else std::this_thread::yield(); }
        waiting.fetch_sub(1, std::memory_order_relaxed);
        return std::unique_lock<std::mutex>(mutex, std::adopt_lock);
    }

    std::atomic<uint64_t> generation{ 0 };

private:
    struct Segment {
        const char* base;
        size_t size;
        uint64_t start;
        std::shared_ptr<void> keep;
        bool live;
    };
    struct Mark { uint64_t at; vt::Attr attr; bool dense; };

    std::deque<Segment> segments;
    std::vector<Mark> marks;
    std::vector<uint64_t> prompts, outputs;
    std::shared_ptr<void> own;
    char* ownBase = nullptr;
    size_t ownUsed = CHUNK;
    size_t ownedBytes = 0;
    uint64_t end = 0;
    bool pendingCR = false;
    bool tailLive = false;
    vt::Attr running;
    vt::EscapeParser parser;
    const std::string* tail = nullptr;
    std::string screenTail;
    int64_t cursorTarget = -1, markTarget = -1;
    mutable int markFound = INT_MIN;

    const std::string* tailText() const { return tail ? tail : screenTail.empty() ? nullptr : &screenTail; }
    uint64_t tailEnd() const { const std::string* t = tailText(); return end + (t ? t->size() : 0); }
    mutable int cursorFound = INT_MIN;
    std::mutex mutex;
    std::atomic<int> waiting{ 0 };

    static void ignoreCell(int, const char32_t*, int, int, const vt::Attr&) {}
    static int floorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
    static int floorMod(int a, int b) { return a - floorDiv(a, b) * b; }
    struct LineEnd {
        int col = 0;
        bool pendingWrap = false;
        bool last = false;
        int rows(int stride) const { return floorDiv(col, stride) - (pendingWrap ? 1 : 0) + 1; }
    };
    int strideFor(int cols) const { return lineWrap ? cols : NO_WRAP_STRIDE; }

    void notify() { if (onChange) onChange(); }

    uint64_t firstOffset() const { return segments.empty() ? end : segments.front().start; }

    static const unsigned char* plainEnd(const unsigned char* s, const unsigned char* e) {
        if (s >= e) return e;
        if (simd::wideFast()) return plainEnd32(s, e);
        using simd::U8x16;
        const U8x16 cr = U8x16::splat('\r'), bs = U8x16::splat('\b'), ff = U8x16::splat('\f'), esc = U8x16::splat(0x1B);
        while (e - s >= 16) {
            U8x16 v = U8x16::load(s);
            if (simd::Mask16 m = ((v == cr) | (v == bs) | (v == ff) | (v == esc)).mask()) return s + m.first();
            s += 16;
        }
        while (s < e && *s != '\r' && *s != '\b' && *s != '\f' && *s != 0x1B) ++s;
        return s;
    }

    CCC_AVX2 static const unsigned char* plainEnd32(const unsigned char* s, const unsigned char* e) {
        using simd::U8x32;
        const U8x32 cr = U8x32::splat('\r'), bs = U8x32::splat('\b'), ff = U8x32::splat('\f'), esc = U8x32::splat(0x1B);
        auto special = [&](const unsigned char* a) CCC_AVX2 {
            U8x32 v = U8x32::loadAligned(a);
            return ((v == cr) | (v == bs) | (v == ff) | (v == esc)).mask();
        };
        const unsigned char* a = reinterpret_cast<const unsigned char*>(reinterpret_cast<uintptr_t>(s) & ~uintptr_t(31));
        simd::Mask32 m = special(a) >> (size_t)(s - a);
        if (m) return (std::min)(s + m.first(), e);
        for (a += 32; a < e; a += 32)
            if ((m = special(a))) return (std::min)(a + m.first(), e);
        return e;
    }

    void ingest(const char* data, size_t n, const std::shared_ptr<void>& keep) {
        const unsigned char* s = reinterpret_cast<const unsigned char*>(data);
        const unsigned char* e = s + n;
        const unsigned char* from = s;
        auto flush = [&](const unsigned char* to) { if (to > from) appendView(from, to - from, keep); from = to; };
        while (s < e) {
            if (pendingCR && *s != '\r') {
                if (*s == '\n') pendingCR = false;
                else { flush(s); cutLine(); }
            }
            if (parser.active()) {
                vt::EscapeParser::Action action;
                uint16_t link = running.link;
                s = parser.run(s, e, running, action);
                if (char mark = std::exchange(parser.mark, '\0'); mark == 'A' || mark == 'C') (mark == 'A' ? prompts : outputs).push_back(end + (s - from));
                if (action == vt::EscapeParser::STYLE || running.link != link) markAt(end + (s - from));
                else if (action == vt::EscapeParser::COMMAND && parser.command == 'J' && parser.param(0, 0) >= 2) { flush(s); clearLocked(); }
                continue;
            }
            const unsigned char* r = plainEnd(s, e);
            if (r - s >= 64 && !marks.empty() && marks.back().dense) marks.push_back({ end + (s - from), running, false });
            s = r;
            if (s == e) break;
            switch (*s) {
            case '\r': pendingCR = true; break;
            case 0x1B:
                if (const unsigned char* q = vt::sgrEnd(s, e); q && q - s <= 64) {
                    vt::applySgrSequence(running, s, q);
                    s = q;
                    markAt(end + (s - from));
                    continue;
                }
                parser.begin();
                break;
            case '\b': flush(s); from = s + 1; backspace(); break;
            case '\f': flush(s); from = s + 1; clearLocked(); break;
            }
            ++s;
        }
        flush(e);
        ++generation;
        trim();
    }

    void appendView(const unsigned char* p, size_t n, const std::shared_ptr<void>& keep) {
        const char* c = reinterpret_cast<const char*>(p);
        if (tailLive && !segments.empty() && segments.back().keep == keep && segments.back().base + segments.back().size == c)
            segments.back().size += n;
        else {
            segments.push_back({ c, n, end, keep, true });
            tailLive = true;
        }
        end += n;
        ownedBytes += n;
    }

    vt::Attr attrAt(uint64_t g) const {
        size_t lo = 0, hi = marks.size();
        while (lo < hi) { size_t mid = (lo + hi) / 2; if (marks[mid].at <= g) lo = mid + 1; else hi = mid; }
        if (!lo) return vt::Attr();
        const Mark& m = marks[lo - 1];
        vt::Attr a = m.attr;
        if (!m.dense) return a;
        Reader r(*this, (std::max)(m.at, firstOffset()));
        vt::EscapeParser ep;
        for (uint64_t p = r.pos(); p < g; ++p) {
            int d = r.next();
            if (d < 0) break;
            if (ep.active()) ep.feed((unsigned char)d, a);
            else if (d == 0x1B) ep.begin();
        }
        return a;
    }

    void markAt(uint64_t at) {
        if (!marks.empty()) {
            Mark& b = marks.back();
            if (b.at == at && !b.dense) { b.attr = running; return; }
            if (!b.dense && b.attr == running) return;
            if (at - b.at < MARK_STRIDE) { b.dense = true; return; }
        }
        marks.push_back({ at, running, false });
    }

    void dropMarksPastEnd() {
        while (!marks.empty() && marks.back().at > end) marks.pop_back();
        markAt(end);
    }

    void shrinkTail(size_t n) {
        Segment& t = segments.back();
        t.size -= n;
        end -= n;
        ownedBytes -= n;
        if (t.size == 0) {
            segments.pop_back();
            tailLive = !segments.empty() && segments.back().live;
        }
    }

    void cutLine() {
        pendingCR = false;
        while (tailLive && !segments.empty()) {
            const Segment& t = segments.back();
            size_t i = t.size;
            while (i > 0 && t.base[i - 1] != '\n') --i;
            shrinkTail(t.size - i);
            if (i > 0) break;
        }
        dropMarksPastEnd();
        ++generation;
    }

    void backspace() {
        if (!tailLive || segments.empty()) return;
        const Segment& t = segments.back();
        unsigned char removed = (unsigned char)t.base[t.size - 1];
        if (removed == '\n') return;
        shrinkTail(1);
        auto lastByte = [&] { return (unsigned char)segments.back().base[segments.back().size - 1]; };
        if ((removed & 0xC0) == 0x80) {
            while (tailLive && !segments.empty() && (lastByte() & 0xC0) == 0x80) shrinkTail(1);
            if (tailLive && !segments.empty() && lastByte() >= 0xC0) shrinkTail(1);
        }
        dropMarksPastEnd();
        ++generation;
    }

    void clearLocked() {
        segments.clear();
        marks.clear();
        prompts.clear();
        outputs.clear();
        ownedBytes = 0;
        tailLive = false;
        pendingCR = false;
        markAt(end);
        view = View();
        view.line = end;
        ++generation;
    }

    void trim() {
        size_t drop = 0, bytes = ownedBytes;
        while (segments.size() - drop > 1 && (bytes > OWNED_BUDGET || segments.size() - drop > MAX_SEGMENTS)) {
            if (segments[drop].live) bytes -= segments[drop].size;
            ++drop;
        }
        if (!drop) return;
        uint64_t first = segments[drop].start;
        vt::Attr a = attrAt(first);
        segments.erase(segments.begin(), segments.begin() + drop);
        ownedBytes = bytes;
        prompts.erase(prompts.begin(), std::lower_bound(prompts.begin(), prompts.end(), first));
        outputs.erase(outputs.begin(), std::lower_bound(outputs.begin(), outputs.end(), first));
        auto last = std::upper_bound(marks.begin(), marks.end(), first, [](uint64_t g, const Mark& m) { return g < m.at; });
        if (last == marks.begin()) return;
        *(last - 1) = { first, a, false };
        if (marks.size() > 1024 && marks[marks.size() / 2].at <= first) marks.erase(marks.begin(), last - 1);
    }

    size_t segmentAt(uint64_t g) const {
        size_t lo = 0, hi = segments.size();
        while (hi - lo > 1) { size_t mid = (lo + hi) / 2; if (segments[mid].start <= g) lo = mid; else hi = mid; }
        return lo;
    }

    struct Reader {
        const Console* c;
        size_t seg = 0;
        size_t off = 0;
        size_t tailPos = 0;
        bool inTail = false;
        Reader(const Console& con, uint64_t g) : c(&con) {
            if (g >= c->end || c->segments.empty()) { inTail = true; tailPos = (size_t)(g - c->end); return; }
            seg = c->segmentAt(g);
            off = (size_t)(g - c->segments[seg].start);
        }
        uint64_t pos() const {
            if (!inTail && seg < c->segments.size()) return c->segments[seg].start + off;
            return c->end + tailPos;
        }
        int next() {
            while (!inTail) {
                if (seg >= c->segments.size()) { inTail = true; break; }
                const Segment& s = c->segments[seg];
                if (off < s.size) return (unsigned char)s.base[off++];
                ++seg;
                off = 0;
            }
            const std::string* t = c->tailText();
            if (t && tailPos < t->size()) return (unsigned char)(*t)[tailPos++];
            return -1;
        }
        int32_t nextCodepoint(int& bytes) {
            int c0 = next();
            if (c0 < 0) return -1;
            ++bytes;
            if (c0 < 0x80) return c0;
            int need = c0 >= 0xF0 && c0 < 0xF8 ? 3 : c0 >= 0xE0 ? 2 : c0 >= 0xC0 ? 1 : 0;
            if (need) {
                Reader look = *this;
                uint32_t cp = c0 & (0x3F >> need);
                int k = 0;
                for (; k < need; ++k) { int d = look.next(); if (d < 0x80 || d > 0xBF) break; cp = cp << 6 | (d & 0x3F); }
                if (k == need) { *this = look; bytes += need; return (int32_t)cp; }
            }
            return (int32_t)unicode::oemByte((unsigned char)c0);
        }
    };

    int byteAt(uint64_t g) const {
        if (g >= end) {
            const std::string* t = tailText();
            return t && g - end < t->size() ? (unsigned char)(*t)[(size_t)(g - end)] : -1;
        }
        const Segment& s = segments[segmentAt(g)];
        return (unsigned char)s.base[g - s.start];
    }

    uint64_t lineStartAt(uint64_t g) const {
        uint64_t first = firstOffset();
        uint64_t limit = g > first + MAX_LINE ? g - MAX_LINE : first;
        while (g > limit && byteAt(g - 1) != '\n') --g;
        return g;
    }

    uint64_t prevLineStart(uint64_t line) const {
        uint64_t first = firstOffset();
        if (line <= first) return first;
        return lineStartAt(line - 1);
    }

    int lineRows(uint64_t g, int cols, int stride) const {
        LineEnd e;
        vt::Attr a;
        walkLine(g, cols, stride, INT_MIN, ignoreCell, e, a);
        return e.rows(stride);
    }

    static bool attaches(uint32_t prev, uint32_t cp, bool afterJoiner) {
        return afterJoiner || unicode::isZeroWidth(cp) || cp == unicode::ZWJ || (unicode::isSkinTone(cp) && unicode::isWide(prev));
    }

    template <class Put>
    void eraseInLine(int mode, int col, int cols, int stride, Put& put, const vt::Attr& attr) const {
        static const char32_t space = ' ';
        int base = floorDiv(col, stride) * stride, x = floorMod(col, stride);
        int from = mode == 0 ? x : 0, to = mode == 1 ? x + 1 : cols;
        for (int k = from; k < (std::min)(to, cols); ++k) put(base + k, &space, 1, 1, attr);
    }

    void moveCursor(const vt::EscapeParser& ep, int& col, int cols, int stride, int lineRow) const {
        int n = ep.param(0, 1);
        int x = floorMod(col, stride), y = floorDiv(col, stride);
        switch (ep.command) {
        case 'A': col -= n * stride; break;
        case 'B': col += n * stride; break;
        case 'C': col = y * stride + (std::min)(x + n, (std::max)(cols - 1, x)); break;
        case 'D': col = y * stride + (std::max)(0, x - n); break;
        case 'E': col = (y + n) * stride; break;
        case 'F': col = (y - n) * stride; break;
        case 'G': col = y * stride + (std::max)(0, n - 1); break;
        case 'H': case 'f':
            if (lineRow != INT_MIN) col = (ep.param(0, 1) - 1 - lineRow) * stride + (std::max)(0, ep.param(1, 1) - 1);
            break;
        case 'd':
            if (lineRow != INT_MIN) col = (n - 1 - lineRow) * stride + x;
            break;
        }
    }

    template <class Put>
    uint64_t walkLine(uint64_t g, int cols, int stride, int lineRow, Put put, LineEnd& e, vt::Attr& attr) const {
        Reader r(*this, g);
        int col = 0, bytes = 0;
        bool pending = false;
        auto settle = [&] { if (pending) { col -= 1; pending = false; } };
        char32_t cluster[MAX_CLUSTER];
        for (;;) {
            if (cursorTarget >= 0 && cursorFound == INT_MIN && r.pos() == (uint64_t)cursorTarget) cursorFound = col;
            if (markTarget >= 0 && markFound == INT_MIN && r.pos() == (uint64_t)markTarget) markFound = col;
            int32_t c = r.nextCodepoint(bytes);
            if (c < 0) { e.last = true; break; }
            if (c == '\n') break;
            if (c == '\r') { settle(); col = floorDiv(col, stride) * stride; continue; }
            if (c == '\b') { settle(); if (floorMod(col, stride) > 0) --col; continue; }
            if (c == 0x1B) {
                vt::EscapeParser ep;
                ep.begin();
                int d = 0;
                vt::EscapeParser::Action action = vt::EscapeParser::NONE;
                while (ep.active() && (d = r.next()) >= 0) { ++bytes; action = ep.feed((unsigned char)d, attr); }
                if (d < 0) { e.last = true; break; }
                if (action == vt::EscapeParser::COMMAND) {
                    settle();
                    if (ep.command == 'K') eraseInLine(ep.param(0, 0), col, cols, stride, put, attr);
                    else moveCursor(ep, col, cols, stride, lineRow);
                }
                continue;
            }
            if (c == 0x1A) {
                static const char32_t caret = '^', zed = 'Z';
                put(col++, &caret, 1, 1, attr);
                put(col++, &zed, 1, 1, attr);
                pending = floorMod(col, stride) == 0;
                continue;
            }
            if (c == '\t') {
                int x = floorMod(col, stride), to = (x / 8 + 1) * 8;
                static const char32_t space = ' ';
                for (; x < to; ++x) put(col++, &space, 1, 1, attr);
                pending = floorMod(col, stride) == 0;
                continue;
            }
            int n = 0;
            cluster[n++] = c < 0x20 ? ' ' : c == 0x7F || (c >= 0x80 && c < 0xA0) ? '?' : (char32_t)c;
            bool joiner = false;
            for (;;) {
                Reader look = r;
                int lookBytes = 0;
                int32_t d = look.nextCodepoint(lookBytes);
                if (d < 0x300 && !(d >= 0x80 && joiner)) break;
                bool pairRI = n == 1 && unicode::isRegionalIndicator(cluster[0]) && unicode::isRegionalIndicator((uint32_t)d);
                if (!pairRI && !attaches(cluster[0], (uint32_t)d, joiner)) break;
                r = look;
                bytes += lookBytes;
                if (n < MAX_CLUSTER) cluster[n++] = (char32_t)d;
                joiner = d == unicode::ZWJ;
                if (pairRI) break;
            }
            if (n == 1 && unicode::isZeroWidth(cluster[0])) continue;
            int w = clusterWidth(cluster, n);
            if (w == 2 && stride == cols && floorMod(col, stride) == cols - 1) ++col;
            put(col, cluster, n, w, attr);
            col += w;
            pending = floorMod(col, stride) == 0;
            if (bytes >= MAX_LINE) break;
        }
        e.col = col;
        e.pendingWrap = pending;
        return g + bytes;
    }

    void anchorAtEnd(int cols, int rows) {
        int stride = strideFor(cols);
        uint64_t first = firstOffset(), last = tailEnd();
        uint64_t line = last > first ? lineStartAt(last) : last;
        int need = rows;
        for (int walked = 0;; ++walked) {
            int n = lineRows(line, cols, stride);
            if (n >= need) { view.line = line; view.skip = n - need; break; }
            need -= n;
            if (line <= first || walked > MAX_WALK_LINES) { view.line = line; view.skip = 0; break; }
            line = prevLineStart(line);
        }
    }

    View bottomView(int cols, int rows) {
        View v = view;
        anchorAtEnd(cols, rows);
        std::swap(v, view);
        return v;
    }

    static bool above(const View& a, const View& b) { return a.line < b.line || (a.line == b.line && a.skip < b.skip); }

    void snapToFollow(int cols, int rows) { if (!above(view, bottomView(cols, rows))) view.follow = true; }

    void keepAboveBottom(int cols, int rows) {
        View bottom = bottomView(cols, rows);
        if (!above(view, bottom)) view = bottom;
        view.follow = false;
    }
};
