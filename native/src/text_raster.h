// Turns a line of text into coverage values (0..1 per pixel), for ui.cpp to colour and outline.
// Windows: GDI with the system's fonts (text_raster_win.cpp). Android/iOS: a font file of their own.
#pragma once

#include "ui.h"

#include <string>
#include <vector>

namespace ui {

struct Coverage {
    int w = 1, h = 1;  // the whole image, with `pad` on every side
    int text_w = 1;    // the text's own width
    std::vector<float> cover;
};

// `px`: the font's em in pixels; `spacing_px`: extra pixels between letters.
Coverage rasterize_text(const std::string& utf8, Font font, int px, int spacing_px, int pad);

}  // namespace ui
