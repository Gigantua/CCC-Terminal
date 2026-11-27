#pragma once
#include <CoreText/CoreText.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include "../shell/console.h"
#include "boxdraw.h"
#include "cascadia_mono.h"

struct FontMetrics {
    int cellW = 8, cellH = 16;
    int baseline = 12;
    int underlineTop = 14, underlineBottom = 15;
    int strikeTop = 8, strikeBottom = 9;
};

template <class T>
class CfRef {
public:
    CfRef() = default;
    explicit CfRef(T r) : ref(r) {}
    CfRef(const CfRef& o) : ref(o.ref) { if (ref) CFRetain(ref); }
    CfRef& operator=(CfRef o) { std::swap(ref, o.ref); return *this; }
    ~CfRef() { if (ref) CFRelease(ref); }
    T get() const { return ref; }
    explicit operator bool() const { return ref != nullptr; }
private:
    T ref = nullptr;
};

class GlyphCache {
public:
    static constexpr uint32_t COLOR_GLYPH = 8;
    static constexpr int STYLES = 4;
    static constexpr const char* BUNDLED = "Cascadia Mono";

    bool init() {
        CfRef<CFDataRef> data(CFDataCreateWithBytesNoCopy(nullptr, cascadia_mono, (CFIndex)sizeof(cascadia_mono), kCFAllocatorNull));
        CfRef<CGDataProviderRef> provider(CGDataProviderCreateWithCFData(data.get()));
        bundledFont = CfRef<CGFontRef>(CGFontCreateWithDataProvider(provider.get()));
        return (bool)bundledFont;
    }

    bool setFont(const std::string& requested, float pixelSize) {
        size = pixelSize;
        std::string family = requested.empty() || !systemFamily(requested) ? BUNDLED : requested;
        fontFamily = family;
        if (!makeFaces()) {
            fontFamily = BUNDLED;
            if (!makeFaces()) return false;
        }
        measure(faces[0].font.get());
        return true;
    }

    const FontMetrics& metrics() const { return m; }
    const std::string& family() const { return fontFamily; }
    uint32_t tileTexels() const { return (uint32_t)(m.cellW * m.cellH); }

    void attach(uint32_t* atlasTexels, uint32_t tileCapacity) {
        atlas = atlasTexels;
        capacity = tileCapacity;
        reset();
    }

    void reset() {
        ascii.fill(0);
        singles.clear();
        clusters.clear();
        std::memset(atlas, 0, (size_t)tileTexels() * 4);
        nextTile = 1;
        overflowed = false;
    }

    bool overflow() const { return overflowed; }

    uint32_t glyph(uint32_t id, const Console::Frame& f, int style) {
        if (id == ' ') return 0;
        if (id < 128) {
            uint32_t& slot = ascii[style * 128 + id];
            if (!slot) slot = rasterize(std::u32string(1, (char32_t)id), style, 1) + 1;
            return slot - 1;
        }
        if (id < Console::CLUSTER_BASE) {
            uint64_t key = (uint64_t)id | (uint64_t)style << 32;
            auto it = singles.find(key);
            if (it != singles.end()) return it->second;
            char32_t cp = (char32_t)id;
            uint32_t g = boxdraw::drawable(cp) ? rasterizeBox(cp) : rasterize(std::u32string(1, cp), style, Console::clusterWidth(&cp, 1));
            singles.emplace(key, g);
            return g;
        }
        const std::u32string& text = f.clusters[id - Console::CLUSTER_BASE];
        std::u32string key = text;
        key += (char32_t)(0x110000 + style);
        auto it = clusters.find(key);
        if (it != clusters.end()) return it->second;
        uint32_t g = rasterize(text, style, Console::clusterWidth(text.data(), text.size()));
        clusters.emplace(std::move(key), g);
        return g;
    }

private:
    struct Face {
        CfRef<CTFontRef> font;
        bool synthBold = false;
    };

    CfRef<CGFontRef> bundledFont;
    Face faces[STYLES];
    std::unordered_map<uint64_t, CfRef<CTFontRef>> fallbacks;
    CfRef<CTFontRef> emojiFont;
    std::string fontFamily;
    float size = 16;
    FontMetrics m;
    uint32_t* atlas = nullptr;
    uint32_t capacity = 0;
    uint32_t nextTile = 1;
    bool overflowed = false;
    std::array<uint32_t, 128 * STYLES> ascii{};
    std::unordered_map<uint64_t, uint32_t> singles;
    std::unordered_map<std::u32string, uint32_t> clusters;

