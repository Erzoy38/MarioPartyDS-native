// How the game appears in the window: resolution, the arrangement of the two screens, their shape,
// and texture packs (folder `textures` next to the program).
#pragma once

#include <SDL3/SDL.h>

#include "launcher.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

enum ScreenLayout { kLayoutStacked = 0, kLayoutSideBySide = 1, kLayoutHybrid = 2, kLayoutCount = 3 };
enum ScreenAspect { kAspect4x3 = 0, kAspect16x9 = 1, kAspectFill = 2, kAspectCount = 3 };

const char* layout_name(int layout);
const char* aspect_name(int aspect);

// Where the two screens go in an output of ow x oh pixels.
struct ScreenRects {
    SDL_FRect top, bottom;
};
ScreenRects screen_rects(float ow, float oh, int layout, int aspect);

// "1920x1080" -> 1920, 1080; false for "" (automatic) or anything else.
bool parse_resolution(const std::string& text, int& w, int& h);

// The resolutions of the main display, smallest first, without repeats.
std::vector<std::pair<int, int>> display_resolutions();

// The window size for "automatic": the screens at `scale` times the DS's size.
void automatic_window_size(int layout, int aspect, int scale, int& w, int& h);

// Window size or fullscreen mode from the settings (`scale` for automatic).
void apply_window_mode(SDL_Window* window, const AppSettings& settings, int scale, bool fullscreen);

// --- Texture packs ---
std::filesystem::path texture_folder();
// Indexes the pack and hands the loading and dumping to the 3D renderer.
void texture_pack_start(int render_scale, bool replace, bool dump);
// Opens the texture folder in Explorer (made first if missing).
void open_texture_folder();
