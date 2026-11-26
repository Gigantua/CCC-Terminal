#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include "base64.h"
#include "bitmap.h"
#include "inflate.h"
#include "png.h"
#include "os.h"
#include "vt.h"

namespace graphics {

constexpr size_t STORE_BYTES = 128 << 20, MAX_PIXELS = 1 << 26, MAX_PAYLOAD = 512 << 20;

struct Command {
    char action = 't', medium = 'd', target = 'a', compression = 0;
    uint32_t format = 32, id = 0, number = 0, placement = 0;
    int width = 0, height = 0, x = 0, y = 0, w = 0, h = 0, offX = 0, offY = 0, cols = 0, rows = 0, z = 0;
    int quiet = 0, more = 0, keepCursor = 0, size = 0, offset = 0, virtualPlacement = 0;

    static Command parse(const std::string& control) {
        Command c;
        size_t at = 0;
        while (at < control.size()) {
            size_t comma = control.find(',', at);
            if (comma == std::string::npos) comma = control.size();
            if (comma > at + 1 && control[at + 1] == '=') c.set(control[at], control.substr(at + 2, comma - at - 2));
            at = comma + 1;
        }
        return c;
    }

    void set(char key, const std::string& v) {
        long long n = strtoll(v.c_str(), nullptr, 10);
        static const std::pair<char, char Command::*> chars[] = { { 'a', &Command::action }, { 't', &Command::medium }, { 'd', &Command::target }, { 'o', &Command::compression } };
        static const std::pair<char, uint32_t Command::*> ids[] = { { 'f', &Command::format }, { 'i', &Command::id }, { 'I', &Command::number }, { 'p', &Command::placement } };
        static const std::pair<char, int Command::*> ints[] = {
            { 's', &Command::width }, { 'v', &Command::height }, { 'x', &Command::x }, { 'y', &Command::y }, { 'w', &Command::w }, { 'h', &Command::h },
            { 'X', &Command::offX }, { 'Y', &Command::offY }, { 'c', &Command::cols }, { 'r', &Command::rows }, { 'z', &Command::z }, { 'q', &Command::quiet },
            { 'm', &Command::more }, { 'C', &Command::keepCursor }, { 'S', &Command::size }, { 'O', &Command::offset }, { 'U', &Command::virtualPlacement },
        };
        for (auto& [k, f] : chars) if (k == key) this->*f = v.empty() ? 0 : v[0];
        for (auto& [k, f] : ids) if (k == key) this->*f = (uint32_t)n;
        for (auto& [k, f] : ints) if (k == key) this->*f = (int)n;
    }
};

struct Frame {
    std::shared_ptr<const Bitmap> bitmap;
    int gap = 40;
};

struct Image {
    enum State { STOPPED = 1, LOADING = 2, RUNNING = 3 };
    uint32_t id = 0, number = 0;
    uint64_t age = 0;
    std::shared_ptr<const Bitmap> bitmap;
    std::vector<Frame> frames;
    int rootGap = 0, state = STOPPED, current = 1, loops = 1, loopsLeft = 0;
    double shownAt = 0;

    int count() const { return 1 + (int)frames.size(); }
    const std::shared_ptr<const Bitmap>& frame(int n) const { return n <= 1 ? bitmap : frames[(size_t)n - 2].bitmap; }
    int gap(int n) const { return n <= 1 ? rootGap : frames[(size_t)n - 2].gap; }
    const std::shared_ptr<const Bitmap>& shown() const { return frame(current); }
    size_t bytes() const {
        size_t n = bitmap ? bitmap->pixels.size() * 4 : 0;
        for (const Frame& f : frames) n += f.bitmap->pixels.size() * 4;
        return n;
    }
};

struct Placement {
    uint32_t image = 0, id = 0;
    int col = 0, row = 0, cols = 0, rows = 0, spanCols = 1, spanRows = 1;
    int x = 0, y = 0, w = 0, h = 0, offX = 0, offY = 0, z = 0;
    bool virtualOnly = false;
    std::shared_ptr<const Bitmap> bitmap;
};

struct Effect {
    std::string reply;
    bool placed = false, moveCursor = false;
    int cols = 0, rows = 0;
};

struct Cursor { int col = 0, row = 0, cellW = 1, cellH = 1, screen = 0; };

class Store {
public:
    std::vector<Placement> placements[2];

