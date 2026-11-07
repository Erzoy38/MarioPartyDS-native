#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "ui.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ui {

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

const wchar_t* face(Font font) {
    switch (font) {
        case Font::Heavy: return L"Arial Black";
        case Font::Semibold: return L"Segoe UI Semibold";
        default: return L"Segoe UI";
    }
}

int weight(Font font) {
    switch (font) {
        case Font::Heavy: return FW_BLACK;
        case Font::Semibold: return FW_SEMIBOLD;
        default: return FW_NORMAL;
    }
}

SDL_FColor fcolour(Rgba c) { return {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f}; }

Rgba mix(Rgba a, Rgba b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    auto m = [t](Uint8 x, Uint8 y) { return Uint8(std::lround(x + (y - x) * t)); };
    return {m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), m(a.a, b.a)};
}

// The outline of a rounded box, clockwise from the top left corner.
std::vector<SDL_FPoint> rounded_points(const SDL_FRect& box, float radius) {
    radius = std::min({radius, box.w / 2, box.h / 2});
    std::vector<SDL_FPoint> points;
    constexpr int kSteps = 8;
    const float pi = 3.14159265f;
    const SDL_FPoint centres[4] = {{box.x + radius, box.y + radius},
                                   {box.x + box.w - radius, box.y + radius},
                                   {box.x + box.w - radius, box.y + box.h - radius},
                                   {box.x + radius, box.y + box.h - radius}};
    for (int corner = 0; corner < 4; ++corner) {
        const float start = pi + corner * pi / 2;
        for (int i = 0; i <= kSteps; ++i) {
            const float a = start + (pi / 2) * float(i) / kSteps;
            points.push_back({centres[corner].x + radius * std::cos(a), centres[corner].y + radius * std::sin(a)});
        }
    }
    return points;
}

// A convex shape as a fan from its middle; each corner's colour from `colour_at`.
template <typename ColourAt>
void fill_fan(SDL_Renderer* r, const std::vector<SDL_FPoint>& points, ColourAt colour_at) {
    if (points.size() < 3) return;
    SDL_FPoint middle{0, 0};
    for (const SDL_FPoint& p : points) {
        middle.x += p.x;
        middle.y += p.y;
    }
    middle.x /= float(points.size());
    middle.y /= float(points.size());
    std::vector<SDL_Vertex> vertices;
    vertices.reserve(points.size() + 1);
    vertices.push_back({middle, fcolour(colour_at(middle)), {0, 0}});
    for (const SDL_FPoint& p : points) vertices.push_back({p, fcolour(colour_at(p)), {0, 0}});
    std::vector<int> indices;
    for (size_t i = 0; i < points.size(); ++i) {
        indices.push_back(0);
        indices.push_back(int(i + 1));
        indices.push_back(int((i + 1) % points.size() + 1));
    }
    SDL_RenderGeometry(r, nullptr, vertices.data(), int(vertices.size()), indices.data(), int(indices.size()));
}

}  // namespace

Text::~Text() {
    for (auto& [key, image] : cache_) SDL_DestroyTexture(image.texture);
}

float Text::pixel_scale() {
    int w = 0, h = 0;
    SDL_RendererLogicalPresentation mode;
    SDL_GetRenderLogicalPresentation(renderer_, &w, &h, &mode);
    SDL_FRect shown{};
    SDL_GetRenderLogicalPresentationRect(renderer_, &shown);
    const float scale = (h > 0 && shown.h > 0) ? shown.h / float(h) : 1.0f;
    if (std::abs(scale - scale_) > 0.01f) {
        for (auto& [key, image] : cache_) SDL_DestroyTexture(image.texture);
        cache_.clear();
        scale_ = scale;
    }
    return scale_;
}

