#pragma once
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vt {

enum : uint32_t { COLOR_DEFAULT = 0x01000000, COLOR_INDEXED = 0x02000000 };
enum : uint16_t { BOLD = 1, DIM = 2, ITALIC = 4, UNDERLINE = 8, BLINK = 16, REVERSE = 32, INVISIBLE = 64, STRIKE = 128 };
enum : uint16_t { UNDERLINE_SHIFT = 8, UNDERLINE_STYLE = 7 << UNDERLINE_SHIFT };
enum Underline : uint16_t { UNDERLINE_NONE, UNDERLINE_SINGLE, UNDERLINE_DOUBLE, UNDERLINE_CURLY, UNDERLINE_DOTTED, UNDERLINE_DASHED };

struct Attr {
    uint32_t fg = COLOR_DEFAULT, bg = COLOR_DEFAULT;
    uint16_t flags = 0, link = 0;
    uint32_t underlineColor = COLOR_DEFAULT;
    bool operator==(const Attr&) const = default;
    int underline() const { return flags & UNDERLINE ? (flags & UNDERLINE_STYLE ? flags >> UNDERLINE_SHIFT & 7 : UNDERLINE_SINGLE) : UNDERLINE_NONE; }
};

inline void setUnderline(Attr& a, uint32_t style) {
    a.flags &= ~(UNDERLINE | UNDERLINE_STYLE);
    if (style == UNDERLINE_NONE || style > UNDERLINE_DASHED) return;
    a.flags |= UNDERLINE;
    if (style != UNDERLINE_SINGLE) a.flags |= (uint16_t)(style << UNDERLINE_SHIFT);
}

struct LinkTable {
    std::mutex m;
    std::vector<std::string> uris{ std::string() };
    std::unordered_map<std::string, uint16_t> ids;
};

inline LinkTable& linkTable() { static LinkTable t; return t; }

inline uint16_t linkId(const std::string& uri) {
    if (uri.empty()) return 0;
    LinkTable& t = linkTable();
    std::lock_guard<std::mutex> l(t.m);
    auto it = t.ids.find(uri);
    if (it != t.ids.end()) return it->second;
    if (t.uris.size() > 0xFFFF) return 0;
    uint16_t id = (uint16_t)t.uris.size();
    t.uris.push_back(uri);
    t.ids.emplace(uri, id);
    return id;
}

inline std::string linkUri(uint16_t id) {
    LinkTable& t = linkTable();
    std::lock_guard<std::mutex> l(t.m);
    return id < t.uris.size() ? t.uris[id] : std::string();
}

inline uint16_t oscLink(const std::string& body) {
    if (body.size() < 2 || body[0] != '8' || body[1] != ';') return UINT16_MAX;
    size_t semi = body.find(';', 2);
    return semi == std::string::npos ? 0 : linkId(body.substr(semi + 1));
}

inline uint32_t rgb(uint32_t r, uint32_t g, uint32_t b) { return (r & 255) | (g & 255) << 8 | (b & 255) << 16; }

inline void resetKeepingLink(Attr& a) { uint16_t l = a.link; a = Attr(); a.link = l; }

constexpr int MAX_PARAMS = 32;

template <class T>
inline int parseParams(const T* s, const T* e, uint32_t* p, uint32_t& sub) {
    int n = 0;
    sub = 0;
    for (; s < e; ++s) {
        if (*s == ';' || *s == ':') {
            if (n == 0) n = 1;
            if (n < MAX_PARAMS) { if (*s == ':') sub |= 1u << n; p[n++] = 0; }
        }
        else if (*s >= '0' && *s <= '9') { if (n == 0) { n = 1; p[0] = 0; } p[n - 1] = (std::min)(p[n - 1] * 10 + (uint32_t)(*s - '0'), 65535u); }
    }
    return n;
}

inline bool sgrColor(uint32_t& c, const uint32_t* q, int n, bool grouped) {
    if (n >= 2 && q[0] == 5) { c = COLOR_INDEXED | (q[1] & 255); return true; }
    if (q[0] != 2) return false;
    if (grouped && n >= 5) { c = rgb(q[2], q[3], q[4]); return true; }
    if (n >= 4) { c = rgb(q[1], q[2], q[3]); return true; }
    return false;
}

inline uint32_t& sgrTarget(Attr& a, uint32_t v) { return v == 38 ? a.fg : v == 48 ? a.bg : a.underlineColor; }

inline void applySgr(Attr& a, const uint32_t* p, int n, uint32_t sub = 0) {
    if (n == 0) { resetKeepingLink(a); return; }
    for (int i = 0; i < n; ++i) {
        uint32_t v = p[i];
        int groupEnd = i + 1;
        while (groupEnd < n && (sub >> groupEnd & 1)) ++groupEnd;
        if (groupEnd > i + 1) {
            const uint32_t* q = p + i + 1;
            int qn = groupEnd - i - 1;
            if (v == 4) setUnderline(a, q[0]);
            else if (v == 38 || v == 48 || v == 58) sgrColor(sgrTarget(a, v), q, qn, true);
            i = groupEnd - 1;
            continue;
        }
        switch (v) {
        case 0: resetKeepingLink(a); break;
        case 1: a.flags |= BOLD; break;
        case 2: a.flags |= DIM; break;
        case 3: a.flags |= ITALIC; break;
        case 4: setUnderline(a, UNDERLINE_SINGLE); break;
        case 21: setUnderline(a, UNDERLINE_DOUBLE); break;
        case 5: case 6: a.flags |= BLINK; break;
        case 7: a.flags |= REVERSE; break;
        case 8: a.flags |= INVISIBLE; break;
        case 9: a.flags |= STRIKE; break;
        case 22: a.flags &= ~(BOLD | DIM); break;
        case 23: a.flags &= ~ITALIC; break;
        case 24: setUnderline(a, UNDERLINE_NONE); break;
        case 25: a.flags &= ~BLINK; break;
        case 27: a.flags &= ~REVERSE; break;
        case 28: a.flags &= ~INVISIBLE; break;
        case 29: a.flags &= ~STRIKE; break;
        case 39: a.fg = COLOR_DEFAULT; break;
        case 49: a.bg = COLOR_DEFAULT; break;
        case 59: a.underlineColor = COLOR_DEFAULT; break;
        case 38: case 48: case 58: {
            const uint32_t* q = p + i + 1;
            int qn = n - i - 1;
            if (qn >= 1 && sgrColor(sgrTarget(a, v), q, qn, false)) i += q[0] == 5 ? 2 : 4;
            else i = n;
            break;
        }
        default:
            if (v >= 30 && v <= 37) a.fg = COLOR_INDEXED | (v - 30);
            else if (v >= 40 && v <= 47) a.bg = COLOR_INDEXED | (v - 40);
            else if (v >= 90 && v <= 97) a.fg = COLOR_INDEXED | (v - 90 + 8);
            else if (v >= 100 && v <= 107) a.bg = COLOR_INDEXED | (v - 100 + 8);
        }
    }
}

template <class T>
inline const T* sgrEnd(const T* s, const T* e) {
    if (e - s < 3 || s[1] != '[') return nullptr;
    const T* q = s + 2;
    while (q < e && ((*q >= '0' && *q <= '9') || *q == ';' || *q == ':')) ++q;
    return q < e && *q == 'm' ? q + 1 : nullptr;
}

template <class T>
inline void applySgrSequence(Attr& a, const T* s, const T* end) {
    uint32_t p[MAX_PARAMS], sub;
    int n = parseParams(s + 2, end - 1, p, sub);
    applySgr(a, p, n, sub);
}

struct EscapeParser {
    enum State : uint8_t { IDLE, ESC, CSI, OSC, OSC_ESC };
    enum Action : uint8_t { NONE, STYLE, COMMAND };
    State state = IDLE;
    bool privateMarker = false;
    char command = 0;
    int count = 0;
    uint32_t params[16] = {};
    uint32_t sub = 0;
    char mark = 0;
    std::string osc;

    void begin() { state = ESC; }

    void endOsc(Attr& a) {
        uint16_t id = oscLink(osc);
        if (id != UINT16_MAX) a.link = id;
        if (osc.size() >= 5 && osc.rfind("133;", 0) == 0) mark = osc[4];
        osc.clear();
    }
    bool active() const { return state != IDLE; }
    int param(int i, int fallback) const { return i < count && params[i] ? (int)params[i] : fallback; }

    Action feed(unsigned char c, Attr& a) { Action action; run(&c, &c + 1, a, action); return action; }

    const unsigned char* run(const unsigned char* s, const unsigned char* e, Attr& a, Action& action) {
        action = NONE;
        while (s < e) {
            unsigned char c = *s++;
            switch (state) {
            case ESC:
                if (c == '[') { state = CSI; count = 0; params[0] = 0; sub = 0; privateMarker = false; }
                else if (c == ']') { state = OSC; osc.clear(); }
                else return state = IDLE, s;
                break;
            case CSI: {
                int n = count;
                uint32_t v = n ? params[n - 1] : 0;
                for (;;) {
                    if (c - '0' < 10u) { n = n ? n : 1; v = v * 10 + (c - '0'); }
                    else if (c == ';' || c == ':') { n = n ? n : 1; if (n < 16) { params[n - 1] = v; if (c == ':') sub |= 1u << n; ++n; v = 0; } }
                    else if (c >= 0x20 && c <= 0x3F) privateMarker = true;
                    else {
                        if (n) params[n - 1] = v;
                        count = n;
                        state = IDLE;
                        command = (char)c;
                        if (privateMarker) return s;
                        if (c == 'm') { applySgr(a, params, count, sub); action = STYLE; }
                        else action = COMMAND;
                        return s;
                    }
                    if (s == e) { if (n) params[n - 1] = v; count = n; return s; }
                    c = *s++;
                }
            }
            case OSC:
                if (c == 0x07) { endOsc(a); return state = IDLE, s; }
                if (c == 0x1B) state = OSC_ESC;
                else if (osc.size() < 4096) osc += (char)c;
                break;
            case OSC_ESC:
                endOsc(a);
                return state = IDLE, s;
            default:
                return s;
            }
        }
        return s;
    }
};

}