    Effect handle(const std::string& body, const Cursor& at) {
        size_t semi = body.find(';');
        std::string control = body.substr(0, semi), payload = semi == std::string::npos ? std::string() : body.substr(semi + 1);
        Command c = Command::parse(control);
        if (chunking) {
            if (data.size() + payload.size() <= MAX_PAYLOAD) data += payload;
            else overflow = true;
            if (c.more) return {};
            chunking = false;
            c = pending;
            payload.swap(data);
            data.clear();
            if (std::exchange(overflow, false)) return answer(c, "EFBIG:payload too large");
        }
        else if (c.more && (c.action == 't' || c.action == 'T' || c.action == 'q' || c.action == 'f')) {
            chunking = true;
            pending = c;
            data = payload;
            return {};
        }
        switch (c.action) {
        case 't': case 'T': case 'q': return transmit(c, payload, at);
        case 'f': return addFrame(c, payload);
        case 'a': return controlAnimation(c);
        case 'c': return composeFrames(c);
        case 'p': return place(c, at);
        case 'd': remove(c, at); return {};
        default: return answer(c, "EINVAL:unsupported action");
        }
    }

    void scroll(int screen, int top, int bottom, int n, bool wholeScreen) {
        auto& list = placements[screen];
        for (Placement& p : list)
            if (!p.virtualOnly && p.row >= top && p.row <= bottom) p.row -= n;
        list.erase(std::remove_if(list.begin(), list.end(), [&](const Placement& p) {
            if (p.virtualOnly) return false;
            return wholeScreen ? p.row + p.spanRows <= top || p.row > bottom : p.row < top || p.row > bottom;
        }), list.end());
    }

    void clearScreen(int screen) {
        auto& list = placements[screen];
        list.erase(std::remove_if(list.begin(), list.end(), [](const Placement& p) { return !p.virtualOnly; }), list.end());
    }

    void reset() {
        placements[0].clear();
        placements[1].clear();
        images.clear();
        bytes = 0;
        chunking = false;
        data.clear();
    }

    void pictures(int screen, std::vector<Picture>& out) const {
        for (const Placement& p : placements[screen]) {
            auto shown = shownOf(p);
            if (!p.virtualOnly && shown) out.push_back({ shown, p.col, p.row, p.offX, p.offY, p.cols, p.rows, p.x, p.y, p.w, p.h, p.z });
        }
    }

    int animate() {
        double now = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(), next = -1;
        for (auto& [id, img] : images) {
            if (img.state == Image::STOPPED || img.count() < 2) continue;
            for (int guard = 0; guard < 4 * img.count(); ++guard) {
                double gap = (std::max)(img.gap(img.current), 0);
                if (now - img.shownAt < gap) { double left = img.shownAt + gap - now; next = next < 0 ? left : (std::min)(next, left); break; }
                int following = img.current + 1;
                if (following > img.count()) {
                    if (img.state == Image::LOADING) break;
                    if (img.loops != 1 && --img.loopsLeft <= 0) { img.state = Image::STOPPED; break; }
                    following = 1;
                }
                img.shownAt = gap ? img.shownAt + gap : now;
                img.current = following;
            }
        }
        return next < 0 ? -1 : (int)std::ceil(next);
    }

