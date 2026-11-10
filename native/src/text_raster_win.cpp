// Text through GDI: Arial Black, Segoe UI and Segoe UI Semibold, as Windows has them.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "text_raster.h"

#include <algorithm>
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

}  // namespace

Coverage rasterize_text(const std::string& utf8, Font style_font, int px, int spacing_px, int pad) {
    const std::wstring wide = widen(utf8);
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT font = CreateFontW(-px, 0, 0, 0, weight(style_font), FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face(style_font));
    HGDIOBJ old_font = SelectObject(dc, font);
    SetTextCharacterExtra(dc, spacing_px);
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
    Coverage out;
    out.w = w;
    out.h = h;
    out.text_w = text_w;
    const BYTE* source = static_cast<const BYTE*>(bits);
    out.cover.resize(size_t(w) * h);
    for (size_t i = 0; i < out.cover.size(); ++i) out.cover[i] = source[i * 4 + 1] / 255.0f;

    SelectObject(dc, old_bitmap);
    SelectObject(dc, old_font);
    DeleteObject(bitmap);
    DeleteObject(font);
    DeleteDC(dc);
    return out;
}

}  // namespace ui
