struct Cell
{
    uint glyph;
    uint fg;
    uint bg;
    uint underline;
};

cbuffer FrameConstants : register(b0)
{
    uint2 cellSize;
    uint2 termSize;
    uint blinkOn;
    uint marginColor;
    uint barX;
    uint thumbTop;
    uint thumbBottom;
    uint barColor;
    uint thumbColor;
    uint underlineTop;
    uint underlineBottom;
    uint strikeTop;
    uint strikeBottom;
    uint topInset;
    uint stripOffset;
    uint stripCols;
    uint stripX;
    uint stripTextTop;
    uint stripTabTop;
    uint stripColor;
    uint buttonsX;
    uint buttonW;
    uint buttonHover;
    uint buttonPressed;
    uint iconColor;
    uint iconHalf;
    uint iconStroke;
    uint maximized;
    uint closeColor;
    uint tabShape;
    uint pictureCount;
};

struct Picture
{
    int x0;
    int y0;
    int w;
    int h;
    uint offset;
    uint width;
    uint srcX;
    uint srcY;
    uint srcW;
    uint srcH;
    int z;
    uint pad;
};

StructuredBuffer<Cell> cells : register(t0);
struct AtlasTexel
{
    uint rgba;
};

struct PictureTexel
{
    uint rgba;
};

StructuredBuffer<AtlasTexel> atlas : register(t1);
StructuredBuffer<PictureTexel> pictureTexels : register(t2);
StructuredBuffer<Picture> pictures : register(t3);

static const uint CURSOR_BEAM = 1;
static const uint BLINK = 2;
static const uint STRIKE = 4;
static const uint COLOR_GLYPH = 8;
static const uint CURSOR_BAR = 16;
static const uint CURSOR_HOLLOW = 32;

float3 unpackColor(uint c)
{
    return float3(c & 255, (c >> 8) & 255, (c >> 16) & 255) / 255.0;
}

float3 blendGlyphOver(Cell cell, uint2 texel, float3 bg)
{
    uint t = atlas[(cell.glyph & 0xFFFFFF) * cellSize.x * cellSize.y + texel.y * cellSize.x + texel.x].rgba;
    float4 ink = float4((t >> 16) & 255, (t >> 8) & 255, t & 255, t >> 24) / 255.0;
    return ((cell.glyph >> 24) & COLOR_GLYPH) ? ink.rgb + bg * (1 - ink.a) : lerp(bg, unpackColor(cell.fg), ink.a);
}

float4 pictureTexel(Picture pic, uint x, uint y)
{
    uint t = pictureTexels[pic.offset + y * pic.width + x].rgba;
    return float4(t & 255, (t >> 8) & 255, (t >> 16) & 255, t >> 24) / 255.0;
}

float4 samplePicture(Picture pic, int2 q)
{
    float2 uv = (float2(q - int2(pic.x0, pic.y0)) + 0.5) / float2(pic.w, pic.h);
    float2 s = uv * float2(pic.srcW, pic.srcH) - 0.5;
    float2 base = floor(s);
    float2 f = s - base;
    int2 hi = int2(pic.srcW, pic.srcH) - 1;
    int2 a = clamp(int2(base), int2(0, 0), hi);
    int2 b = clamp(int2(base) + 1, int2(0, 0), hi);
    uint2 o = uint2(pic.srcX, pic.srcY);
    float4 top = lerp(pictureTexel(pic, o.x + a.x, o.y + a.y), pictureTexel(pic, o.x + b.x, o.y + a.y), f.x);
    float4 bottom = lerp(pictureTexel(pic, o.x + a.x, o.y + b.y), pictureTexel(pic, o.x + b.x, o.y + b.y), f.x);
    return lerp(top, bottom, f.y);
}

float3 overlayPictures(float3 color, int2 q, bool below)
{
    for (uint i = 0; i < pictureCount; ++i)
    {
        Picture pic = pictures[i];
        if ((pic.z < 0) != below || q.x < pic.x0 || q.y < pic.y0 || q.x >= pic.x0 + pic.w || q.y >= pic.y0 + pic.h)
            continue;
        float4 c = samplePicture(pic, q);
        color = lerp(color, c.rgb, c.a);
    }
    return color;
}

float segment(float2 q, float2 a, float2 b)
{
    float2 ab = b - a;
    float t = saturate(dot(q - a, ab) / dot(ab, ab));
    return length(q - a - ab * t);
}

float squareOutline(float2 q, float2 center, float extent)
{
    float2 d = abs(q - center);
    return abs(max(d.x, d.y) - extent);
}

float underlineCoverage(uint style, uint2 texel, uint x)
{
    float top = underlineTop, thick = max(1.0, float(underlineBottom) - float(underlineTop));
    float y = texel.y + 0.5;
    bool band = y >= top && y < top + thick;
    float coverage = 0;
    if (style == 1)
        coverage = band;
    else if (style == 2)
        coverage = band || (y >= top - 2 * thick && y < top - thick);
    else if (style == 3)
    {
        float amplitude = max(thick, cellSize.y / 16.0);
        float center = min(top + thick * 0.5, cellSize.y - amplitude - thick * 0.5);
        float phase = 6.2831853 * (x + 0.5) / cellSize.x;
        float wave = center + amplitude * sin(phase);
        float slope = amplitude * 6.2831853 / cellSize.x * cos(phase);
        coverage = saturate(thick * 0.5 + 0.5 - abs(y - wave) / sqrt(1 + slope * slope));
    }
    else if (style == 4)
        coverage = band && ((x / uint(thick * 2)) & 1) == 0;
    else if (style == 5)
        coverage = band && (x + cellSize.x / 8) % cellSize.x < cellSize.x * 3 / 4;
    return coverage;
}