    template <class CellAt>
    void placeholderPictures(int screen, int rows, int cols, int cellW, int cellH, CellAt cellAt, std::vector<Picture>& out) const {
        if (std::none_of(placements[screen].begin(), placements[screen].end(), [](const Placement& p) { return p.virtualOnly; })) return;
        for (int y = 0; y < rows; ++y) {
            Run run, prev;
            for (int x = 0; x <= cols; ++x) {
                Run cell;
                std::u32string text;
                vt::Attr a;
                if (x < cols) text = cellAt(x, y, a);
                if (!text.empty() && text[0] == PLACEHOLDER && colorId(a.fg, cell.image)) {
                    colorId(a.underlineColor, cell.placement);
                    int row = text.size() > 1 ? diacriticIndex(text[1]) : -1, col = text.size() > 2 ? diacriticIndex(text[2]) : -1;
                    int high = text.size() > 3 ? diacriticIndex(text[3]) : -1;
                    bool follows = prev.n && prev.x + prev.n == x && prev.image == cell.image && prev.placement == cell.placement;
                    int prevCol = prev.col + prev.n - 1;
                    if (follows && row < 0) { row = prev.row; col = prevCol + 1; high = prev.high; }
                    else if (follows && col < 0 && row == prev.row) { col = prevCol + 1; high = prev.high; }
                    else if (follows && high < 0 && row == prev.row && col == prevCol + 1) high = prev.high;
                    cell.row = (std::max)(row, 0);
                    cell.col = (std::max)(col, 0);
                    cell.high = (std::max)(high, 0);
                    cell.x = x;
                    cell.n = 1;
                }
                if (cell.n && run.n && run.image == cell.image && run.placement == cell.placement && run.high == cell.high && run.row == cell.row &&
                    run.col + run.n == cell.col && run.x + run.n == x) { ++run.n; prev = run; continue; }
                if (run.n) emitRun(screen, run, y, cellW, cellH, out);
                run = cell;
                prev = cell;
            }
        }
    }

private:
    static constexpr char32_t PLACEHOLDER = 0x10EEEE;

    struct Run { uint32_t image = 0, placement = 0; int row = 0, col = 0, high = 0, x = 0, n = 0; };

    static bool colorId(uint32_t color, uint32_t& id) {
        if (color & vt::COLOR_DEFAULT) return false;
        id = color & vt::COLOR_INDEXED ? color & 255 : (color & 255) << 16 | (color & 0xFF00) | (color >> 16 & 255);
        return true;
    }

