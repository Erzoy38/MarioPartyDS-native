// The two 2D engines: draws one scanline at a time from the registers, VRAM, palette and OAM.
// Background 0 can show the 3D picture from gpu3d.
#pragma once

#include "cpu.h"

#include <vector>

constexpr int kScreenWidth = 256;
constexpr int kScreenHeight = 192;

void ppu_render_line(u32 line);
void ppu_finish_frame();  // the finished picture becomes the one to show

// RGBA, top screen followed by the bottom screen: 256 x 384 pixels.
const u8* ppu_frame();

// The last frame for the screen at gpu3d_display_scale() times the size: where the frame shows the
// 3D picture alone, the pixels come from the screen's own 3D picture (higher resolution, texture
// pack); everywhere else the game's pixels are made larger. `scale` is set. False if there is no
// screen picture yet (then `out` is the game's frame made larger).
// upscaled (may be null) is the game's frame already made larger to the same size; it is used for the
// pixels that do not come from the 3D picture.
bool ppu_compose_display(std::vector<u8>& out, int& scale, const u8* upscaled = nullptr);

// How many lines used 3D and display capture, so a test can tell what it actually exercised.
struct PpuGaps {
    u64 lines_with_3d = 0;
    u64 lines_with_capture = 0;
};
const PpuGaps& ppu_gaps();