float3 captionButton(uint2 p)
{
    uint which = min((p.x - buttonsX) / buttonW, 2u);
    float3 bar = unpackColor(stripColor);
    float3 ink = unpackColor(iconColor);
    float3 bg = bar;
    if (buttonHover == which + 1)
    {
        if (which == 2)
        {
            bg = unpackColor(closeColor);
            ink = float3(1, 1, 1);
        }
        else
            bg = lerp(bar, ink, buttonPressed == which + 1 ? 0.18 : 0.1);
    }
    float2 center = float2(buttonsX + which * buttonW + buttonW / 2, topInset / 2);
    float2 q = float2(p) + 0.5 - center;
    float s = iconHalf + 0.5;
    float d = 0;
    if (which == 0)
        d = segment(q, float2(-s, 0.5), float2(s, 0.5));
    else if (which == 2)
        d = min(segment(q, float2(-s, -s), float2(s, s)), segment(q, float2(-s, s), float2(s, -s)));
    else if (maximized != 0)
    {
        float o = round(s * 0.2);
        float front = squareOutline(q, float2(-o, o), s - o);
        float2 fr = abs(q - float2(-o, o));
        float behind = max(fr.x, fr.y) < s - o + iconStroke ? 1e9 : squareOutline(q, float2(o, -o), s - o);
        d = min(front, behind);
    }
    else
        d = squareOutline(q, float2(0, 0), s);
    float a = saturate(iconStroke * 0.5 + 0.5 - d);
    return lerp(bg, ink, a);
}

float3 stripPixel(uint2 p)
{
    if (p.x >= buttonsX)
        return captionButton(p);
    float3 bar = unpackColor(stripColor);
    if (p.x < stripX || p.y < stripTabTop)
        return bar;
    uint col = (p.x - stripX) / cellSize.x;
    if (col >= stripCols)
        return bar;
    Cell cell = cells[stripOffset + col];
    int ty = int(p.y) - int(stripTextTop);
    float3 color = unpackColor(cell.bg);
    if (ty >= 0 && ty < int(cellSize.y))
        color = blendGlyphOver(cell, uint2((p.x - stripX) % cellSize.x, uint(ty)), color);
    uint stripTabCols = tabShape & 255;
    uint stripTabCount = (tabShape >> 8) & 255;
    uint tabRadius = tabShape >> 16;
    if (stripTabCols > 0 && col < stripTabCols * stripTabCount && tabRadius > 0)
    {
        uint tab = col / stripTabCols;
        float left = stripX + tab * stripTabCols * cellSize.x;
        float right = left + (stripTabCols - 1) * cellSize.x;
        float r = tabRadius;
        float2 q = float2(p) + 0.5;
        float cx = q.x < left + r ? left + r : q.x > right - r ? right - r : q.x;
        float cy = stripTabTop + r;
        if (q.y < cy && cx != q.x)
            color = lerp(bar, color, saturate(r - length(q - float2(cx, cy)) + 0.5));
    }
    return color;
}

float4 main(float4 pos : SV_Position) : SV_Target
{
    uint2 p = uint2(pos.xy);
    if (p.y < topInset)
        return float4(stripPixel(p), 1);
    p.y -= topInset;
    if (p.x >= barX)
        return float4(unpackColor(p.y >= thumbTop && p.y < thumbBottom ? thumbColor : barColor), 1);
    uint2 c = p / cellSize;
    if (c.x >= termSize.x || c.y >= termSize.y)
        return float4(unpackColor(marginColor), 1);

    Cell cell = cells[c.y * termSize.x + c.x];
    uint2 texel = p % cellSize;
    uint flags = cell.glyph >> 24;
    float3 fg = unpackColor(cell.fg);
    float3 bg = unpackColor(cell.bg);
    float3 color = blendGlyphOver(cell, texel, overlayPictures(bg, int2(p), true));
    uint underlineStyle = cell.underline >> 24;
    if (underlineStyle != 0)
        color = lerp(color, unpackColor(cell.underline), underlineCoverage(underlineStyle, texel, p.x));
    if ((flags & STRIKE) && texel.y >= strikeTop && texel.y < strikeBottom)
        color = fg;
    if ((flags & BLINK) && blinkOn == 0)
        color = bg;
    if ((flags & CURSOR_BEAM) && texel.x < max(2u, cellSize.x / 8))
        color = fg;
    if ((flags & CURSOR_BAR) && texel.y >= cellSize.y - max(2u, cellSize.y / 6))
        color = fg;
    uint edge = max(1u, cellSize.y / 16);
    if ((flags & CURSOR_HOLLOW) && (texel.x < edge || texel.y < edge || texel.x >= cellSize.x - edge || texel.y >= cellSize.y - edge))
        color = fg;
    return float4(overlayPictures(color, int2(p), false), 1);
}