    static int diacriticIndex(char32_t cp) {
        static const uint32_t marks[] = {
            0x0305, 0x030D, 0x030E, 0x0310, 0x0312, 0x033D, 0x033E, 0x033F, 0x0346, 0x034A, 0x034B, 0x034C, 0x0350, 0x0351, 0x0352, 0x0357, 0x035B, 0x0363, 0x0364,
            0x0365, 0x0366, 0x0367, 0x0368, 0x0369, 0x036A, 0x036B, 0x036C, 0x036D, 0x036E, 0x036F, 0x0483, 0x0484, 0x0485, 0x0486, 0x0487, 0x0592, 0x0593, 0x0594,
            0x0595, 0x0597, 0x0598, 0x0599, 0x059C, 0x059D, 0x059E, 0x059F, 0x05A0, 0x05A1, 0x05A8, 0x05A9, 0x05AB, 0x05AC, 0x05AF, 0x05C4, 0x0610, 0x0611, 0x0612,
            0x0613, 0x0614, 0x0615, 0x0616, 0x0617, 0x0657, 0x0658, 0x0659, 0x065A, 0x065B, 0x065D, 0x065E, 0x06D6, 0x06D7, 0x06D8, 0x06D9, 0x06DA, 0x06DB, 0x06DC,
            0x06DF, 0x06E0, 0x06E1, 0x06E2, 0x06E4, 0x06E7, 0x06E8, 0x06EB, 0x06EC, 0x0730, 0x0732, 0x0733, 0x0735, 0x0736, 0x073A, 0x073D, 0x073F, 0x0740, 0x0741,
            0x0743, 0x0745, 0x0747, 0x0749, 0x074A, 0x07EB, 0x07EC, 0x07ED, 0x07EE, 0x07EF, 0x07F0, 0x07F1, 0x07F3, 0x0816, 0x0817, 0x0818, 0x0819, 0x081B, 0x081C,
            0x081D, 0x081E, 0x081F, 0x0820, 0x0821, 0x0822, 0x0823, 0x0825, 0x0826, 0x0827, 0x0829, 0x082A, 0x082B, 0x082C, 0x082D, 0x0951, 0x0953, 0x0954, 0x0F82,
            0x0F83, 0x0F86, 0x0F87, 0x135D, 0x135E, 0x135F, 0x17DD, 0x193A, 0x1A17, 0x1A75, 0x1A76, 0x1A77, 0x1A78, 0x1A79, 0x1A7A, 0x1A7B, 0x1A7C, 0x1B6B, 0x1B6D,
            0x1B6E, 0x1B6F, 0x1B70, 0x1B71, 0x1B72, 0x1B73, 0x1CD0, 0x1CD1, 0x1CD2, 0x1CDA, 0x1CDB, 0x1CE0, 0x1DC0, 0x1DC1, 0x1DC3, 0x1DC4, 0x1DC5, 0x1DC6, 0x1DC7,
            0x1DC8, 0x1DC9, 0x1DCB, 0x1DCC, 0x1DD1, 0x1DD2, 0x1DD3, 0x1DD4, 0x1DD5, 0x1DD6, 0x1DD7, 0x1DD8, 0x1DD9, 0x1DDA, 0x1DDB, 0x1DDC, 0x1DDD, 0x1DDE, 0x1DDF,
            0x1DE0, 0x1DE1, 0x1DE2, 0x1DE3, 0x1DE4, 0x1DE5, 0x1DE6, 0x1DFE, 0x20D0, 0x20D1, 0x20D4, 0x20D5, 0x20D6, 0x20D7, 0x20DB, 0x20DC, 0x20E1, 0x20E7, 0x20E9,
            0x20F0, 0x2CEF, 0x2CF0, 0x2CF1, 0x2DE0, 0x2DE1, 0x2DE2, 0x2DE3, 0x2DE4, 0x2DE5, 0x2DE6, 0x2DE7, 0x2DE8, 0x2DE9, 0x2DEA, 0x2DEB, 0x2DEC, 0x2DED, 0x2DEE,
            0x2DEF, 0x2DF0, 0x2DF1, 0x2DF2, 0x2DF3, 0x2DF4, 0x2DF5, 0x2DF6, 0x2DF7, 0x2DF8, 0x2DF9, 0x2DFA, 0x2DFB, 0x2DFC, 0x2DFD, 0x2DFE, 0x2DFF, 0xA66F, 0xA67C,
            0xA67D, 0xA6F0, 0xA6F1, 0xA8E0, 0xA8E1, 0xA8E2, 0xA8E3, 0xA8E4, 0xA8E5, 0xA8E6, 0xA8E7, 0xA8E8, 0xA8E9, 0xA8EA, 0xA8EB, 0xA8EC, 0xA8ED, 0xA8EE, 0xA8EF,
            0xA8F0, 0xA8F1, 0xAAB0, 0xAAB2, 0xAAB3, 0xAAB7, 0xAAB8, 0xAABE, 0xAABF, 0xAAC1, 0xFE20, 0xFE21, 0xFE22, 0xFE23, 0xFE24, 0xFE25, 0xFE26, 0x10A0F, 0x10A38,
            0x1D185, 0x1D186, 0x1D187, 0x1D188, 0x1D189, 0x1D1AA, 0x1D1AB, 0x1D1AC, 0x1D1AD, 0x1D242, 0x1D243, 0x1D244,
        };
        const uint32_t* at = std::lower_bound(std::begin(marks), std::end(marks), (uint32_t)cp);
        return at != std::end(marks) && *at == cp ? (int)(at - std::begin(marks)) : -1;
    }

