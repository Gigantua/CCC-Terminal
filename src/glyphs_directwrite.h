#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>
#include "../shell/console.h"
#include "boxdraw.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

struct FontMetrics {
    int cellW = 8, cellH = 16;
    int baseline = 12;
    int underlineTop = 14, underlineBottom = 15;
    int strikeTop = 8, strikeBottom = 9;
};

class GlyphCache {
public:
    static constexpr uint32_t COLOR_GLYPH = 8;
    static constexpr int STYLES = 4, MAX_CELLS = 8;

    bool init() {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return false;
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()))) return false;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite.GetAddressOf())))) return false;
        return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)));
    }

    bool setFont(const std::string& name, float pixelSize) {
        std::wstring requested = ccc::W(name);
        std::wstring family = installed(requested) ? requested : installed(L"Cascadia Mono") ? L"Cascadia Mono" : L"Consolas";
        if (!measure(family, pixelSize)) return false;
        static const DWRITE_FONT_WEIGHT weights[STYLES] = { DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_WEIGHT_BOLD };
        static const DWRITE_FONT_STYLE styles[STYLES] = { DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STYLE_ITALIC, DWRITE_FONT_STYLE_ITALIC };
        for (int s = 0; s < STYLES; ++s) {
            formats[s].Reset();
            if (FAILED(dwrite->CreateTextFormat(family.c_str(), nullptr, weights[s], styles[s], DWRITE_FONT_STRETCH_NORMAL, pixelSize, L"en-us", &formats[s]))) return false;
            formats[s]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            formats[s]->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, (float)m.cellH, (float)m.baseline);
        }
        bitmap.Reset();
        target.Reset();
        brush.Reset();
        if (FAILED(wic->CreateBitmap(MAX_CELLS * m.cellW, m.cellH, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap))) return false;
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        if (FAILED(d2d->CreateWicBitmapRenderTarget(bitmap.Get(), props, &target))) return false;
        target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        target->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush);
        fontFamily = ccc::U(family);
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
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IDWriteTextFormat> formats[STYLES];
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1RenderTarget> target;
    ComPtr<ID2D1SolidColorBrush> brush;
    std::string fontFamily;
    FontMetrics m;
    uint32_t* atlas = nullptr;
    uint32_t capacity = 0;
    uint32_t nextTile = 1;
    bool overflowed = false;
    std::array<uint32_t, 128 * STYLES> ascii{};
    std::unordered_map<uint64_t, uint32_t> singles;
    std::unordered_map<std::u32string, uint32_t> clusters;

    bool installed(const std::wstring& family) {
        if (family.empty()) return false;
        ComPtr<IDWriteFontCollection> fonts;
        dwrite->GetSystemFontCollection(&fonts);
        UINT32 index;
        BOOL exists = FALSE;
        fonts->FindFamilyName(family.c_str(), &index, &exists);
        return exists != FALSE;
    }

    bool measure(const std::wstring& family, float pixelSize) {
        ComPtr<IDWriteFontCollection> fonts;
        dwrite->GetSystemFontCollection(&fonts);
        UINT32 index;
        BOOL exists = FALSE;
        fonts->FindFamilyName(family.c_str(), &index, &exists);
        if (!exists) return false;
        ComPtr<IDWriteFontFamily> fam;
        ComPtr<IDWriteFont> font;
        ComPtr<IDWriteFontFace> face;
        fonts->GetFontFamily(index, &fam);
        fam->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font);
        font->CreateFontFace(&face);
        DWRITE_FONT_METRICS fm;
        face->GetMetrics(&fm);
        UINT32 zero = '0';
        UINT16 index0 = 0;
        face->GetGlyphIndices(&zero, 1, &index0);
        DWRITE_GLYPH_METRICS gm;
        face->GetDesignGlyphMetrics(&index0, 1, &gm, FALSE);
        float scale = pixelSize / fm.designUnitsPerEm;
        m.cellW = (std::max)(1, (int)std::lround(gm.advanceWidth * scale));
        float gap = fm.lineGap * scale;
        m.cellH = (std::max)(1, (int)std::ceil((fm.ascent + fm.descent) * scale + gap));
        m.baseline = (int)std::lround(fm.ascent * scale + gap / 2);
        int underlineH = (std::max)(1, (int)std::lround(fm.underlineThickness * scale));
        m.underlineTop = (std::min)(m.cellH - underlineH, m.baseline - (int)std::lround(fm.underlinePosition * scale));
        m.underlineBottom = m.underlineTop + underlineH;
        int strikeH = (std::max)(1, (int)std::lround(fm.strikethroughThickness * scale));
        m.strikeTop = m.baseline - (int)std::lround(fm.strikethroughPosition * scale);
        m.strikeBottom = m.strikeTop + strikeH;
        return true;
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
        std::wstring utf16;
        for (char32_t cp : text) if (cp < Console::SIZED) unicode::appendUtf16(utf16, cp);
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwrite->CreateTextLayout(utf16.data(), (UINT32)utf16.size(), formats[style].Get(), (float)(MAX_CELLS * m.cellW), (float)m.cellH, &layout))) return tile;
        Console::Sizing z;
        bool scaled = Console::sizingOf(text.data(), text.size(), z) && z.scaled();
        float k = scaled ? z.s * (z.d ? (float)z.n / z.d : 1.0f) : 1.0f, ox = 0, oy = 0;
        int cells = scaled ? z.w : width;
        bool emoji = cells == 2 || text.find(unicode::VS_EMOJI) != std::u32string::npos;
        DWRITE_OVERHANG_METRICS om = {};
        layout->GetOverhangMetrics(&om);
        float room = (float)(cells * m.cellW), inkRight = (float)(MAX_CELLS * m.cellW) + om.right;
        float fit = inkRight > room + 0.5f ? room / inkRight : 1.0f;
        if (scaled) {
            ox = alignedOffset(z.h, width * m.cellW - room * k);
            oy = alignedOffset(z.v, (z.s - k) * m.cellH) - z.row * m.cellH;
        }
        bool colored = false;
        for (int first = 0; first < width; first += MAX_CELLS) {
            target->BeginDraw();
            target->Clear(D2D1::ColorF(0, 0, 0, 0));
            target->SetTransform(D2D1::Matrix3x2F::Scale(fit, fit, D2D1::Point2F(0, (float)m.baseline)) * D2D1::Matrix3x2F::Scale(k, k) *
                                 D2D1::Matrix3x2F::Translation(ox - first * m.cellW, oy));
            target->DrawTextLayout(D2D1::Point2F(0, 0), layout.Get(), brush.Get(),
                                   emoji ? D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT | D2D1_DRAW_TEXT_OPTIONS_CLIP : D2D1_DRAW_TEXT_OPTIONS_CLIP);
            target->SetTransform(D2D1::Matrix3x2F::Identity());
            target->EndDraw();
            WICRect rect = { 0, 0, MAX_CELLS * m.cellW, m.cellH };
            ComPtr<IWICBitmapLock> lock;
            if (FAILED(bitmap->Lock(&rect, WICBitmapLockRead, &lock))) return tile;
            UINT stride = 0, size = 0;
            BYTE* pixels = nullptr;
            lock->GetStride(&stride);
            lock->GetDataPointer(&size, &pixels);
            for (int t = first; t < (std::min)(width, first + MAX_CELLS); ++t) {
                uint32_t* dst = atlas + (size_t)(tile + t) * tileTexels();
                for (int y = 0; y < m.cellH; ++y) {
                    const uint32_t* src = reinterpret_cast<const uint32_t*>(pixels + (size_t)y * stride) + (t - first) * m.cellW;
                    for (int x = 0; x < m.cellW; ++x) {
                        uint32_t p = src[x];
                        int b = p & 255, g = p >> 8 & 255, r = p >> 16 & 255;
                        if (std::abs(r - g) > 8 || std::abs(g - b) > 8) colored = true;
                        dst[y * m.cellW + x] = p;
                    }
                }
            }
        }
        return tile | (colored ? COLOR_GLYPH << 24 : 0);
    }

    static float alignedOffset(int align, float room) { return align == 1 ? room : align == 2 ? room / 2 : 0;
    }
};
