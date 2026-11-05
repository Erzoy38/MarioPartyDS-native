// The two 2D engines: draws one scanline at a time from the registers, VRAM, palette and OAM.
// 3D comes later: where the game shows 3D in background 0, this layer stays see-through.
#pragma once

#include "cpu.h"

constexpr int kScreenWidth = 256;
constexpr int kScreenHeight = 192;

void ppu_render_line(u32 line);
void ppu_finish_frame();  // the finished picture becomes the one to show

// RGBA, top screen followed by the bottom screen: 256 x 384 pixels.
const u8* ppu_frame();

// How often a line asked for a feature that is not drawn yet. For honest reporting in tests.
struct PpuGaps {
    u64 lines_with_3d = 0;
    u64 lines_with_capture = 0;
};
const PpuGaps& ppu_gaps();
