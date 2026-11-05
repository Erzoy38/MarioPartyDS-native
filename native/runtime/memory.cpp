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
        case 0x06: return &m.vram[addr % 0xA4000];
        case 0x07: return &m.oam[addr & 0x7FF];
        default: return nullptr;
    }
}

inline bool special_read(u32 addr, u32& value) {
    if ((addr >> 24) == 0x04) return hw_read(addr, 4, value);
    return false;
}

}  // namespace

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
