#include "ppu.h"

#include "gpu3d.h"

#include <algorithm>
#include <thread>
#include <vector>
#include "memory.h"

#include <algorithm>
#include <cstring>

namespace {

constexpr u32 kOpaque = 0x8000;
constexpr u8 kNoPriority = 4;

// One 2D engine's view of the hardware.
struct Engine {
    int index;       // 0 = A, 1 = B
    u32 regs;        // offset of its registers in the I/O area
    u32 bg_vram;     // offset from 0x06000000
    u32 obj_vram;
    u32 palette;     // offset of its background palette; sprite palette follows at +0x200
    u32 oam;
    // Internal reference points of backgrounds 2 and 3, with the register values they came from.
    s32 ref_x[2] = {}, ref_y[2] = {};
    u32 seen_x[2] = {}, seen_y[2] = {};
    bool seen_valid[2] = {};
    u16 line_out[kScreenWidth] = {};
};

Engine g_engines[2] = {{0, 0x0000, 0x000000, 0x400000, 0x000, 0x000},
                       {1, 0x1000, 0x200000, 0x600000, 0x400, 0x400}};

u8 g_back[kScreenWidth * kScreenHeight * 2 * 4];
u8 g_front[sizeof g_back];
PpuGaps g_gaps;

u16 reg16(const Engine& e, u32 offset) {
    u16 value;
    std::memcpy(&value, &g_machine.io[e.regs + offset], 2);
    return value;
}

u32 reg32(const Engine& e, u32 offset) {
    u32 value;
    std::memcpy(&value, &g_machine.io[e.regs + offset], 4);
    return value;
}

u8 vram8(u32 offset) {
    const u8* page = vram_page(offset);
    return page ? page[offset & 0x3FFF] : 0;
}

u16 vram16(u32 offset) {
    offset &= ~1u;
    const u8* page = vram_page(offset);
    if (page == nullptr) return 0;
    u16 value;
    std::memcpy(&value, page + (offset & 0x3FFF), 2);
    return value;
}

u16 palette16(u32 offset) {
    u16 value;
    std::memcpy(&value, &g_machine.palette[offset & 0x7FF], 2);
    return value;
}

u16 ext16(const u8* table, u32 index) {
    if (table == nullptr) return 0;
    u16 value;
    std::memcpy(&value, table + (index & 0xFFF) * 2, 2);
    return value;
}

// Everything drawn for one line before the layers are merged.
struct Line {
    u16 bg[4][kScreenWidth];   // 15-bit colour | kOpaque
    u16 obj[kScreenWidth];
    u8 obj_priority[kScreenWidth];
    u8 obj_alpha[kScreenWidth];   // 0 normal, 0x80 semi-transparent, 0xC0 | alpha for bitmaps
    bool obj_window[kScreenWidth];
    u8 bg0_alpha[kScreenWidth];   // from the 3D picture: 1 to 31; 0 when background 0 is not 3D
    u32 bg0_rgb[kScreenWidth];    // its colour, keeping the 3D picture's 6 bits per channel
    u8 bg0_x[kScreenWidth];       // the column of the 3D picture it came from
};
Line g_line;

u32 char_base(const Engine& e, u16 control, u32 dispcnt) {
    u32 base = ((control >> 2) & 15) * 0x4000;
    if (e.index == 0) base += ((dispcnt >> 24) & 7) * 0x10000;
    return e.bg_vram + base;
}

u32 map_base(const Engine& e, u16 control, u32 dispcnt) {
    u32 base = ((control >> 8) & 31) * 0x800;
    if (e.index == 0) base += ((dispcnt >> 27) & 7) * 0x10000;
    return e.bg_vram + base;
}

const u8* ext_table(const Engine& e, int bg, u16 control, u32 dispcnt) {
    if (!(dispcnt & (1u << 30))) return nullptr;
    int slot = bg;
    if (bg < 2 && (control & 0x2000)) slot = bg + 2;
    return vram_bg_ext_palette(e.index, slot);
}

void apply_bg_mosaic(const Engine& e, u16 control, u16* out) {
    if (!(control & 0x40)) return;
    const u32 size = (reg16(e, 0x4C) & 15) + 1;
    if (size == 1) return;
    for (u32 x = 0; x < kScreenWidth; ++x) out[x] = out[x - x % size];
}

u32 mosaic_line(const Engine& e, u16 control, u32 line) {
    if (!(control & 0x40)) return line;
    const u32 size = ((reg16(e, 0x4C) >> 4) & 15) + 1;
    return line - line % size;
}

void draw_text_bg(const Engine& e, int bg, u32 line, u32 dispcnt, u16* out) {
    const u16 control = reg16(e, 0x08 + bg * 2);
    const u32 scroll_x = reg16(e, 0x10 + bg * 4) & 0x1FF;
    const u32 scroll_y = reg16(e, 0x12 + bg * 4) & 0x1FF;
    const u32 size = (control >> 14) & 3;
    const u32 width = (size & 1) ? 512 : 256;
    const u32 height = (size & 2) ? 512 : 256;
    const u32 y = (mosaic_line(e, control, line) + scroll_y) & (height - 1);
    const bool colors256 = control & 0x80;
    const u32 chars = char_base(e, control, dispcnt);
    u32 map = map_base(e, control, dispcnt);
    if (y >= 256 && height == 512) map += (width == 512) ? 0x1000 : 0x800;
    const u32 row = (y & 0xFF) / 8;
    const u8* ext = colors256 ? ext_table(e, bg, control, dispcnt) : nullptr;
    for (u32 sx = 0; sx < kScreenWidth; ++sx) {
        const u32 x = (sx + scroll_x) & (width - 1);
        u32 block = map;
        if (x >= 256) block += 0x800;
        const u16 entry = vram16(block + (row * 32 + (x & 0xFF) / 8) * 2);
        const u32 px = (entry & 0x400) ? 7 - (x & 7) : (x & 7);
        const u32 py = (entry & 0x800) ? 7 - (y & 7) : (y & 7);
        const u32 tile = entry & 0x3FF;
        const u32 pal = entry >> 12;
        if (colors256) {
            const u8 index = vram8(chars + tile * 64 + py * 8 + px);
            if (index == 0) continue;
            const u16 color = ext ? ext16(ext, (pal << 8) | index) : palette16(e.palette + index * 2);
            out[sx] = color | kOpaque;
        } else {
            const u8 byte = vram8(chars + tile * 32 + py * 4 + px / 2);
            const u8 index = (px & 1) ? byte >> 4 : byte & 15;
            if (index == 0) continue;
            out[sx] = palette16(e.palette + (pal * 16 + index) * 2) | kOpaque;
        }
    }
    apply_bg_mosaic(e, control, out);
}

// Moves the internal reference point of background 2 or 3 to this line.
void update_reference(Engine& e, int which, u32 line) {
    const u32 regs = 0x20 + which * 0x10;
    const u32 raw_x = reg32(e, regs + 8), raw_y = reg32(e, regs + 12);
    if (line == 0 || !e.seen_valid[which] || raw_x != e.seen_x[which] || raw_y != e.seen_y[which]) {
        e.ref_x[which] = s32(raw_x << 4) >> 4;
        e.ref_y[which] = s32(raw_y << 4) >> 4;
        e.seen_x[which] = raw_x;
        e.seen_y[which] = raw_y;
        e.seen_valid[which] = true;
    }
}

template <typename Sample>
void draw_affine(const Engine& e, int which, s32 width, s32 height, bool wrap, u16 control,
                 u16* out, Sample sample) {
    const u32 regs = 0x20 + which * 0x10;
    const s32 pa = s16(reg16(e, regs)), pc = s16(reg16(e, regs + 4));
    s32 x = e.ref_x[which], y = e.ref_y[which];
    for (u32 sx = 0; sx < kScreenWidth; ++sx, x += pa, y += pc) {
        s32 tx = x >> 8, ty = y >> 8;
        if (wrap) {
            tx &= width - 1;
            ty &= height - 1;
        } else if (tx < 0 || ty < 0 || tx >= width || ty >= height) {
            continue;
        }
        const u32 color = sample(u32(tx), u32(ty));
        if (color & kOpaque) out[sx] = u16(color);
    }
    apply_bg_mosaic(e, control, out);
}

void draw_rotation_bg(Engine& e, int bg, u32 dispcnt, u16* out) {
    const u16 control = reg16(e, 0x08 + bg * 2);
    const s32 size = 128 << ((control >> 14) & 3);
    const u32 chars = char_base(e, control, dispcnt);
    const u32 map = map_base(e, control, dispcnt);
    draw_affine(e, bg - 2, size, size, control & 0x2000, control, out, [&](u32 x, u32 y) -> u32 {
        const u32 tile = vram8(map + (y / 8) * (size / 8) + x / 8);
        const u8 index = vram8(chars + tile * 64 + (y & 7) * 8 + (x & 7));
        return index ? (palette16(e.palette + index * 2) | kOpaque) : 0;
    });
}

void draw_extended_bg(Engine& e, int bg, u32 dispcnt, u16* out) {
    const u16 control = reg16(e, 0x08 + bg * 2);
    const u32 bits = (control >> 14) & 3;
    const bool wrap = control & 0x2000;
    if (control & 0x80) {  // bitmap
        static constexpr s32 kWidth[4] = {128, 256, 512, 512};
        static constexpr s32 kHeight[4] = {128, 256, 256, 512};
        const s32 width = kWidth[bits];
        const u32 base = e.bg_vram + ((control >> 8) & 31) * 0x4000;
        if (control & 4) {
            draw_affine(e, bg - 2, width, kHeight[bits], wrap, control, out, [&](u32 x, u32 y) -> u32 {
                const u16 color = vram16(base + (y * u32(width) + x) * 2);
                return (color & 0x8000) ? color : 0;
            });
        } else {
            draw_affine(e, bg - 2, width, kHeight[bits], wrap, control, out, [&](u32 x, u32 y) -> u32 {
                const u8 index = vram8(base + y * u32(width) + x);
                return index ? (palette16(e.palette + index * 2) | kOpaque) : 0;
            });
        }
        return;
    }
    const s32 size = 128 << bits;  // 16-bit map entries, 256 colours
    const u32 chars = char_base(e, control, dispcnt);
    const u32 map = map_base(e, control, dispcnt);
    const u8* ext = ext_table(e, bg, control, dispcnt);
    draw_affine(e, bg - 2, size, size, wrap, control, out, [&](u32 x, u32 y) -> u32 {
        const u16 entry = vram16(map + ((y / 8) * (size / 8) + x / 8) * 2);
        const u32 px = (entry & 0x400) ? 7 - (x & 7) : (x & 7);
        const u32 py = (entry & 0x800) ? 7 - (y & 7) : (y & 7);
        const u8 index = vram8(chars + (entry & 0x3FF) * 64 + py * 8 + px);
        if (index == 0) return 0;
        const u16 color = ext ? ext16(ext, ((entry >> 12) << 8) | index) : palette16(e.palette + index * 2);
        return color | kOpaque;
    });
}

void draw_large_bg(Engine& e, u32 /*dispcnt*/, u16* out) {
    const u16 control = reg16(e, 0x0C);
    const bool wide = !(control & 0x4000);
    const s32 width = wide ? 1024 : 512, height = wide ? 512 : 1024;
    draw_affine(e, 0, width, height, control & 0x2000, control, out, [&](u32 x, u32 y) -> u32 {
        const u8 index = vram8(e.bg_vram + y * u32(width) + x);
        return index ? (palette16(e.palette + index * 2) | kOpaque) : 0;
    });
}

void draw_sprites(const Engine& e, u32 line, u32 dispcnt) {
    static constexpr u8 kWidths[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
    static constexpr u8 kHeights[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};
    const u8* oam = &g_machine.oam[e.oam];
    const u8* ext = (dispcnt & (1u << 31)) ? vram_obj_ext_palette(e.index) : nullptr;
    const u32 mosaic = reg16(e, 0x4C);
    const u32 mosaic_x = ((mosaic >> 8) & 15) + 1, mosaic_y = ((mosaic >> 12) & 15) + 1;
    for (int i = 127; i >= 0; --i) {
        u16 attr0, attr1, attr2;
        std::memcpy(&attr0, oam + i * 8, 2);
        std::memcpy(&attr1, oam + i * 8 + 2, 2);
        std::memcpy(&attr2, oam + i * 8 + 4, 2);
        const bool affine = attr0 & 0x100;
        if (!affine && (attr0 & 0x200)) continue;  // hidden
        const u32 mode = (attr0 >> 10) & 3;
        const u32 shape = attr0 >> 14;
        if (shape == 3) continue;
        const u32 size = attr1 >> 14;
        const s32 width = kWidths[shape][size], height = kHeights[shape][size];
        const bool doubled = affine && (attr0 & 0x200);
        const s32 box_w = doubled ? width * 2 : width, box_h = doubled ? height * 2 : height;
        s32 y0 = attr0 & 0xFF;
        if (y0 >= 192) y0 -= 256;
        const bool use_mosaic = attr0 & 0x1000;
        const s32 this_line = use_mosaic ? s32(line - line % mosaic_y) : s32(line);
        const s32 row = this_line - y0;
        if (row < 0 || row >= box_h) continue;
        s32 x0 = attr1 & 0x1FF;
        if (x0 >= 256) x0 -= 512;
        const u32 tile = attr2 & 0x3FF;
        const u8 priority = (attr2 >> 10) & 3;
        const u32 pal = attr2 >> 12;
        const bool colors256 = attr0 & 0x2000;

        s32 pa = 256, pb = 0, pc = 0, pd = 256;
        if (affine) {
            const u32 group = (attr1 >> 9) & 31;
            s16 value;
            std::memcpy(&value, oam + group * 32 + 6, 2); pa = value;
            std::memcpy(&value, oam + group * 32 + 14, 2); pb = value;
            std::memcpy(&value, oam + group * 32 + 22, 2); pc = value;
            std::memcpy(&value, oam + group * 32 + 30, 2); pd = value;
        }
        const bool hflip = !affine && (attr1 & 0x1000), vflip = !affine && (attr1 & 0x2000);

        // Where the sprite's pixels start and how far apart its rows are.
        u32 base;
        u32 stride;
        if (mode == 3) {
            if (dispcnt & 0x40) {
                base = tile * (128u << ((dispcnt >> 22) & 1));
                stride = u32(width) * 2;
            } else if (dispcnt & 0x20) {
                base = (tile & 0x1F) * 0x10 + (tile & ~0x1Fu) * 0x80;
                stride = 512;
            } else {
                base = (tile & 0xF) * 0x10 + (tile & ~0xFu) * 0x80;
                stride = 256;
            }
        } else if (dispcnt & 0x10) {
            base = tile * (32u << ((dispcnt >> 20) & 3));
            stride = u32(width) * (colors256 ? 8 : 4) / 8;
        } else {
            base = tile * 32;
            stride = 32 * 32;
        }
        base += e.obj_vram;

        for (s32 column = 0; column < box_w; ++column) {
            const s32 sx = x0 + column;
            if (sx < 0 || sx >= kScreenWidth) continue;
            s32 tx, ty;
            if (affine) {
                const s32 cx = column - box_w / 2, cy = row - box_h / 2;
                tx = ((pa * cx + pb * cy) >> 8) + width / 2;
                ty = ((pc * cx + pd * cy) >> 8) + height / 2;
                if (tx < 0 || ty < 0 || tx >= width || ty >= height) continue;
            } else {
                tx = hflip ? width - 1 - column : column;
                ty = vflip ? height - 1 - row : row;
            }
            if (use_mosaic) tx -= tx % s32(mosaic_x);
            u32 color = 0;
            if (mode == 3) {
                const u16 pixel = vram16(base + u32(ty) * stride + u32(tx) * 2);
                if ((pixel & 0x8000) && pal != 0) color = pixel | kOpaque;
            } else if (colors256) {
                u32 where;
                if (dispcnt & 0x10) {
                    where = base + (u32(ty) / 8) * (stride * 8) + (u32(ty) & 7) * 8 + (u32(tx) / 8) * 64 + (u32(tx) & 7);
                } else {
                    // In 2D mapping each row of tiles is 32 units wide, a 256-colour tile takes two.
                    const u32 start = base & ~31u;
                    where = start + (u32(ty) / 8) * stride + (u32(ty) & 7) * 8 + (u32(tx) / 8) * 64 + (u32(tx) & 7);
                }
                const u8 index = vram8(where);
                if (index) {
                    color = (ext ? ext16(ext, (pal << 8) | index)
                                 : palette16(e.palette + 0x200 + index * 2)) | kOpaque;
                }
            } else {
                u32 where;
                if (dispcnt & 0x10) {
                    where = base + (u32(ty) / 8) * (stride * 8) + (u32(ty) & 7) * 4 + (u32(tx) / 8) * 32 + (u32(tx) & 7) / 2;
                } else {
                    where = base + (u32(ty) / 8) * stride + (u32(ty) & 7) * 4 + (u32(tx) / 8) * 32 + (u32(tx) & 7) / 2;
                }
                const u8 byte = vram8(where);
                const u8 index = (tx & 1) ? byte >> 4 : byte & 15;
                if (index) color = palette16(e.palette + 0x200 + (pal * 16 + index) * 2) | kOpaque;
            }
            if (!(color & kOpaque)) continue;
            if (mode == 2) {
                g_line.obj_window[sx] = true;
                continue;
            }
            // Sprites run from 127 to 0, so a lower number wins among equal priorities.
            if (priority > g_line.obj_priority[sx]) continue;
            g_line.obj[sx] = u16(color);
            g_line.obj_priority[sx] = priority;
            g_line.obj_alpha[sx] = mode == 1 ? 0x80 : mode == 3 ? u8(0xC0 | pal) : 0;
        }
    }
}

// Which layers each pixel may show: bits 0-3 backgrounds, 4 sprites, 5 colour effects.
void build_window_mask(const Engine& e, u32 line, u32 dispcnt, u8* mask) {
    if (!(dispcnt & 0xE000)) {
        std::memset(mask, 0x3F, kScreenWidth);
        return;
    }
    const u16 win_in = reg16(e, 0x48), win_out = reg16(e, 0x4A);
    std::memset(mask, win_out & 0x3F, kScreenWidth);
    if (dispcnt & 0x8000) {
        const u8 inside = (win_out >> 8) & 0x3F;
        for (int x = 0; x < kScreenWidth; ++x) {
            if (g_line.obj_window[x]) mask[x] = inside;
        }
    }
    const auto apply = [&](u32 h, u32 v, u8 inside) {
        const u32 x2 = h & 0xFF, x1 = h >> 8, y2 = v & 0xFF, y1 = v >> 8;
        const bool in_y = y1 <= y2 ? (line >= y1 && line < y2) : (line >= y1 || line < y2);
        if (!in_y) return;
        for (u32 x = 0; x < kScreenWidth; ++x) {
            if (x1 <= x2 ? (x >= x1 && x < x2) : (x >= x1 || x < x2)) mask[x] = inside;
        }
    };
    if (dispcnt & 0x4000) apply(reg16(e, 0x42), reg16(e, 0x46), (win_in >> 8) & 0x3F);
    if (dispcnt & 0x2000) apply(reg16(e, 0x40), reg16(e, 0x44), win_in & 0x3F);
}

// Channels in 6 bits, as the hardware mixes them.
struct Rgb6 { u32 r, g, b; };
Rgb6 to_rgb6(u16 c) { return {(c & 31u) * 2, ((c >> 5) & 31u) * 2, ((c >> 10) & 31u) * 2}; }

Rgb6 mix(Rgb6 top, Rgb6 under, u32 eva, u32 evb) {
    return {std::min(63u, (top.r * eva + under.r * evb + 8) >> 4),
            std::min(63u, (top.g * eva + under.g * evb + 8) >> 4),
            std::min(63u, (top.b * eva + under.b * evb + 8) >> 4)};
}

void merge_layers(const Engine& e, u32 dispcnt, const u8* mask, u8* rgba, u16* map, u8* master_row) {
    const u16 bldcnt = reg16(e, 0x50), bldalpha = reg16(e, 0x52);
    const u32 effect = (bldcnt >> 6) & 3;
    const u32 eva = std::min(16u, bldalpha & 31u), evb = std::min(16u, (bldalpha >> 8) & 31u);
    const u32 evy = std::min(16u, reg16(e, 0x54) & 31u);
    const u16 master = reg16(e, 0x6C);
    const u32 master_mode = master >> 14, master_factor = std::min(16u, master & 31u);
    const u16 backdrop = palette16(e.palette);
    u32 bg_priority[4];
    for (int b = 0; b < 4; ++b) bg_priority[b] = reg16(e, 0x08 + b * 2) & 3;
    *master_row = u8((master_mode << 5) | master_factor);

    for (u32 x = 0; x < kScreenWidth; ++x) {
        constexpr int kBackdrop = 5, kObj = 4;
        int top = kBackdrop, under = kBackdrop;
        u16 top_color = backdrop, under_color = backdrop;
        int found = 0;
        for (u32 p = 0; p < 4 && found < 2; ++p) {
            if ((dispcnt & 0x1000) && g_line.obj_priority[x] == p && (mask[x] & 0x10)) {
                if (found++ == 0) { top = kObj; top_color = g_line.obj[x]; }
                else { under = kObj; under_color = g_line.obj[x]; }
            }
            for (int b = 0; b < 4 && found < 2; ++b) {
                if (bg_priority[b] != p || !(dispcnt & (0x100u << b)) || !(mask[x] & (1u << b))) continue;
                const u16 color = g_line.bg[b][x];
                if (!(color & kOpaque)) continue;
                if (found++ == 0) { top = b; top_color = color; }
                else { under = b; under_color = color; }
            }
        }
        const auto layer_rgb = [&](int layer, u16 color) -> Rgb6 {
            if (layer == 0 && g_line.bg0_alpha[x] != 0) {
                const u32 c = g_line.bg0_rgb[x];
                return {c & 63, (c >> 6) & 63, (c >> 12) & 63};
            }
            return to_rgb6(color);
        };
        Rgb6 out = layer_rgb(top, top_color);
        const bool under_ok = bldcnt & (1u << (8 + under));
        // Shown as the 3D picture alone: the screen may take this pixel from its own picture.
        const bool effect_here = (mask[x] & 0x20) && (bldcnt & 1u) && ((effect == 1 && under_ok) || effect >= 2);
        map[x] = (top == 0 && g_line.bg0_alpha[x] == 31 && !effect_here) ? g_line.bg0_x[x] : u16(0xFFFF);
        const u8 alpha = top == kObj ? g_line.obj_alpha[x] : 0;
        if ((alpha & 0x80) && under_ok) {
            const bool bitmap = alpha & 0x40;
            const u32 a = bitmap ? ((alpha & 15u) + 1) : eva;
            const u32 b = bitmap ? 16 - a : evb;
            out = mix(out, layer_rgb(under, under_color), a, b);
        } else if (top == 0 && g_line.bg0_alpha[x] != 0 && g_line.bg0_alpha[x] < 31 && under_ok) {
            // A translucent 3D pixel blends by its own alpha, whatever the blend mode says.
            const Rgb6 below = layer_rgb(under, under_color);
            const u32 a = g_line.bg0_alpha[x] + 1u, b = 32 - a;
            out = {std::min(63u, (out.r * a + below.r * b) >> 5),
                   std::min(63u, (out.g * a + below.g * b) >> 5),
                   std::min(63u, (out.b * a + below.b * b) >> 5)};
        } else if ((mask[x] & 0x20) && (bldcnt & (1u << top))) {
            if (effect == 1 && under_ok) {
                out = mix(out, layer_rgb(under, under_color), eva, evb);
            } else if (effect == 2) {
                out.r += ((63 - out.r) * evy + 8) >> 4;
                out.g += ((63 - out.g) * evy + 8) >> 4;
                out.b += ((63 - out.b) * evy + 8) >> 4;
            } else if (effect == 3) {
                out.r -= (out.r * evy + 7) >> 4;
                out.g -= (out.g * evy + 7) >> 4;
                out.b -= (out.b * evy + 7) >> 4;
            }
        }
        // The display capture takes the picture before the master brightness.
        const_cast<Engine&>(e).line_out[x] = u16((out.r >> 1) | ((out.g >> 1) << 5) | ((out.b >> 1) << 10) | 0x8000);
        if (master_mode == 1) {
            out.r += ((63 - out.r) * master_factor) >> 4;
            out.g += ((63 - out.g) * master_factor) >> 4;
            out.b += ((63 - out.b) * master_factor) >> 4;
        } else if (master_mode == 2) {
            out.r -= (out.r * master_factor + 15) >> 4;
            out.g -= (out.g * master_factor + 15) >> 4;
            out.b -= (out.b * master_factor + 15) >> 4;
        }
        rgba[x * 4 + 0] = u8((out.r << 2) | (out.r >> 4));
        rgba[x * 4 + 1] = u8((out.g << 2) | (out.g >> 4));
        rgba[x * 4 + 2] = u8((out.b << 2) | (out.b >> 4));
        rgba[x * 4 + 3] = 255;
    }
}

// Where the 3D picture went this line: set by render_engine for engine A, read by ppu_render_line.
bool g_3d_this_line = false;
u32 g_3d_scroll_back = 0, g_3d_scroll_front = 0;
int g_3d_screen_back = -1, g_3d_screen_front = -1;  // 0 top, 1 bottom, -1 no 3D in the frame

void render_engine(Engine& e, u32 line, u8* rgba, u16* map, u8* master_row) {
    std::fill(map, map + kScreenWidth, u16(0xFFFF));
    *master_row = 0;
    const u32 dispcnt = reg32(e, 0);
    const u32 display = (dispcnt >> 16) & 3;
    const bool main_engine = e.index == 0;
    if (display == 0 || (dispcnt & 0x80)) {  // display off or forced blank: white
        std::memset(rgba, 0xFF, kScreenWidth * 4);
        return;
    }
    if (display == 2 && main_engine) {  // shows a VRAM bank directly
        const u32 bank = 0x800000 + ((dispcnt >> 18) & 3) * 0x20000;
        for (u32 x = 0; x < kScreenWidth; ++x) {
            const Rgb6 c = to_rgb6(vram16(bank + (line * kScreenWidth + x) * 2));
            rgba[x * 4 + 0] = u8((c.r << 2) | (c.r >> 4));
            rgba[x * 4 + 1] = u8((c.g << 2) | (c.g >> 4));
            rgba[x * 4 + 2] = u8((c.b << 2) | (c.b >> 4));
            rgba[x * 4 + 3] = 255;
        }
        return;
    }
    std::memset(&g_line, 0, sizeof g_line);
    std::memset(g_line.obj_priority, kNoPriority, sizeof g_line.obj_priority);

    const u32 mode = dispcnt & 7;
    update_reference(e, 0, line);
    update_reference(e, 1, line);
    if (dispcnt & 0x1000 || dispcnt & 0x8000) draw_sprites(e, line, dispcnt);

    const bool bg0_3d = main_engine && (dispcnt & 8);
    if (bg0_3d && (dispcnt & 0x100)) {
        ++g_gaps.lines_with_3d;
        const u32* source = gpu3d_line(line);
        const u32 scroll = reg16(e, 0x10) & 0x1FF;
        g_3d_this_line = true;
        g_3d_scroll_back = scroll;
        for (u32 x = 0; x < kScreenWidth; ++x) {
            const u32 pixel = source[(x + scroll) & 0x1FF & 0xFF];
            if ((x + scroll) & 0x100) continue;  // beyond the 256 pixels of the picture
            const u32 alpha = (pixel >> 18) & 31;
            if (alpha == 0) continue;
            g_line.bg[0][x] = kOpaque;
            g_line.bg0_rgb[x] = pixel & 0x3FFFF;
            g_line.bg0_alpha[x] = u8(alpha);
            g_line.bg0_x[x] = u8((x + scroll) & 0xFF);
        }
    } else if (dispcnt & 0x100) {
        draw_text_bg(e, 0, line, dispcnt, g_line.bg[0]);
    }
    if (dispcnt & 0x200) draw_text_bg(e, 1, line, dispcnt, g_line.bg[1]);
    if (dispcnt & 0x400) {
        if (mode == 0 || mode == 1 || mode == 3) draw_text_bg(e, 2, line, dispcnt, g_line.bg[2]);
        else if (mode == 2 || mode == 4) draw_rotation_bg(e, 2, dispcnt, g_line.bg[2]);
        else if (mode == 5) draw_extended_bg(e, 2, dispcnt, g_line.bg[2]);
        else if (mode == 6) draw_large_bg(e, dispcnt, g_line.bg[2]);
    }
    if (dispcnt & 0x800) {
        if (mode == 0) draw_text_bg(e, 3, line, dispcnt, g_line.bg[3]);
        else if (mode == 1 || mode == 2) draw_rotation_bg(e, 3, dispcnt, g_line.bg[3]);
        else if (mode >= 3 && mode <= 5) draw_extended_bg(e, 3, dispcnt, g_line.bg[3]);
    }

    u8 mask[kScreenWidth];
    build_window_mask(e, line, dispcnt, mask);
    merge_layers(e, dispcnt, mask, rgba, map, master_row);

    // The reference points move on by one line.
    for (int which = 0; which < 2; ++which) {
        const u32 regs = 0x20 + which * 0x10;
        e.ref_x[which] += s16(reg16(e, regs + 2));
        e.ref_y[which] += s16(reg16(e, regs + 6));
    }
}

// Display capture: engine A's line, the 3D picture or a VRAM bank goes into a VRAM bank. The game
// starts one by setting bit 31; it runs from the next frame's first line and then clears the bit.
bool g_capture_running = false;

// For the screen's picture: per pixel of the frame the 3D column it shows alone (0xFFFF: none),
// per line the master brightness (mode << 5 | factor), and which 3D picture the frame used.
u16 g_map_back[kScreenHeight * 2 * kScreenWidth], g_map_front[kScreenHeight * 2 * kScreenWidth];
u8 g_master_back[kScreenHeight * 2], g_master_front[kScreenHeight * 2];
int g_display_back = -1, g_display_front = -1;

void capture_line(u32 line, u32 control) {
    static constexpr u32 kWidths[4] = {128, 256, 256, 256};
    static constexpr u32 kHeights[4] = {128, 64, 128, 192};
    const u32 size = (control >> 20) & 3;
    if (line >= kHeights[size]) return;
    const Engine& e = g_engines[0];
    const u32 eva = std::min(16u, control & 31), evb = std::min(16u, (control >> 8) & 31);
    const u32 source = (control >> 29) & 3;
    const bool from_3d = control & (1u << 24);
    const bool b_from_fifo = control & (1u << 25);
    const u32 bank_write = (control >> 16) & 3, offset_write = (control >> 18) & 3;
    const u32 offset_read = (control >> 26) & 3;
    const u32 dispcnt = reg32(e, 0);
    const u32 bank_read = (dispcnt >> 18) & 3;
    const u32* source_3d = gpu3d_line(line);
    for (u32 x = 0; x < kWidths[size]; ++x) {
        u16 a;
        if (from_3d) {
            const u32 p = source_3d[x];
            const u32 alpha = (p >> 18) & 31;
            a = u16(((p & 63) >> 1) | ((((p >> 6) & 63) >> 1) << 5) | ((((p >> 12) & 63) >> 1) << 10)
                    | (alpha ? 0x8000 : 0));
        } else {
            a = e.line_out[x];
        }
        // The main memory display FIFO is not built; the game's capture code has not needed it.
        u16 b = 0;
        if (!b_from_fifo) {
            const u32 at = bank_read * 0x20000 + ((offset_read * 0x8000 + (line * 256 + x) * 2) & 0x1FFFF);
            std::memcpy(&b, &g_machine.vram[at], 2);
        }
        u16 out;
        if (source == 0) {
            out = a;
        } else if (source == 1) {
            out = b;
        } else {
            const u32 aa = (a >> 15) & 1, ab = (b >> 15) & 1;
            u32 mixed = 0;
            for (int s = 0; s < 15; s += 5) {
                const u32 ca = (a >> s) & 31, cb = (b >> s) & 31;
                mixed |= std::min(31u, (ca * aa * eva + cb * ab * evb + 8) >> 4) << s;
            }
            out = u16(mixed | (((aa && eva) || (ab && evb)) ? 0x8000u : 0u));
        }
        const u32 at = bank_write * 0x20000 + ((offset_write * 0x8000 + (line * 256 + x) * 2) & 0x1FFFF);
        std::memcpy(&g_machine.vram[at], &out, 2);
    }
    ++g_gaps.lines_with_capture;
}

}  // namespace

void ppu_render_line(u32 line) {
    u16 powcnt;
    std::memcpy(&powcnt, &g_machine.io[0x304], 2);
    // With bit 15 set engine A feeds the top screen, otherwise the bottom one.
    const bool a_on_top = powcnt & 0x8000;
    u8* top = g_back + size_t(line) * kScreenWidth * 4;
    u8* bottom = top + size_t(kScreenHeight) * kScreenWidth * 4;
    u16* map_top = g_map_back + size_t(line) * kScreenWidth;
    u16* map_bottom = map_top + size_t(kScreenHeight) * kScreenWidth;
    u8* master_top = g_master_back + line;
    u8* master_bottom = master_top + kScreenHeight;
    u32 capture;
    std::memcpy(&capture, &g_machine.io[0x64], 4);
    if (line == 0) {
        g_capture_running = capture & 0x80000000u;
        g_display_back = gpu3d_display_index();
        g_3d_screen_back = -1;
    }
    g_3d_this_line = false;
    render_engine(g_engines[0], line, a_on_top ? top : bottom, a_on_top ? map_top : map_bottom,
                  a_on_top ? master_top : master_bottom);
    if (g_3d_this_line) g_3d_screen_back = a_on_top ? 0 : 1;
    render_engine(g_engines[1], line, a_on_top ? bottom : top, a_on_top ? map_bottom : map_top,
                  a_on_top ? master_bottom : master_top);
    if (g_capture_running) capture_line(line, capture);
}

void ppu_finish_frame() {
    std::memcpy(g_front, g_back, sizeof g_front);
    std::memcpy(g_map_front, g_map_back, sizeof g_map_front);
    std::memcpy(g_master_front, g_master_back, sizeof g_master_front);
    g_display_front = g_display_back;
    g_3d_screen_front = g_3d_screen_back;
    g_3d_scroll_front = g_3d_scroll_back;
    if (g_capture_running) {
        g_capture_running = false;
        g_machine.io[0x67] &= 0x7F;  // finished: the enable bit clears itself
    }
}

const u8* ppu_frame() { return g_front; }

const u16* ppu_frame_map() { return g_map_front; }

Ppu3dPlacement ppu_3d_placement() { return {g_3d_screen_front, g_3d_scroll_front}; }

bool ppu_3d_layer(std::vector<u8>& out, int& scale) {
    scale = gpu3d_display_scale();
    const u32* picture = gpu3d_display_picture(g_display_front);
    if (picture == nullptr || g_3d_screen_front < 0) return false;
    const int s = scale;
    const size_t w = size_t(kScreenWidth) * size_t(s), h = size_t(kScreenHeight) * size_t(s);
    out.resize(w * h * 4);
    const auto rows = [&](size_t first, size_t last) {
        for (size_t y = first; y < last; ++y) {
            const u32 master = g_master_front[size_t(g_3d_screen_front) * kScreenHeight + y / size_t(s)];
            const u32 mode = master >> 5, factor = master & 31;
            const u32* in = picture + y * w;
            u8* o = &out[y * w * 4];
            for (size_t x = 0; x < w; ++x, o += 4) {
                const u32 p = in[x];
                if (((p >> 18) & 31) == 0) {
                    o[0] = o[1] = o[2] = o[3] = 0;
                    continue;
                }
                u32 c[3] = {p & 63, (p >> 6) & 63, (p >> 12) & 63};
                for (u32& v : c) {
                    if (mode == 1) v += ((63 - v) * factor) >> 4;
                    else if (mode == 2) v -= (v * factor + 15) >> 4;
                }
                o[0] = u8((c[0] << 2) | (c[0] >> 4));
                o[1] = u8((c[1] << 2) | (c[1] >> 4));
                o[2] = u8((c[2] << 2) | (c[2] >> 4));
                o[3] = 255;
            }
        }
    };
    const int threads = std::max(1, std::min(8, int(std::thread::hardware_concurrency())));
    std::vector<std::thread> workers;
    const size_t step = (h + size_t(threads) - 1) / size_t(threads);
    for (size_t first = step; first < h; first += step) workers.emplace_back(rows, first, std::min(h, first + step));
    rows(0, std::min(h, step));
    for (std::thread& t : workers) t.join();
    return true;
}

bool ppu_compose_display(std::vector<u8>& out, int& scale, const u8* upscaled) {
    scale = gpu3d_display_scale();
    const u32* picture = gpu3d_display_picture(g_display_front);
    const int s = scale;
    const size_t out_w = size_t(kScreenWidth) * size_t(s);
    out.resize(out_w * size_t(kScreenHeight * 2) * size_t(s) * 4);
    const auto rows = [&](int first, int last) {
    for (int y = first; y < last; ++y) {
        const u32 master = g_master_front[y];
        const u32 master_mode = master >> 5, factor = master & 31;
        const int line3d = y % kScreenHeight;
        for (int x = 0; x < kScreenWidth; ++x) {
            const u8* native = g_front + (size_t(y) * kScreenWidth + size_t(x)) * 4;
            const u16 column = g_map_front[size_t(y) * kScreenWidth + size_t(x)];
            for (int j = 0; j < s; ++j) {
                u8* row = &out[((size_t(y) * size_t(s) + size_t(j)) * out_w + size_t(x) * size_t(s)) * 4];
                for (int i = 0; i < s; ++i) {
                    u8* px = row + size_t(i) * 4;
                    u32 p = 0;
                    if (column != 0xFFFF && picture != nullptr) {
                        p = picture[(size_t(line3d) * size_t(s) + size_t(j)) * out_w + size_t(column) * size_t(s) + size_t(i)];
                    }
                    if (((p >> 18) & 31) == 0) {  // nothing of the 3D picture here: the game's pixel,
                        // made larger by the upscaler when there is one
                        if (upscaled != nullptr) std::memcpy(px, upscaled + (px - out.data()), 4);
                        else std::memcpy(px, native, 4);
                        continue;
                    }
                    u32 c[3] = {p & 63, (p >> 6) & 63, (p >> 12) & 63};
                    for (u32& v : c) {
                        if (master_mode == 1) v += ((63 - v) * factor) >> 4;
                        else if (master_mode == 2) v -= (v * factor + 15) >> 4;
                    }
                    px[0] = u8((c[0] << 2) | (c[0] >> 4));
                    px[1] = u8((c[1] << 2) | (c[1] >> 4));
                    px[2] = u8((c[2] << 2) | (c[2] >> 4));
                    px[3] = 255;
                }
            }
        }
    }
    };
    // In bands of rows on several cores; each band writes only its own rows.
    const int threads = s > 1 ? std::max(1, std::min(8, int(std::thread::hardware_concurrency()))) : 1;
    std::vector<std::thread> workers;
    const int step = (kScreenHeight * 2 + threads - 1) / threads;
    for (int first = step; first < kScreenHeight * 2; first += step) {
        workers.emplace_back(rows, first, std::min(kScreenHeight * 2, first + step));
    }
    rows(0, std::min(kScreenHeight * 2, step));
    for (std::thread& t : workers) t.join();
    return picture != nullptr;
}

const PpuGaps& ppu_gaps() { return g_gaps; }
