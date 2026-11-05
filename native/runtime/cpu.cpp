#include "cpu.h"

namespace {

int bank_index(u32 mode) {
    switch (mode) {
        case kFiq: return 1;
        case kIrq: return 2;
        case kSvc: return 3;
        case kAbt: return 4;
        case kUnd: return 5;
        default: return 0;  // usr and sys share a bank
    }
}

}  // namespace

u32 Cpu::cpsr() const {
    return (n << 31) | (z << 30) | (cf << 29) | (v << 28) | (q << 27) | (irq_off << 7) |
           (fiq_off << 6) | (thumb << 5) | mode;
}

void Cpu::switch_mode(u32 new_mode) {
    if (new_mode == mode) return;
    const int from = bank_index(mode);
    const int to = bank_index(new_mode);
    if (from != to) {
        bank_r13[from] = r[13];
        bank_r14[from] = r[14];
        bank_spsr[from] = spsr;
        if (mode == kFiq) {
            for (int i = 0; i < 5; ++i) { fiq_r8_12[i] = r[8 + i]; r[8 + i] = usr_r8_12[i]; }
        } else if (new_mode == kFiq) {
            for (int i = 0; i < 5; ++i) { usr_r8_12[i] = r[8 + i]; r[8 + i] = fiq_r8_12[i]; }
        }
        r[13] = bank_r13[to];
        r[14] = bank_r14[to];
        spsr = bank_spsr[to];
    }
    mode = new_mode;
}

void Cpu::set_cpsr(u32 value, u32 mask) {
    if (mask & 0xFF000000u) {
        n = (value >> 31) & 1;
        z = (value >> 30) & 1;
        cf = (value >> 29) & 1;
        v = (value >> 28) & 1;
        q = (value >> 27) & 1;
    }
    if (mask & 0xFF) {
        irq_off = (value >> 7) & 1;
        fiq_off = (value >> 6) & 1;
        thumb = (value >> 5) & 1;
        switch_mode(value & 0x1F);
    }
}

void Cpu::exception_return(u32 target) {
    set_cpsr(spsr, 0xFFFFFFFFu);
    pc = target & (thumb ? ~1u : ~3u);
}

u32 Cpu::user_reg(int index) const {
    if (index >= 13 && index <= 14 && bank_index(mode) != 0) {
        return index == 13 ? bank_r13[0] : bank_r14[0];
    }
    if (index >= 8 && index <= 12 && mode == kFiq) return usr_r8_12[index - 8];
    return r[index];
}

void Cpu::set_user_reg(int index, u32 value) {
    if (index >= 13 && index <= 14 && bank_index(mode) != 0) {
        (index == 13 ? bank_r13[0] : bank_r14[0]) = value;
    } else if (index >= 8 && index <= 12 && mode == kFiq) {
        usr_r8_12[index - 8] = value;
    } else {
        r[index] = value;
    }
}
