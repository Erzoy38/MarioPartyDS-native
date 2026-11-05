// The 3D hardware: geometry commands go in through the FIFO or the direct ports, and a finished
// polygon list is drawn into a picture the 2D engine shows as background 0.
//
// Geometry is exact integer work, because the game can read results back. The rasterizer uses
// floats: its picture never flows back into the game, so it cannot make players' games diverge.
#pragma once

#include "cpu.h"

void gpu3d_write(u32 addr, u32 size, u32 value);  // 0x04000400 to 0x040005FF
u32 gpu3d_read(u32 addr);                         // 0x04000600 to 0x040006A3
void gpu3d_write_status(u32 value);               // GXSTAT
void gpu3d_vblank();                              // a waiting list is swapped in and drawn

// One line of the last finished picture. Colour in bits 0-14, alpha 0-31 in bits 16-20; alpha 0
// means nothing is there.
const u32* gpu3d_line(u32 line);

struct Gpu3dStats {
    u64 lists_drawn = 0;
    u32 last_polygons = 0;
    u32 last_vertices = 0;
    u64 unknown_commands = 0;
};
const Gpu3dStats& gpu3d_stats();