    void emitRun(int screen, const Run& run, int y, int cellW, int cellH, std::vector<Picture>& out) const {
        uint32_t image = run.image | (uint32_t)run.high << 24;
        const Placement* p = nullptr;
        for (const Placement& q : placements[screen])
            if (q.virtualOnly && q.image == image && q.bitmap && (!run.placement || q.id == run.placement)) p = &q;
        auto shown = p ? shownOf(*p) : nullptr;
        if (!p || !shown || p->w <= 0 || p->h <= 0) return;
        double areaW = (double)(p->cols ? p->cols : (p->w + cellW - 1) / cellW) * cellW, areaH = (double)(p->rows ? p->rows : (p->h + cellH - 1) / cellH) * cellH;
        double scale = (std::min)(areaW / p->w, areaH / p->h), drawnW = p->w * scale, drawnH = p->h * scale;
        double ox = (areaW - drawnW) / 2, oy = (areaH - drawnH) / 2;
        double x0 = (std::max)((double)run.col * cellW, ox), x1 = (std::min)((double)(run.col + run.n) * cellW, ox + drawnW);
        double y0 = (std::max)((double)run.row * cellH, oy), y1 = (std::min)((double)(run.row + 1) * cellH, oy + drawnH);
        if (x1 <= x0 || y1 <= y0) return;
        Picture pic;
        pic.bitmap = shown;
        pic.col = run.x;
        pic.row = y;
        pic.offX = (int)(x0 - (double)run.col * cellW);
        pic.offY = (int)(y0 - (double)run.row * cellH);
        pic.destW = (int)std::lround(x1 - x0);
        pic.destH = (int)std::lround(y1 - y0);
        pic.srcX = p->x + (int)((x0 - ox) / scale);
        pic.srcY = p->y + (int)((y0 - oy) / scale);
        pic.srcW = (std::max)(1, (int)std::lround((x1 - x0) / scale));
        pic.srcH = (std::max)(1, (int)std::lround((y1 - y0) / scale));
        pic.z = p->z;
        out.push_back(pic);
    }

    std::map<uint32_t, Image> images;
    size_t bytes = 0;
    uint64_t clock = 0;
    bool chunking = false, overflow = false;
    Command pending;
    std::string data;

    static Effect answer(const Command& c, const std::string& message) {
        Effect e;
        bool ok = message == "OK";
        if (!(c.id || c.number) || (ok && c.quiet >= 1) || (!ok && c.quiet >= 2)) return e;
        std::string keys;
        auto add = [&](const char* key, uint32_t v) { if (v) keys += (keys.empty() ? "" : ",") + std::string(key) + std::to_string(v); };
        add("i=", c.id);
        add("I=", c.number);
        add("p=", c.placement);
        e.reply = "\x1b_G" + keys + ";" + message + "\x1b\\";
        return e;
    }

    static std::filesystem::path utf8Path(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }

    static bool readFile(const Command& c, const std::string& payload, std::string& out) {
        std::string path = base64::decode(payload);
        std::ifstream in(utf8Path(path), std::ios::binary);
        if (!in) return false;
        if (c.offset) in.seekg(c.offset);
        if (c.size) { out.resize((size_t)c.size); in.read(out.data(), c.size); out.resize((size_t)in.gcount()); }
        else out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        in.close();
        std::error_code ec;
        if (c.medium == 't' && path.find("tty-graphics-protocol") != std::string::npos) std::filesystem::remove(utf8Path(path), ec);
        return true;
    }

    static std::string decodeBitmap(const Command& c, std::string raw, Bitmap& b) {
        if (c.compression == 'z') {
            std::string plain;
            if (!inflate::zlib(reinterpret_cast<const uint8_t*>(raw.data()), raw.size(), plain, MAX_PIXELS * 4)) return "EINVAL:zlib data could not be decompressed";
            raw.swap(plain);
        }
        if (c.format == 100) {
            png::Picture p;
            if (!png::decode(raw, p, MAX_PIXELS)) return "EBADPNG:PNG data could not be decoded";
            b.width = p.width;
            b.height = p.height;
            b.pixels.swap(p.pixels);
            return "";
        }
        if (c.format != 24 && c.format != 32) return "EINVAL:unknown format";
        if (c.width <= 0 || c.height <= 0 || (size_t)c.width * c.height > MAX_PIXELS) return "EINVAL:image size missing or too large";
        size_t channels = c.format / 8, count = (size_t)c.width * c.height;
        if (raw.size() < count * channels) return "ENODATA:insufficient image data";
        b.width = c.width;
        b.height = c.height;
        b.pixels.resize(count);
        const uint8_t* p = reinterpret_cast<const uint8_t*>(raw.data());
        if (channels == 4) std::memcpy(b.pixels.data(), p, count * 4);
        else for (size_t i = 0; i < count; ++i, p += 3) b.pixels[i] = p[0] | p[1] << 8 | p[2] << 16 | 0xFF000000u;
        return "";
    }

    uint32_t freeId() const {
        uint32_t id = images.empty() ? 1 : images.rbegin()->first + 1;
        while (!id || images.count(id)) ++id;
        return id;
    }