    static bool bold(int style) { return style & 1; }
    static bool italic(int style) { return style & 2; }

    static CfRef<CFStringRef> cfString(const std::string& s) { return CfRef<CFStringRef>(CFStringCreateWithCString(nullptr, s.c_str(), kCFStringEncodingUTF8)); }

    static std::string utf8(CFStringRef s) {
        if (!s) return std::string();
        CFIndex n = CFStringGetMaximumSizeForEncoding(CFStringGetLength(s), kCFStringEncodingUTF8) + 1;
        std::string out((size_t)n, '\0');
        if (!CFStringGetCString(s, out.data(), n, kCFStringEncodingUTF8)) return std::string();
        out.resize(std::strlen(out.c_str()));
        return out;
    }

    static bool systemFamily(const std::string& family) {
        CfRef<CTFontRef> f(CTFontCreateWithName(cfString(family).get(), 12, nullptr));
        CfRef<CFStringRef> name(CTFontCopyFamilyName(f.get()));
        return f && strcasecmp(utf8(name.get()).c_str(), family.c_str()) == 0;
    }

    static CGAffineTransform shear(bool on) { return CGAffineTransformMake(1, 0, on ? 0.21875 : 0, 1, 0, 0); }

    CfRef<CTFontRef> bundled(int style, float px) {
        CGAffineTransform t = shear(italic(style));
        int wght = 0x77676874;
        CfRef<CFNumberRef> axis(CFNumberCreate(nullptr, kCFNumberIntType, &wght));
        double weight = bold(style) ? 700 : 400;
        CfRef<CFNumberRef> value(CFNumberCreate(nullptr, kCFNumberDoubleType, &weight));
        const void* keys[] = { axis.get() };
        const void* values[] = { value.get() };
        CfRef<CFDictionaryRef> variation(CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
        const void* attrKeys[] = { kCTFontVariationAttribute };
        const void* attrValues[] = { variation.get() };
        CfRef<CFDictionaryRef> attrs(CFDictionaryCreate(nullptr, attrKeys, attrValues, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
        CfRef<CTFontDescriptorRef> desc(CTFontDescriptorCreateWithAttributes(attrs.get()));
        return CfRef<CTFontRef>(CTFontCreateWithGraphicsFont(bundledFont.get(), px, &t, desc.get()));
    }

    Face system(int style, float px, bool& ok) {
        Face f;
        CfRef<CTFontRef> base(CTFontCreateWithName(cfString(fontFamily).get(), px, nullptr));
        if (!base) { ok = false; return f; }
        CTFontSymbolicTraits want = (bold(style) ? kCTFontTraitBold : 0) | (italic(style) ? kCTFontTraitItalic : 0);
        if (!want) { f.font = base; return f; }
        CTFontRef styled = CTFontCreateCopyWithSymbolicTraits(base.get(), px, nullptr, want, want);
        if (styled) { f.font = CfRef<CTFontRef>(styled); return f; }
        CGAffineTransform t = shear(italic(style));
        CfRef<CTFontDescriptorRef> desc(CTFontCopyFontDescriptor(base.get()));
        f.font = CfRef<CTFontRef>(CTFontCreateWithFontDescriptor(desc.get(), px, &t));
        f.synthBold = bold(style);
        return f;
    }

    bool makeFaces() {
        fallbacks.clear();
        emojiFont = CfRef<CTFontRef>();
        bool ok = true;
        for (int s = 0; s < STYLES; ++s) {
            faces[s] = fontFamily == BUNDLED ? Face{ bundled(s, size), false } : system(s, size, ok);
            if (!faces[s].font) return false;
        }
        return ok;
    }

    void measure(CTFontRef font) {
        float ascent = (float)CTFontGetAscent(font), descent = (float)CTFontGetDescent(font), lineGap = (std::max)(0.0f, (float)CTFontGetLeading(font));
        UniChar zero = '0';
        CGGlyph g = 0;
        float advance = size * 0.6f;
        if (CTFontGetGlyphsForCharacters(font, &zero, &g, 1)) advance = (float)CTFontGetAdvancesForGlyphs(font, kCTFontOrientationHorizontal, &g, nullptr, 1);
        m.cellW = (std::max)(1, (int)std::lround(advance));
        m.cellH = (std::max)(1, (int)std::ceil(ascent + descent + lineGap));
        m.baseline = (int)std::lround(ascent + lineGap / 2);
        int underlineH = (std::max)(1, (int)std::lround(CTFontGetUnderlineThickness(font)));
        m.underlineTop = (std::min)(m.cellH - underlineH, m.baseline - (int)std::lround(CTFontGetUnderlinePosition(font)));
        m.underlineBottom = m.underlineTop + underlineH;
        m.strikeTop = m.baseline - (int)std::lround(CTFontGetXHeight(font) / 2) - underlineH / 2;
        m.strikeBottom = m.strikeTop + underlineH;
    }

    static int utf16(char32_t cp, UniChar (&units)[2]) {
        if (cp < 0x10000) { units[0] = (UniChar)cp; return 1; }
        cp -= 0x10000;
        units[0] = (UniChar)(0xD800 + (cp >> 10));
        units[1] = (UniChar)(0xDC00 + (cp & 0x3FF));
        return 2;
    }

    static CGGlyph glyphIn(CTFontRef font, char32_t cp) {
        UniChar units[2];
        CGGlyph glyphs[2] = {};
        int n = utf16(cp, units);
        return CTFontGetGlyphsForCharacters(font, units, glyphs, n) ? glyphs[0] : 0;
    }

    CTFontRef emojiFace(char32_t cp) {
        if (!emojiFont) emojiFont = CfRef<CTFontRef>(CTFontCreateWithName(CFSTR("Apple Color Emoji"), size, nullptr));
        return emojiFont && glyphIn(emojiFont.get(), cp) ? emojiFont.get() : nullptr;
    }

    CTFontRef faceFor(char32_t cp, int style, bool& synthBold) {
        synthBold = faces[style].synthBold;
        if (glyphIn(faces[style].font.get(), cp)) return faces[style].font.get();
        synthBold = false;
        uint64_t key = (uint64_t)cp | (uint64_t)style << 32;
        auto it = fallbacks.find(key);
        if (it != fallbacks.end()) return it->second.get();
        UniChar units[2];
        int n = utf16(cp, units);
        CfRef<CFStringRef> text(CFStringCreateWithCharacters(nullptr, units, n));
        CfRef<CTFontRef> found(CTFontCreateForString(faces[style].font.get(), text.get(), CFRangeMake(0, n)));
        if (found && !glyphIn(found.get(), cp)) found = CfRef<CTFontRef>();
        return fallbacks.emplace(key, found).first->second.get();
    }

    void drawGlyph(CGContextRef cg, CTFontRef font, bool synthBold, char32_t cp, int width, bool& colored) {
        CGGlyph g = glyphIn(font, cp);
        if (!g) return;
        bool color = (CTFontGetSymbolicTraits(font) & kCTFontTraitColorGlyphs) != 0;
        int room = width * m.cellW;
        CGRect box = CTFontGetBoundingRectsForGlyphs(font, kCTFontOrientationHorizontal, &g, nullptr, 1);
        double advance = CTFontGetAdvancesForGlyphs(font, kCTFontOrientationHorizontal, &g, nullptr, 1);
        double scale = 1, x = 0, baseline = m.cellH - m.baseline;
        if (color) {
            double h = CTFontGetAscent(font) + CTFontGetDescent(font);
            scale = (std::min)(room / (std::max)(advance, 1.0), m.cellH / (std::max)(h, 1.0));
            x = (room - advance * scale) / 2;
            baseline = (m.cellH - h * scale) / 2 + CTFontGetDescent(font) * scale;
        }
        else if (CGRectGetMaxX(box) > room + 1 && CGRectGetMaxX(box) > 0) scale = room / CGRectGetMaxX(box);
        CGContextSaveGState(cg);
        CGContextTranslateCTM(cg, x, baseline);
        CGContextScaleCTM(cg, scale, scale);
        if (synthBold) {
            CGContextSetTextDrawingMode(cg, kCGTextFillStroke);
            CGContextSetLineWidth(cg, size * 0.035);
        }
        CGPoint at = CGPointZero;
        CTFontDrawGlyphs(font, &g, &at, 1, cg);
        CGContextRestoreGState(cg);
        colored |= color;
    }

    uint32_t rasterizeBox(char32_t cp) {
        if (nextTile + 1 > capacity) { overflowed = true; return 0; }
        uint32_t tile = nextTile++;
        std::vector<uint8_t> coverage(tileTexels());
        boxdraw::draw(cp, m.cellW, m.cellH, coverage.data());
        uint32_t* dst = atlas + (size_t)tile * tileTexels();
        for (size_t i = 0; i < coverage.size(); ++i) dst[i] = coverage[i] * 0x01010101u;
        return tile;
    }

    uint32_t rasterize(const std::u32string& text, int style, int width) {
        if (nextTile + width > capacity) { overflowed = true; return 0; }
        uint32_t tile = nextTile;
        nextTile += width;
        Console::Sizing z;
        bool scaled = Console::sizingOf(text.data(), text.size(), z) && z.scaled();
        float k = scaled ? z.s * (z.d ? (float)z.n / z.d : 1.0f) : 1.0f;
        int cells = scaled ? z.w : width, ox = 0, oy = 0;
        FontMetrics cell = m;
        float cellSize = size;
        if (scaled) setScale(k);
        int canvasW = cells * m.cellW, canvasH = m.cellH;
        std::vector<uint32_t> canvas((size_t)canvasW * canvasH, 0);
        bool colored = false;
        {
            CfRef<CGColorSpaceRef> space(CGColorSpaceCreateDeviceRGB());
            CfRef<CGContextRef> cg(CGBitmapContextCreate(canvas.data(), (size_t)canvasW, (size_t)canvasH, 8, (size_t)canvasW * 4, space.get(),
                                                         kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little));
            CGContextSetRGBFillColor(cg.get(), 1, 1, 1, 1);
            CGContextSetRGBStrokeColor(cg.get(), 1, 1, 1, 1);
            CGContextSetShouldSmoothFonts(cg.get(), false);
            CGContextSetAllowsFontSmoothing(cg.get(), false);
            bool wantEmoji = text.find(unicode::VS_EMOJI) != std::u32string::npos;
            for (char32_t cp : text) {
                if (cp >= Console::SIZED) continue;
                if (cp == unicode::ZWJ || cp == unicode::VS_EMOJI || cp == unicode::VS_TEXT) continue;
                bool synthBold = false;
                CTFontRef font = wantEmoji ? emojiFace(cp) : nullptr;
                if (!font) font = faceFor(cp, style, synthBold);
                if (font) drawGlyph(cg.get(), font, synthBold, cp, cells, colored);
                if (colored) break;
            }
        }
        if (!colored)
            for (uint32_t& p : canvas) { uint32_t a = p >> 24; p = a * 0x01010101u; }
        if (scaled) {
            ox = alignedOffset(z.h, width * cell.cellW - canvasW);
            oy = alignedOffset(z.v, z.s * cell.cellH - canvasH) - z.row * cell.cellH;
            m = cell;
            size = cellSize;
            setScale(1);
        }
        for (int t = 0; t < width; ++t) {
            uint32_t* dst = atlas + (size_t)(tile + t) * tileTexels();
            for (int y = 0; y < m.cellH; ++y)
                for (int x = 0; x < m.cellW; ++x) {
                    int sx = t * m.cellW + x - ox, sy = y - oy;
                    dst[(size_t)y * m.cellW + x] = sx >= 0 && sx < canvasW && sy >= 0 && sy < canvasH ? canvas[(size_t)sy * canvasW + sx] : 0;
                }
        }
        return tile | (colored ? COLOR_GLYPH << 24 : 0);
    }

    static int alignedOffset(int align, int room) { return align == 1 ? room : align == 2 ? room / 2 : 0; }

    void setScale(float k) {
        if (k != 1) {
            size *= k;
            m.cellW = (int)std::lround(m.cellW * k);
            m.cellH = (int)std::lround(m.cellH * k);
            m.baseline = (int)std::lround(m.baseline * k);
        }
        makeFaces();
    }
};
