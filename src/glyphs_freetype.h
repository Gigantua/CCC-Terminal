#pragma once
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H
#include FT_OUTLINE_H
#include FT_TRUETYPE_TABLES_H
#include <fontconfig/fontconfig.h>
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

class GlyphCache {
public:
    static constexpr uint32_t COLOR_GLYPH = 8;
    static constexpr int STYLES = 4;
    static constexpr const char* BUNDLED = "Cascadia Mono";

    ~GlyphCache() {
        closeFaces();
        if (library) FT_Done_FreeType(library);
    }

    bool init() { return FT_Init_FreeType(&library) == 0 && FcInit(); }

    bool setFont(const std::string& requested, float pixelSize) {
        closeFaces();
        size = pixelSize;
        std::string family = requested.empty() ? BUNDLED : requested;
        for (int s = 0; s < STYLES; ++s) {
            faces[s] = family == BUNDLED ? bundled(s) : system(family, s);
            if (!faces[s]) { family = BUNDLED; faces[s] = bundled(s); }
            if (!faces[s]) return false;
        }
        fontFamily = family;
        measure(faces[0].face);
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
        FT_Face face = nullptr;
        bool synthBold = false, synthItalic = false;
        explicit operator bool() const { return face != nullptr; }
    };

    FT_Library library = nullptr;
    Face faces[STYLES];
    std::unordered_map<uint64_t, FT_Face> fallbacks;
    std::vector<FT_Face> fallbackFaces;
    std::unordered_map<std::string, FT_Face> fallbackFiles;
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

    void closeFaces() {
        for (Face& f : faces) { if (f.face) FT_Done_Face(f.face); f = Face(); }
        for (FT_Face f : fallbackFaces) FT_Done_Face(f);
        fallbackFaces.clear();
        fallbackFiles.clear();
        fallbacks.clear();
    }

    bool sized(FT_Face face) {
        if (FT_IS_SCALABLE(face)) return FT_Set_Char_Size(face, 0, (FT_F26Dot6)std::lround(size * 64), 72, 72) == 0;
        if (!face->num_fixed_sizes) return false;
        return FT_Select_Size(face, 0) == 0;
    }

    void setWeight(FT_Face face, bool heavy) {
        FT_MM_Var* mm = nullptr;
        if (!FT_HAS_MULTIPLE_MASTERS(face) || FT_Get_MM_Var(face, &mm)) return;
        std::vector<FT_Fixed> coords(mm->num_axis);
        for (FT_UInt a = 0; a < mm->num_axis; ++a) {
            coords[a] = mm->axis[a].def;
            if (mm->axis[a].tag == FT_MAKE_TAG('w', 'g', 'h', 't')) coords[a] = std::clamp<FT_Fixed>((heavy ? 700 : 400) << 16, mm->axis[a].minimum, mm->axis[a].maximum);
        }
        FT_Set_Var_Design_Coordinates(face, mm->num_axis, coords.data());
        FT_Done_MM_Var(library, mm);
    }

    Face bundled(int style) {
        Face f;
        if (FT_New_Memory_Face(library, cascadia_mono, (FT_Long)sizeof(cascadia_mono), 0, &f.face)) return Face();
        setWeight(f.face, bold(style));
        f.synthItalic = italic(style);
        if (!sized(f.face)) { FT_Done_Face(f.face); return Face(); }
        return f;
    }