    static std::string readPayload(const Command& c, const std::string& payload, std::string& raw) {
        if (c.medium == 'd') raw = base64::decode(payload);
        else if (c.medium == 'f' || c.medium == 't') { if (!readFile(c, payload, raw)) return "ENOENT:file could not be read"; }
        else if (c.medium == 's') { if (!os::readSharedMemory(base64::decode(payload), (size_t)c.offset, (size_t)c.size, raw)) return "ENOENT:shared memory could not be read"; }
        else return "EINVAL:unsupported transmission medium";
        return "";
    }

    std::shared_ptr<const Bitmap> shownOf(const Placement& p) const {
        auto it = images.find(p.image);
        return it == images.end() ? p.bitmap : it->second.shown();
    }

    Image* imageFor(const Command& c) {
        Image* img = nullptr;
        if (c.id) { auto it = images.find(c.id); if (it != images.end()) img = &it->second; }
        else if (c.number) for (auto& kv : images) if (kv.second.number == c.number && (!img || kv.second.age > img->age)) img = &kv.second;
        return img && img->bitmap ? img : nullptr;
    }

    static void blend(Bitmap& dst, const Bitmap& src, int dx, int dy, int sx, int sy, int w, int h, bool overwrite) {
        for (int y = 0; y < h; ++y) {
            if (dy + y < 0 || dy + y >= dst.height || sy + y < 0 || sy + y >= src.height) continue;
            for (int x = 0; x < w; ++x) {
                if (dx + x < 0 || dx + x >= dst.width || sx + x < 0 || sx + x >= src.width) continue;
                uint32_t s = src.pixels[(size_t)(sy + y) * src.width + sx + x];
                uint32_t& d = dst.pixels[(size_t)(dy + y) * dst.width + dx + x];
                uint32_t a = s >> 24;
                if (overwrite || a == 255) { d = s; continue; }
                uint32_t da = d >> 24, rest = da * (255 - a) / 255, oa = a + rest, out = 0;
                for (int k = 0; k < 24; k += 8) out |= (oa ? ((s >> k & 255) * a + (d >> k & 255) * rest) / oa : 0) << k;
                d = out | oa << 24;
            }
        }
    }

    Effect addFrame(Command c, const std::string& payload) {
        Image* img = imageFor(c);
        if (!img) return answer(c, "ENOENT:image not found");
        const Bitmap& root = *img->bitmap;
        int edit = c.rows, base = c.cols;
        if (edit > img->count() || base > img->count()) return answer(c, "ENOENT:frame not found");
        std::string raw, error = readPayload(c, payload, raw);
        if (!error.empty()) return answer(c, error);
        if (!c.width && !c.height && c.format != 100) { c.width = root.width; c.height = root.height; }
        Bitmap data;
        error = decodeBitmap(c, std::move(raw), data);
        if (!error.empty()) return answer(c, error);
        auto canvas = std::make_shared<Bitmap>();
        if (edit) *canvas = *img->frame(edit);
        else if (base) *canvas = *img->frame(base);
        else {
            uint32_t bg = (uint32_t)c.offY;
            canvas->width = root.width;
            canvas->height = root.height;
            canvas->pixels.assign(root.pixels.size(), (bg >> 24 & 255) | (bg >> 16 & 255) << 8 | (bg >> 8 & 255) << 16 | (bg & 255) << 24);
        }
        blend(*canvas, data, c.x, c.y, 0, 0, data.width, data.height, c.offX == 1);
        canvas->key = Bitmap::nextKey();
        int gap = c.z > 0 ? c.z : c.z < 0 ? -1 : 0;
        bytes += canvas->pixels.size() * 4;
        if (edit == 1) { bytes -= img->bitmap->pixels.size() * 4; img->bitmap = canvas; if (gap) img->rootGap = gap; }
        else if (edit) { Frame& f = img->frames[(size_t)edit - 2]; bytes -= f.bitmap->pixels.size() * 4; f.bitmap = canvas; if (gap) f.gap = gap; }
        else img->frames.push_back({ canvas, gap ? gap : 40 });
        evict();
        return answer(c, "OK");
    }

