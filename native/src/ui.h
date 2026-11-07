// Drawing for the start screen: text in Windows' own fonts (GDI, with an outline), rounded boxes
// and gradients. Coordinates are logical, as set with SDL_SetRenderLogicalPresentation; text is
// made at the window's real pixel size, so it stays sharp when the window is large.
#pragma once

#include <SDL3/SDL.h>

#include <map>
#include <string>
#include <vector>

namespace ui {

struct Rgba {
    Uint8 r = 0, g = 0, b = 0, a = 255;
};

enum class Font { Heavy, Plain, Semibold };  // Arial Black, Segoe UI, Segoe UI Semibold

struct TextStyle {
    Font font = Font::Plain;
    float size = 14;           // logical pixels, the height of the font's em
    Rgba fill{255, 255, 255, 255};
    Rgba outline{0, 0, 0, 0};  // alpha 0: none
    float outline_width = 0;   // logical pixels
    float spacing = 0;         // extra logical pixels between letters
};

class Text {
public:
    explicit Text(SDL_Renderer* renderer) : renderer_(renderer) {}
    ~Text();
    Text(const Text&) = delete;
    Text& operator=(const Text&) = delete;

    // Draws `text` (UTF-8) with the top of its line at y; returns its width. `angle` turns it
    // around its middle, in degrees.
    float draw(const std::string& text, float x, float y, const TextStyle& style, float angle = 0);
    float width(const std::string& text, const TextStyle& style);

private:
    struct Image {
        SDL_Texture* texture = nullptr;
        float width = 0;   // of the text itself, logical
        float pad = 0;     // around it in the texture, logical
        float w = 0, h = 0;  // the whole texture, logical
    };
    const Image& image(const std::string& text, const TextStyle& style);
    float pixel_scale();

    SDL_Renderer* renderer_;
    float scale_ = 0;
    std::map<std::string, Image> cache_;
};

// Breaks `text` into lines no wider than `limit`, between words.
std::vector<std::string> wrap(Text& text, const std::string& s, const TextStyle& style, float limit);

void fill_rounded(SDL_Renderer* r, const SDL_FRect& box, float radius, Rgba colour);
// From `left` at the left edge to `right` at the right edge.
void fill_rounded_gradient(SDL_Renderer* r, const SDL_FRect& box, float radius, Rgba left, Rgba right);
void outline_rounded(SDL_Renderer* r, const SDL_FRect& box, float radius, Rgba colour);
// From `top` at the top edge to `bottom` at the bottom edge.
void fill_vertical_gradient(SDL_Renderer* r, const SDL_FRect& box, Rgba top, Rgba bottom);
void fill_horizontal_gradient(SDL_Renderer* r, const SDL_FRect& box, Rgba left, Rgba right);

}  // namespace ui