    static std::string matchFile(FcPattern* pattern, int& index) {
        FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
        FcDefaultSubstitute(pattern);
        FcResult result;
        FcPattern* match = FcFontMatch(nullptr, pattern, &result);
        std::string path;
        if (match) {
            FcChar8* file = nullptr;
            if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) path = reinterpret_cast<const char*>(file);
            if (FcPatternGetInteger(match, FC_INDEX, 0, &index) != FcResultMatch) index = 0;
            FcPatternDestroy(match);
        }
        FcPatternDestroy(pattern);
        return path;
    }

    Face system(const std::string& family, int style) {
        FcPattern* exact = FcPatternCreate();
        FcPatternAddString(exact, FC_FAMILY, reinterpret_cast<const FcChar8*>(family.c_str()));
        FcObjectSet* os = FcObjectSetBuild(FC_FAMILY, nullptr);
        FcFontSet* found = FcFontList(nullptr, exact, os);
        bool known = found && found->nfont > 0;
        if (found) FcFontSetDestroy(found);
        FcObjectSetDestroy(os);
        FcPatternDestroy(exact);
        if (!known) return Face();
        FcPattern* p = FcPatternCreate();
        FcPatternAddString(p, FC_FAMILY, reinterpret_cast<const FcChar8*>(family.c_str()));
        FcPatternAddInteger(p, FC_WEIGHT, bold(style) ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
        FcPatternAddInteger(p, FC_SLANT, italic(style) ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
        int index = 0;
        std::string path = matchFile(p, index);
        Face f;
        if (path.empty() || FT_New_Face(library, path.c_str(), index, &f.face)) return Face();
        f.synthBold = bold(style) && !(f.face->style_flags & FT_STYLE_FLAG_BOLD);
        f.synthItalic = italic(style) && !(f.face->style_flags & FT_STYLE_FLAG_ITALIC);
        if (f.synthBold) setWeight(f.face, true);
        if (!sized(f.face)) { FT_Done_Face(f.face); return Face(); }
        return f;
    }

    FT_Face fallbackFor(uint32_t cp, bool color = false) {
        uint64_t slot = (uint64_t)cp | (uint64_t)color << 32;
        auto it = fallbacks.find(slot);
        if (it != fallbacks.end()) return it->second;
        FcPattern* p = FcPatternCreate();
        FcCharSet* chars = FcCharSetCreate();
        FcCharSetAddChar(chars, cp);
        FcPatternAddCharSet(p, FC_CHARSET, chars);
        FcCharSetDestroy(chars);
        if (color || cp >= 0x1F000) FcPatternAddBool(p, FC_COLOR, FcTrue);
        int index = 0;
        std::string file = matchFile(p, index);
        std::string key = file + "#" + std::to_string(index);
        FT_Face face = nullptr;
        auto open = fallbackFiles.find(key);
        if (open != fallbackFiles.end()) face = open->second;
        else if (!file.empty() && !FT_New_Face(library, file.c_str(), index, &face)) {
            if (sized(face)) { fallbackFaces.push_back(face); fallbackFiles.emplace(key, face); }
            else { FT_Done_Face(face); face = nullptr; }
        }
        if (face && !FT_Get_Char_Index(face, cp)) face = nullptr;
        if (color && face && !FT_HAS_COLOR(face)) face = nullptr;
        fallbacks.emplace(slot, face);
        return face;
    }

    void measure(FT_Face face) {
        const FT_Size_Metrics& sm = face->size->metrics;
        float ascent = sm.ascender / 64.0f, descent = -sm.descender / 64.0f, lineGap = (std::max)(0.0f, sm.height / 64.0f - ascent - descent);
        FT_UInt zero = FT_Get_Char_Index(face, '0');
        float advance = size * 0.6f;
        if (zero && !FT_Load_Glyph(face, zero, FT_LOAD_NO_HINTING)) advance = face->glyph->advance.x / 64.0f;
        float scale = FT_IS_SCALABLE(face) ? sm.y_scale / 65536.0f / 64.0f : 1.0f;
        m.cellW = (std::max)(1, (int)std::lround(advance));
        m.cellH = (std::max)(1, (int)std::ceil(ascent + descent + lineGap));
        m.baseline = (int)std::lround(ascent + lineGap / 2);
        int underlineH = (std::max)(1, (int)std::lround(face->underline_thickness * scale));
        m.underlineTop = (std::min)(m.cellH - underlineH, m.baseline - (int)std::lround(face->underline_position * scale));
        m.underlineBottom = m.underlineTop + underlineH;
        TT_OS2* os2 = static_cast<TT_OS2*>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2));
        int strikeH = os2 ? (std::max)(1, (int)std::lround(os2->yStrikeoutSize * scale)) : underlineH;
        m.strikeTop = m.baseline - (os2 ? (int)std::lround(os2->yStrikeoutPosition * scale) : m.cellH / 4);
        m.strikeBottom = m.strikeTop + strikeH;
    }

    FT_Face faceFor(uint32_t cp, int style, Face& synth) {
        synth = faces[style];
        if (FT_Get_Char_Index(faces[style].face, cp)) return faces[style].face;
        synth = Face();
        synth.synthBold = bold(style);
        return fallbackFor(cp);
    }

    void drawGlyph(FT_Face face, const Face& synth, uint32_t cp, float originX, int width, std::vector<uint32_t>& canvas, int canvasW, bool& colored) {
        FT_UInt index = FT_Get_Char_Index(face, cp);
        if (!index) return;
        bool color = FT_HAS_COLOR(face);
        FT_Matrix shear = { 0x10000, synth.synthItalic ? 0x3800 : 0, 0, 0x10000 };
        FT_Set_Transform(face, &shear, nullptr);
        if (FT_Load_Glyph(face, index, FT_LOAD_TARGET_LIGHT | (color ? FT_LOAD_COLOR : 0))) return;
        FT_GlyphSlot slot = face->glyph;
        if (synth.synthBold && slot->format == FT_GLYPH_FORMAT_OUTLINE) FT_Outline_Embolden(&slot->outline, (FT_Pos)(size * 2.2f));
        if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL)) return;
        const FT_Bitmap& b = slot->bitmap;
        if (!b.width || !b.rows) return;
        float scale = 1.0f;
        int room = width * m.cellW;
        if (b.pixel_mode == FT_PIXEL_MODE_BGRA) scale = (std::min)((float)room / b.width, (float)m.cellH / b.rows);
        else if (slot->bitmap_left + (int)b.width > room + 1 && b.width) scale = (float)room / (slot->bitmap_left + b.width);
        int left = b.pixel_mode == FT_PIXEL_MODE_BGRA ? (int)std::lround((room - b.width * scale) / 2) : (int)std::lround(originX + slot->bitmap_left * scale);
        int top = b.pixel_mode == FT_PIXEL_MODE_BGRA ? (int)std::lround((m.cellH - b.rows * scale) / 2) : m.baseline - (int)std::lround(slot->bitmap_top * scale);
        int outW = (int)std::lround(b.width * scale), outH = (int)std::lround(b.rows * scale);
        for (int y = 0; y < outH; ++y) {
            int cy = top + y;
            if (cy < 0 || cy >= m.cellH) continue;
            const uint8_t* row = b.buffer + (size_t)(std::min)((int)(y / scale), (int)b.rows - 1) * b.pitch;
            for (int x = 0; x < outW; ++x) {
                int cx = left + x;
                if (cx < 0 || cx >= canvasW) continue;
                int sx = (std::min)((int)(x / scale), (int)b.width - 1);
                uint32_t& dst = canvas[(size_t)cy * canvasW + cx];
                if (b.pixel_mode == FT_PIXEL_MODE_BGRA) {
                    uint32_t p = reinterpret_cast<const uint32_t*>(row)[sx];
                    if (p >> 24) { dst = p; colored = true; }
                }
                else if (b.pixel_mode == FT_PIXEL_MODE_GRAY) {
                    uint32_t a = (std::max)(row[sx], (uint8_t)(dst >> 24));
                    if (!colored || a) dst = a << 24 | a << 16 | a << 8 | a;
                }
                else if (b.pixel_mode == FT_PIXEL_MODE_MONO && (row[sx / 8] >> (7 - sx % 8) & 1)) dst = 0xFFFFFFFF;
            }
        }
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
        int canvasW = cells * m.cellW;
        std::vector<uint32_t> canvas((size_t)canvasW * m.cellH, 0);
        bool colored = false;
        bool wantEmoji = text.find(unicode::VS_EMOJI) != std::u32string::npos;
        for (char32_t cp : text) {
            if (cp >= Console::SIZED) continue;
            if (cp == unicode::ZWJ || cp == unicode::VS_EMOJI || cp == unicode::VS_TEXT) continue;
            Face synth;
            FT_Face face = wantEmoji ? fallbackFor(cp, true) : nullptr;
            if (!face) face = faceFor(cp, style, synth);
            if (face) drawGlyph(face, synth, cp, 0, cells, canvas, canvasW, colored);
            if (colored) break;
        }
        int canvasH = m.cellH;
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
        for (Face& f : faces) if (f.face) sized(f.face);
        for (FT_Face f : fallbackFaces) sized(f);
    }
};