    Effect controlAnimation(const Command& c) {
        Image* img = imageFor(c);
        if (!img) return answer(c, "ENOENT:image not found");
        int state = c.width, frame = c.rows, current = c.cols, loops = c.height;
        if (frame && frame <= img->count() && c.z) (frame == 1 ? img->rootGap : img->frames[(size_t)frame - 2].gap) = c.z > 0 ? c.z : -1;
        if (current && current <= img->count()) { img->current = current; img->shownAt = nowMs(); }
        if (loops) img->loops = loops;
        if (state >= Image::STOPPED && state <= Image::RUNNING) {
            if (img->state == Image::STOPPED && state != Image::STOPPED) { img->shownAt = nowMs(); img->loopsLeft = img->loops - 1; }
            img->state = state;
        }
        return answer(c, "OK");
    }

    Effect composeFrames(const Command& c) {
        Image* img = imageFor(c);
        if (!img) return answer(c, "ENOENT:image not found");
        int target = c.cols ? c.cols : 1, source = c.rows ? c.rows : 1;
        if (target > img->count() || source > img->count()) return answer(c, "ENOENT:frame not found");
        const Bitmap& src = *img->frame(source);
        int w = c.w ? c.w : src.width, h = c.h ? c.h : src.height;
        if (c.offX + w > src.width || c.offY + h > src.height) return answer(c, "EINVAL:source rectangle out of bounds");
        auto canvas = std::make_shared<Bitmap>(*img->frame(target));
        if (c.x + w > canvas->width || c.y + h > canvas->height) return answer(c, "EINVAL:destination rectangle out of bounds");
        blend(*canvas, src, c.x, c.y, c.offX, c.offY, w, h, c.keepCursor == 1);
        canvas->key = Bitmap::nextKey();
        if (target == 1) img->bitmap = canvas;
        else img->frames[(size_t)target - 2].bitmap = canvas;
        return answer(c, "OK");
    }

    static double nowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

    Effect transmit(Command c, const std::string& payload, const Cursor& at) {
        std::string raw;
        if (c.action == 'q' && payload.empty()) return answer(c, "OK");
        std::string error = readPayload(c, payload, raw);
        if (!error.empty()) return answer(c, error);
        auto b = std::make_shared<Bitmap>();
        error = decodeBitmap(c, std::move(raw), *b);
        if (!error.empty()) return answer(c, error);
        if (c.action == 'q') return answer(c, "OK");
        b->key = Bitmap::nextKey();
        if (!c.id && c.number) c.id = freeId();
        Image& img = images[c.id];
        bytes -= img.bytes();
        img = Image();
        img.id = c.id;
        img.number = c.number;
        img.age = ++clock;
        img.bitmap = b;
        bytes += b->pixels.size() * 4;
        for (auto& list : placements)
            for (Placement& p : list)
                if (p.image == c.id) p.bitmap = b;
        evict();
        if (c.action == 'T') return place(c, at);
        return answer(c, "OK");
    }

    Effect place(const Command& c, const Cursor& at) {
        const Image* img = nullptr;
        if (c.id || !c.number) { auto it = images.find(c.id); if (it != images.end()) img = &it->second; }
        else for (auto& kv : images) if (kv.second.number == c.number && (!img || kv.second.age > img->age)) img = &kv.second;
        if (!img || !img->bitmap) return answer(c, "ENOENT:image not found");
        const Bitmap& b = *img->bitmap;
        Placement p;
        p.image = img->id;
        p.id = c.placement;
        p.bitmap = img->bitmap;
        p.col = at.col;
        p.row = at.row;
        p.x = std::clamp(c.x, 0, b.width);
        p.y = std::clamp(c.y, 0, b.height);
        p.w = c.w > 0 ? (std::min)(c.w, b.width - p.x) : b.width - p.x;
        p.h = c.h > 0 ? (std::min)(c.h, b.height - p.y) : b.height - p.y;
        p.offX = (std::max)(c.offX, 0);
        p.offY = (std::max)(c.offY, 0);
        p.cols = (std::max)(c.cols, 0);
        p.rows = (std::max)(c.rows, 0);
        p.z = c.z;
        p.virtualOnly = c.virtualPlacement != 0;
        int pixelW = p.cols ? p.cols * at.cellW : p.w, pixelH = p.rows ? p.rows * at.cellH : p.h;
        if (p.cols && !p.rows && p.w) pixelH = pixelW * p.h / p.w;
        if (p.rows && !p.cols && p.h) pixelW = pixelH * p.w / p.h;
        p.spanCols = (std::max)(1, (p.offX + pixelW + at.cellW - 1) / at.cellW);
        p.spanRows = (std::max)(1, (p.offY + pixelH + at.cellH - 1) / at.cellH);
        auto& list = placements[at.screen];
        if (p.id) list.erase(std::remove_if(list.begin(), list.end(), [&](const Placement& q) { return q.image == p.image && q.id == p.id; }), list.end());
        list.push_back(p);
        std::stable_sort(list.begin(), list.end(), [](const Placement& a, const Placement& b) { return a.z < b.z; });
        Effect e = answer(c, "OK");
        e.placed = !p.virtualOnly;
        e.moveCursor = !c.keepCursor && !p.virtualOnly;
        e.cols = p.spanCols;
        e.rows = p.spanRows;
        return e;
    }

