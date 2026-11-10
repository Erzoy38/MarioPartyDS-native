// The 3D hardware: geometry commands go in through the FIFO or the direct ports, and a finished
// polygon list is drawn into a picture the 2D engine shows as background 0.
//
// Geometry is exact integer work, because the game can read results back. The rasterizer uses
// floats: its picture never flows back into the game, so it cannot make players' games diverge.
#pragma once

#include "cpu.h"

#include <vector>

void gpu3d_write(u32 addr, u32 size, u32 value);  // 0x04000400 to 0x040005FF
u32 gpu3d_read(u32 addr);                         // 0x04000600 to 0x040006A3
void gpu3d_write_status(u32 value);               // GXSTAT
void gpu3d_vblank();                              // a waiting list is swapped in and drawn

// One line of the last finished picture: 6 bits each of red, green and blue in bits 0-17, alpha
// 0-31 in bits 18-22. Alpha 0 means nothing is there.
const u32* gpu3d_line(u32 line);

struct Gpu3dStats {
    u64 lists_drawn = 0;
    u32 last_polygons = 0;
    u32 last_vertices = 0;
    u64 unknown_commands = 0;
    u64 textures_replaced = 0;  // replacement textures loaded from the texture pack
};

// --- The picture for the screen ---
// Besides the game's own 256 x 192 picture, the 3D list can be drawn a second time only for the
// screen: at `scale` times the resolution and with a texture pack's pictures. That one never goes
// back into the game, so online games stay alike whatever each player sets.
//
// Textures are named by a 64-bit key: a hash of the texture as the game shows it (RGBA, r in the
// low byte). `load` gives a replacement of any size (a multiple of the original makes sense);
// `dump` receives each texture once. Both may be null.
struct Gpu3dTextureHooks {
    bool (*load)(u64 key, int& w, int& h, std::vector<u32>& rgba) = nullptr;
    void (*dump)(u64 key, int w, int h, const std::vector<u32>& rgba) = nullptr;
};
void gpu3d_set_display(int scale, bool replace_textures, bool dump_textures, const Gpu3dTextureHooks& hooks);
int gpu3d_display_scale();
// Which of two pictures was drawn last (-1: none yet); the 2D engines note it for each frame.
int gpu3d_display_index();
// (256 * scale) x (192 * scale), in the format of gpu3d_line. Null if not drawn.
const u32* gpu3d_display_picture(int index);

// For DLSS: the screen's picture at any size (w, h; 0, 0 back to 256 * scale x 192 * scale), and
// a sample point moved by (x, y) of its pixels for the next list drawn. Pixel i then samples the
// game's picture at (i + 0.5 + x) / w of its width.
void gpu3d_set_display_size(int w, int h);
void gpu3d_set_display_jitter(float x, float y);
struct Gpu3dDisplayInfo {
    int w = 0, h = 0;
    float jitter_x = 0, jitter_y = 0;  // what this picture was drawn with
    const float* depth = nullptr;      // w x h, 0 near to 1 far; only with a free size
    // w x h pairs (dx, dy) in these pixels: where the pixel's surface was in the list before (found
    // by matching polygons; 0, 0 where unknown). Only with a free size.
    const float* motion = nullptr;
    const u8* fresh = nullptr;  // w x h: 255 where the surface's polygon had none before it
    u64 serial = 0;  // which list this is; the same number means the same picture
};
bool gpu3d_display_info(int index, Gpu3dDisplayInfo& info);
const Gpu3dStats& gpu3d_stats();
void gpu3d_print_last_list();  // for finding out why a scene looks wrong
