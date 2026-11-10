// Text from font files that come with the program (Noto Sans, SIL Open Font License; see
// third_party/fonts/OFL.txt), drawn with stb_truetype (public domain). For Android and iOS, where
// Windows' fonts are not there. The measures follow GDI's, so the start screen keeps its layout:
// the size is the em, the line is ascent + descent, and the letter spacing comes after every letter.
#include "text_raster.h"

#include "platform.h"

#include <SDL3/SDL.h>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#define STB_TRUETYPE_IMPLEMENTATION
#include "../third_party/stb/stb_truetype.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ui {

namespace {

struct LoadedFont {
    void* data = nullptr;  // the file, kept for stb_truetype
    stbtt_fontinfo info{};
    bool ok = false;
};

const char* file_of(Font font) {
    switch (font) {
        case Font::Heavy: return "fonts/NotoSans-Black.ttf";       // for Arial Black
        case Font::Semibold: return "fonts/NotoSans-SemiBold.ttf";  // for Segoe UI Semibold
        default: return "fonts/NotoSans-Regular.ttf";               // for Segoe UI
    }
}

const LoadedFont& font_for(Font font) {
    static LoadedFont fonts[3];
    const int index = font == Font::Heavy ? 0 : font == Font::Semibold ? 1 : 2;
    LoadedFont& f = fonts[index];
    if (f.data == nullptr) {
        std::size_t size = 0;
        const std::string path = platform::bundled_file(file_of(font));
        f.data = SDL_LoadFile(path.c_str(), &size);
        f.ok = f.data != nullptr
               && stbtt_InitFont(&f.info, static_cast<const unsigned char*>(f.data),
                                 stbtt_GetFontOffsetForIndex(static_cast<const unsigned char*>(f.data), 0))
                      != 0;
        if (!f.ok) std::printf("text: font %s cannot be read: %s\n", path.c_str(), SDL_GetError());
    }
    return f;
}

// UTF-8 to code points; broken bytes become U+FFFD.
std::vector<int> code_points(const std::string& s) {
    std::vector<int> out;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int length = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (length == 0 || i + std::size_t(length) > s.size()) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        int cp = length == 1 ? c : c & (0x7F >> length);
        for (int k = 1; k < length; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + std::size_t(k)]) & 0x3F);
        out.push_back(cp);
        i += std::size_t(length);
    }
    return out;
}

}  // namespace

Coverage rasterize_text(const std::string& utf8, Font font, int px, int spacing_px, int pad) {
    Coverage out;
    const LoadedFont& f = font_for(font);
    if (!f.ok) {
        out.w = out.h = 1 + 2 * pad;
        out.cover.assign(std::size_t(out.w) * out.h, 0.0f);
        return out;
    }
    const float scale = stbtt_ScaleForMappingEmToPixels(&f.info, float(px));
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&f.info, &ascent, &descent, &gap);
    const float baseline = float(ascent) * scale;
    const int text_h = std::max(1, int(std::lround(float(ascent - descent) * scale)));

    // Pen positions first, for the width.
    const std::vector<int> cps = code_points(utf8);
    std::vector<float> pen(cps.size());
    float x = 0;
    for (std::size_t i = 0; i < cps.size(); ++i) {
        pen[i] = x;
        int advance = 0, bearing = 0;
        stbtt_GetCodepointHMetrics(&f.info, cps[i], &advance, &bearing);
        x += float(advance) * scale + float(spacing_px);
        if (i + 1 < cps.size()) x += float(stbtt_GetCodepointKernAdvance(&f.info, cps[i], cps[i + 1])) * scale;
    }
    const int text_w = std::max(1, int(std::ceil(x)));
    out.text_w = text_w;
    out.w = text_w + 2 * pad;
    out.h = text_h + 2 * pad;
    out.cover.assign(std::size_t(out.w) * out.h, 0.0f);

    std::vector<unsigned char> glyph;
    for (std::size_t i = 0; i < cps.size(); ++i) {
        const float origin_x = float(pad) + pen[i];
        const float origin_y = float(pad) + baseline;
        const float shift_x = origin_x - std::floor(origin_x), shift_y = origin_y - std::floor(origin_y);
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetCodepointBitmapBoxSubpixel(&f.info, cps[i], scale, scale, shift_x, shift_y, &x0, &y0, &x1, &y1);
        const int gw = x1 - x0, gh = y1 - y0;
        if (gw <= 0 || gh <= 0) continue;
        glyph.assign(std::size_t(gw) * gh, 0);
        stbtt_MakeCodepointBitmapSubpixel(&f.info, glyph.data(), gw, gh, gw, scale, scale, shift_x, shift_y, cps[i]);
        const int left = int(std::floor(origin_x)) + x0, top = int(std::floor(origin_y)) + y0;
        for (int gy = 0; gy < gh; ++gy) {
            const int y = top + gy;
            if (y < 0 || y >= out.h) continue;
            for (int gx = 0; gx < gw; ++gx) {
                const int px_x = left + gx;
                if (px_x < 0 || px_x >= out.w) continue;
                float& c = out.cover[std::size_t(y) * out.w + px_x];
                c = std::min(1.0f, c + glyph[std::size_t(gy) * gw + gx] / 255.0f);
            }
        }
    }
    return out;
}

}  // namespace ui
