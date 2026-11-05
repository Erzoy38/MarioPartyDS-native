#include "memory.h"

#include "hw.h"

#include <cstring>

Machine g_machine;

void memory_init() {
    g_machine.main_ram.assign(4 * 1024 * 1024, 0);
    g_machine.itcm.assign(32 * 1024, 0);
    g_machine.dtcm.assign(16 * 1024, 0);
    g_machine.palette.assign(2 * 1024, 0);
    g_machine.oam.assign(2 * 1024, 0);
    g_machine.vram.assign(656 * 1024, 0);
    g_machine.io.assign(64 * 1024, 0);
}

namespace {

// VRAM: nine banks in one vector, in the order of the LCDC area, each mapped by its control
// register. One 16 KiB page per entry. Where banks overlap, real hardware merges them when
// reading; here the later bank wins.
constexpr u32 kPageShift = 14, kPageSize = 1u << kPageShift;
constexpr u32 kVramSpan = 0xA00000;
u8* g_vram_page[kVramSpan >> kPageShift];
const u8* g_bg_ext[2][4];   // per engine and slot, 8 KiB each
const u8* g_obj_ext[2];     // per engine, 8 KiB

struct Bank { u32 offset, size; };
constexpr Bank kBanks[9] = {{0x00000, 0x20000}, {0x20000, 0x20000}, {0x40000, 0x20000},
                            {0x60000, 0x20000}, {0x80000, 0x10000}, {0x90000, 0x4000},
                            {0x94000, 0x4000},  {0x98000, 0x8000},  {0xA0000, 0x4000}};

void map_bank(u32 where, const Bank& bank) {
    for (u32 i = 0; i < bank.size; i += kPageSize) {
        g_vram_page[(where + i) >> kPageShift] = &g_machine.vram[bank.offset + i];
    }
}

void mirror_area(u32 start, u32 area, u32 span) {
    for (u32 i = span; i < area; i += kPageSize) {
        g_vram_page[(start + i) >> kPageShift] = g_vram_page[(start + (i % span)) >> kPageShift];
    }
}

// Returns storage for an address, or nullptr for what has no plain storage.
inline u8* locate(u32 addr) {
    Machine& m = g_machine;
    if (addr - m.dtcm_base < 0x4000u) return &m.dtcm[addr - m.dtcm_base];
    switch (addr >> 24) {
        case 0x00:
        case 0x01: return &m.itcm[addr & 0x7FFF];
        case 0x02: return &m.main_ram[addr & 0x3FFFFF];
        case 0x04:
            if (addr < 0x04010000u) return &m.io[addr & 0xFFFF];
            return nullptr;
        case 0x05: return &m.palette[addr & 0x7FF];
        case 0x06: {
            const u32 offset = addr & 0xFFFFFF;
            if (offset >= kVramSpan) return nullptr;
            u8* page = g_vram_page[offset >> kPageShift];
            return page ? page + (offset & (kPageSize - 1)) : nullptr;
        }
        case 0x07: return &m.oam[addr & 0x7FF];
        default: return nullptr;
    }
}

inline bool special_read(u32 addr, u32& value) {
    if ((addr >> 24) == 0x04) return hw_read(addr, 4, value);
    return false;
}

}  // namespace

