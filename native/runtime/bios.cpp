// The DS BIOS calls the game uses, as native code. Behaviour follows GBATEK's description;
// each function was written for this project and copies no BIOS code.
#include "hw.h"

#include "memory.h"

#include <cstdio>

namespace {

void cpu_set(Cpu& c) {
    u32 src = c.r[0], dst = c.r[1];
    const u32 control = c.r[2];
    u32 count = control & 0x1FFFFF;
    const bool fill = control >> 24 & 1;
    const bool words = control >> 26 & 1;
    if (words) {
        src &= ~3u;
        dst &= ~3u;
        const u32 value = rd32(src);
        for (; count; --count, dst += 4) {
            wr32(dst, fill ? value : rd32(src));
            if (!fill) src += 4;
        }
    } else {
        src &= ~1u;
        dst &= ~1u;
        const u16 value = rd16(src);
        for (; count; --count, dst += 2) {
            wr16(dst, fill ? value : rd16(src));
            if (!fill) src += 2;
        }
    }
}

void cpu_fast_set(Cpu& c) {
    u32 src = c.r[0] & ~3u, dst = c.r[1] & ~3u;
    const u32 control = c.r[2];
    u32 count = (control & 0x1FFFFF);
    count = (count + 7) & ~7u;
    const bool fill = control >> 24 & 1;
    const u32 value = rd32(src);
    for (; count; --count, dst += 4) {
        wr32(dst, fill ? value : rd32(src));
        if (!fill) src += 4;
    }
}

void lz77_wram(Cpu& c) {
    u32 src = c.r[0], dst = c.r[1];
    const u32 header = rd32(src & ~3u);
    src += 4;
    u32 remaining = header >> 8;
    while (remaining) {
        u32 flags = rd8(src++);
        for (int bit = 0; bit < 8 && remaining; ++bit, flags <<= 1) {
            if (flags & 0x80) {
                const u32 b1 = rd8(src++), b2 = rd8(src++);
                u32 length = (b1 >> 4) + 3;
                const u32 distance = (((b1 & 15) << 8) | b2) + 1;
                while (length-- && remaining) {
                    wr8(dst, rd8(dst - distance));
                    ++dst;
                    --remaining;
                }
            } else {
                wr8(dst++, rd8(src++));
                --remaining;
            }
        }
    }
}

u32 integer_sqrt(u32 value) {
    u32 root = 0, bit = 1u << 30;
    while (bit > value) bit >>= 2;
    while (bit) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root;
}

}  // namespace

void bios_swi(Cpu& c, u32 number) {
    switch (number) {
        case 0x03: return;  // WaitByLoop: timing is not modelled
        case 0x04:          // IntrWait
        case 0x05:          // VBlankIntrWait
            // The SDK replaces these with its own thread wait; halt until an interrupt.
            g_hw.halted = true;
            return;
        case 0x06: g_hw.halted = true; return;  // Halt
        case 0x09: {  // Div
            const s32 numerator = s32(c.r[0]), denominator = s32(c.r[1]);
            if (denominator == 0) return;
            const s32 quotient = numerator / denominator;
            const s32 remainder = numerator % denominator;
            c.r[0] = u32(quotient);
            c.r[1] = u32(remainder);
            c.r[3] = u32(quotient < 0 ? -quotient : quotient);
            return;
        }
        case 0x0B: cpu_set(c); return;
        case 0x0C: cpu_fast_set(c); return;
        case 0x0D: c.r[0] = integer_sqrt(c.r[0]); return;
        case 0x11: lz77_wram(c); return;
        default:
            std::printf("unhandled software interrupt %#x\n", number);
            runtime_abort("software interrupt", c.pc);
    }
}