const Text::Image& Text::image(const std::string& text, const TextStyle& style) {
    const float scale = pixel_scale();
    char key_head[160];
    SDL_snprintf(key_head, sizeof key_head, "%d|%.2f|%02x%02x%02x%02x|%02x%02x%02x%02x|%.2f|%.2f|",
                 int(style.font), style.size, style.fill.r, style.fill.g, style.fill.b, style.fill.a,
                 style.outline.r, style.outline.g, style.outline.b, style.outline.a, style.outline_width, style.spacing);
    const std::string key = key_head + text;
    auto found = cache_.find(key);
    if (found != cache_.end()) return found->second;

    const std::wstring wide = widen(text);
    const int px = std::max(1, int(std::lround(style.size * scale)));
    const float outline_px = style.outline.a > 0 ? style.outline_width * scale : 0;
    const int pad = int(std::ceil(outline_px)) + px / 6 + 2;

    HDC dc = CreateCompatibleDC(nullptr);
    HFONT font = CreateFontW(-px, 0, 0, 0, weight(style.font), FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face(style.font));
    HGDIOBJ old_font = SelectObject(dc, font);
    SetTextCharacterExtra(dc, int(std::lround(style.spacing * scale)));
    SIZE size{0, 0};
    if (!wide.empty()) GetTextExtentPoint32W(dc, wide.c_str(), int(wide.size()), &size);
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    const int text_w = std::max<int>(size.cx, 1);
    const int text_h = std::max<int>(metrics.tmHeight, 1);
    const int w = text_w + 2 * pad, h = text_h + 2 * pad;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof info.bmiHeader;
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h;  // top down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
    std::memset(bits, 0, size_t(w) * h * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    if (!wide.empty()) TextOutW(dc, pad, pad, wide.c_str(), int(wide.size()));
    GdiFlush();

    // White text on black: any channel is the coverage.
    const Uint8* source = static_cast<const Uint8*>(bits);
    std::vector<float> cover(size_t(w) * h);
    for (size_t i = 0; i < cover.size(); ++i) cover[i] = source[i * 4 + 1] / 255.0f;
    std::vector<float> ring(cover.size(), 0.0f);
    if (outline_px > 0) {
        const int reach = int(std::ceil(outline_px));
        struct Offset { int dx, dy; float weight; };
        std::vector<Offset> offsets;
        for (int dy = -reach; dy <= reach; ++dy) {
            for (int dx = -reach; dx <= reach; ++dx) {
                const float weight_here = std::clamp(outline_px + 0.5f - std::sqrt(float(dx * dx + dy * dy)), 0.0f, 1.0f);
                if (weight_here > 0) offsets.push_back({dx, dy, weight_here});
            }
        }
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                float best = 0;
                for (const Offset& o : offsets) {
                    const int sx = x + o.dx, sy = y + o.dy;
                    if (sx < 0 || sy < 0 || sx >= w || sy >= h) continue;
                    best = std::max(best, cover[size_t(sy) * w + sx] * o.weight);
                    if (best >= 1.0f) break;
                }
                ring[size_t(y) * w + x] = best;
            }
        }
    }
    std::vector<Uint8> rgba(size_t(w) * h * 4);
    const float fill_a = style.fill.a / 255.0f, outline_a = style.outline.a / 255.0f;
    for (size_t i = 0; i < cover.size(); ++i) {
        const float f = cover[i] * fill_a;
        const float o = ring[i] * outline_a * (1 - f);
        const float a = f + o;
        Uint8* p = &rgba[i * 4];
        if (a <= 0) {
            p[0] = p[1] = p[2] = p[3] = 0;
            continue;
        }
        p[0] = Uint8(std::lround((style.fill.r * f + style.outline.r * o) / a));
        p[1] = Uint8(std::lround((style.fill.g * f + style.outline.g * o) / a));
        p[2] = Uint8(std::lround((style.fill.b * f + style.outline.b * o) / a));
        p[3] = Uint8(std::lround(std::min(a, 1.0f) * 255));
    }
    SelectObject(dc, old_bitmap);
    SelectObject(dc, old_font);
    DeleteObject(bitmap);
    DeleteObject(font);
    DeleteDC(dc);

    Image made;
    made.texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
    SDL_UpdateTexture(made.texture, nullptr, rgba.data(), w * 4);
    SDL_SetTextureBlendMode(made.texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(made.texture, SDL_SCALEMODE_LINEAR);
    made.width = float(text_w) / scale;
    made.pad = float(pad) / scale;
    made.w = float(w) / scale;
    made.h = float(h) / scale;
    return cache_.emplace(key, made).first->second;
}

float Text::draw(const std::string& text, float x, float y, const TextStyle& style, float angle) {
    const Image& made = image(text, style);
    const SDL_FRect where{x - made.pad, y - made.pad, made.w, made.h};
    if (angle == 0) SDL_RenderTexture(renderer_, made.texture, nullptr, &where);
    else SDL_RenderTextureRotated(renderer_, made.texture, nullptr, &where, angle, nullptr, SDL_FLIP_NONE);
    return made.width;
}

float Text::width(const std::string& text, const TextStyle& style) { return image(text, style).width; }

std::vector<std::string> wrap(Text& text, const std::string& s, const TextStyle& style, float limit) {
    std::vector<std::string> lines;
    std::string line, word;
    auto put = [&] {
        if (word.empty()) return;
        const std::string longer = line.empty() ? word : line + " " + word;
        if (!line.empty() && text.width(longer, style) > limit) {
            lines.push_back(line);
            line = word;
        } else {
            line = longer;
        }
        word.clear();
    };
    for (char c : s) {
        if (c == ' ') put();
        else if (c == '\n') {
            put();
            lines.push_back(line);
            line.clear();
        } else word += c;
    }
    put();
    if (!line.empty()) lines.push_back(line);
    return lines;
}

void fill_rounded(SDL_Renderer* r, const SDL_FRect& box, float radius, Rgba colour) {
    fill_fan(r, rounded_points(box, radius), [colour](SDL_FPoint) { return colour; });
}

void fill_rounded_gradient(SDL_Renderer* r, const SDL_FRect& box, float radius, Rgba left, Rgba right) {
    fill_fan(r, rounded_points(box, radius), [&](SDL_FPoint p) { return mix(left, right, (p.x - box.x) / box.w); });
}

void outline_rounded(SDL_Renderer* r, const SDL_FRect& box, float radius, Rgba colour) {
    std::vector<SDL_FPoint> points = rounded_points(box, radius);
    points.push_back(points.front());
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, colour.r, colour.g, colour.b, colour.a);
    SDL_RenderLines(r, points.data(), int(points.size()));
}

void fill_vertical_gradient(SDL_Renderer* r, const SDL_FRect& box, Rgba top, Rgba bottom) {
    const std::vector<SDL_FPoint> points{{box.x, box.y}, {box.x + box.w, box.y}, {box.x + box.w, box.y + box.h},
                                         {box.x, box.y + box.h}};
    fill_fan(r, points, [&](SDL_FPoint p) { return mix(top, bottom, (p.y - box.y) / box.h); });
}

void fill_horizontal_gradient(SDL_Renderer* r, const SDL_FRect& box, Rgba left, Rgba right) {
    const std::vector<SDL_FPoint> points{{box.x, box.y}, {box.x + box.w, box.y}, {box.x + box.w, box.y + box.h},
                                         {box.x, box.y + box.h}};
    fill_fan(r, points, [&](SDL_FPoint p) { return mix(left, right, (p.x - box.x) / box.w); });
}

}  // namespace ui