void vram_remap() {
    Machine& m = g_machine;
    std::memset(g_vram_page, 0, sizeof g_vram_page);
    std::memset(g_bg_ext, 0, sizeof g_bg_ext);
    std::memset(g_obj_ext, 0, sizeof g_obj_ext);
    constexpr u32 kBgA = 0x000000, kBgB = 0x200000, kObjA = 0x400000, kObjB = 0x600000;
    constexpr u32 kLcdc = 0x800000;
    constexpr u8 kControlOffset[9] = {0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x48, 0x49};
    for (int b = 0; b < 9; ++b) {
        const u8 control = m.io[0x200 + kControlOffset[b]];
        if (!(control & 0x80)) continue;
        const Bank& bank = kBanks[b];
        const u32 mst = control & 7;
        const u32 ofs = (control >> 3) & 3;
        const u8* base = &m.vram[bank.offset];
        if (mst == 0) {
            map_bank(kLcdc + bank.offset, bank);
        } else if (b <= 3) {  // A, B, C, D
            if (mst == 1) map_bank(kBgA + ofs * 0x20000, bank);
            else if (mst == 2 && b <= 1) map_bank(kObjA + (ofs & 1) * 0x20000, bank);
            else if (mst == 4 && b == 2) map_bank(kBgB, bank);
            else if (mst == 4 && b == 3) map_bank(kObjB, bank);
        } else if (b == 4) {  // E
            if (mst == 1) map_bank(kBgA, bank);
            else if (mst == 2) map_bank(kObjA, bank);
            else if (mst == 4) for (int s = 0; s < 4; ++s) g_bg_ext[0][s] = base + s * 0x2000;
        } else if (b <= 6) {  // F, G
            const u32 step = (ofs & 1) * 0x4000 + (ofs >> 1) * 0x10000;
            if (mst == 1) map_bank(kBgA + step, bank);
            else if (mst == 2) map_bank(kObjA + step, bank);
            else if (mst == 4) for (int s = 0; s < 2; ++s) g_bg_ext[0][(ofs & 1) * 2 + s] = base + s * 0x2000;
            else if (mst == 5) g_obj_ext[0] = base;
        } else if (b == 7) {  // H
            if (mst == 1) map_bank(kBgB, bank);
            else if (mst == 2) for (int s = 0; s < 4; ++s) g_bg_ext[1][s] = base + s * 0x2000;
        } else {  // I
            if (mst == 1) map_bank(kBgB + 0x8000, bank);
            else if (mst == 2) map_bank(kObjB, bank);
            else if (mst == 3) g_obj_ext[1] = base;
        }
    }
    mirror_area(kBgA, 0x200000, 0x80000);
    mirror_area(kBgB, 0x200000, 0x20000);
    mirror_area(kObjA, 0x200000, 0x40000);
    mirror_area(kObjB, 0x200000, 0x20000);
}

const u8* vram_page(u32 offset) {
    return offset < kVramSpan ? g_vram_page[offset >> kPageShift] : nullptr;
}
const u8* vram_bg_ext_palette(int engine, int slot) { return g_bg_ext[engine][slot]; }
const u8* vram_obj_ext_palette(int engine) { return g_obj_ext[engine]; }

u8 rd8(u32 addr) {
    u32 value;
    if (special_read(addr, value)) return u8(value);
    if (const u8* p = locate(addr)) return *p;
    ++g_machine.unknown_reads;
    return 0;
}

u16 rd16(u32 addr) {
    addr &= ~1u;
    u32 value;
    if (special_read(addr, value)) return u16(value);
    if (const u8* p = locate(addr)) {
        u16 out;
        std::memcpy(&out, p, 2);
        return out;
    }
    ++g_machine.unknown_reads;
    return 0;
}

u32 rd32(u32 addr) {
    addr &= ~3u;
    u32 value;
    if (special_read(addr, value)) return value;
    if (const u8* p = locate(addr)) {
        u32 out;
        std::memcpy(&out, p, 4);
        return out;
    }
    ++g_machine.unknown_reads;
    return 0;
}

void wr8(u32 addr, u8 value) {
    if ((addr >> 24) == 0x04 && hw_write(addr, 1, value)) return;
    if (u8* p = locate(addr)) { *p = value; return; }
    ++g_machine.unknown_writes;
}

void wr16(u32 addr, u16 value) {
    addr &= ~1u;
    if ((addr >> 24) == 0x04 && hw_write(addr, 2, value)) return;
    if (u8* p = locate(addr)) { std::memcpy(p, &value, 2); return; }
    ++g_machine.unknown_writes;
}

void wr32(u32 addr, u32 value) {
    addr &= ~3u;
    if ((addr >> 24) == 0x04 && hw_write(addr, 4, value)) return;
    if (u8* p = locate(addr)) { std::memcpy(p, &value, 4); return; }
    ++g_machine.unknown_writes;
}