    static bool covers(const Placement& p, int col, int row) { return col >= p.col && col < p.col + p.spanCols && row >= p.row && row < p.row + p.spanRows; }

    void remove(const Command& c, const Cursor& at) {
        char t = c.target ? c.target : 'a';
        bool freeData = t >= 'A' && t <= 'Z';
        char k = (char)(t | 0x20);
        uint32_t numbered = 0;
        if (k == 'n') { uint64_t best = 0; for (auto& kv : images) if (kv.second.number == c.number && kv.second.age >= best) { best = kv.second.age; numbered = kv.first; } }
        auto match = [&](const Placement& p) {
            switch (k) {
            case 'a': return !p.virtualOnly;
            case 'i': case 'n': case 'r': break;
            default: if (p.virtualOnly) return false;
            }
            switch (k) {
            case 'i': return p.image == c.id && (!c.placement || p.id == c.placement);
            case 'n': return p.image == numbered && numbered != 0;
            case 'c': return covers(p, at.col, at.row);
            case 'p': return covers(p, c.x - 1, c.y - 1);
            case 'q': return covers(p, c.x - 1, c.y - 1) && p.z == c.z;
            case 'x': return c.x - 1 >= p.col && c.x - 1 < p.col + p.spanCols;
            case 'y': return c.y - 1 >= p.row && c.y - 1 < p.row + p.spanRows;
            case 'z': return p.z == c.z;
            case 'r': return p.image >= (uint32_t)c.x && p.image <= (uint32_t)c.y;
            default: return false;
            }
        };
        std::vector<uint32_t> touched;
        auto& list = placements[at.screen];
        for (const Placement& p : list) if (match(p)) touched.push_back(p.image);
        list.erase(std::remove_if(list.begin(), list.end(), match), list.end());
        if (!freeData) return;
        if (k == 'i' && !c.placement) touched.push_back(c.id);
        if (k == 'n') touched.push_back(numbered);
        if (k == 'r') for (auto& kv : images) if (kv.first >= (uint32_t)c.x && kv.first <= (uint32_t)c.y) touched.push_back(kv.first);
        for (uint32_t id : touched) if (!placed(id)) drop(id);
    }

    bool placed(uint32_t id) const {
        for (auto& list : placements) for (const Placement& p : list) if (p.image == id) return true;
        return false;
    }

    void drop(uint32_t id) {
        auto it = images.find(id);
        if (it == images.end()) return;
        bytes -= it->second.bytes();
        images.erase(it);
    }

    void evict() {
        while (bytes > STORE_BYTES && images.size() > 1) {
            auto oldest = images.end();
            for (int pass = 0; pass < 2 && oldest == images.end(); ++pass)
                for (auto it = images.begin(); it != images.end(); ++it)
                    if ((pass || !placed(it->first)) && (oldest == images.end() || it->second.age < oldest->second.age)) oldest = it;
            if (oldest == images.end()) return;
            uint32_t id = oldest->first;
            for (auto& list : placements) list.erase(std::remove_if(list.begin(), list.end(), [&](const Placement& p) { return p.image == id; }), list.end());
            drop(id);
        }
    }
};

}
