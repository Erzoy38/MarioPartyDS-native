#define _CRT_SECURE_NO_WARNINGS  // getenv, only for a diagnostic switch
#include "hw.h"

#include "gpu3d.h"
#include "memory.h"
#include "ppu.h"
#include "sound.h"

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <algorithm>
#include <cstring>
#include <deque>
#include <iterator>
#include <map>
#include <vector>

HwState g_hw;

namespace {

constexpr u32 kIrqEntry = 0xFFFF0018;
constexpr u32 kIrqReturn = 0xFFFF0028;

u8 g_ipc_out = 0, g_ipc_recv = 0;
bool g_ipc_sync_irq_on = false, g_fifo_send_irq_on = false, g_fifo_recv_irq_on = false, g_fifo_on = false;
std::deque<u32> g_fifo_to_arm9;
u32 g_fifo_last = 0;
std::map<u32, u32> g_tag_counts;
u32 g_keys_pressed = 0;

// Stand-in for the ARM7 side of the game's message channel. A word is tag (bits 0-4), flag
// (bit 5: the receiver's third callback argument, see 0x020675CC) and data (bits 6-31).
// Unknown messages are logged, not guessed at.
void arm7_reply(u32 tag, u32 data, bool flag = false) {
    g_fifo_to_arm9.push_back(tag | (flag ? 0x20u : 0u) | (data << 6));
    if (g_fifo_recv_irq_on) hw_raise(irq::kFifoRecv);
}

// The clock is fixed, not read from the PC: every player's game must see the same time.
// Date and time use the layout the ARM7 writes to the shared area (BCD).
void arm7_rtc(u32 data) {
    constexpr u32 kDateTimeArea = 0x027FFDE8;
    const u32 command = (data >> 8) & 0x7F;
    wr32(kDateTimeArea, 0x00010106);       // 2006-01-01, a Sunday; weekday in the top byte
    wr32(kDateTimeArea + 4, 0x00000012);   // 12:00:00
    arm7_reply(5, command << 8);           // result 0: success
}

// The save memory on the card, as the ARM7 card thread serves it (arm7.bin file offset 0x8340).
// The game fills a shared block and sends a command; the block's first word is the result.
// Files are the caller's business: see hw_set_save and hw_save.
namespace backup {
constexpr u32 kResultOk = 0, kResultVerifyFailed = 1, kResultBadAlignment = 2, kResultUnsupported = 3;
constexpr u32 kResult = 0x00, kCardId = 0x08, kRamOrSave = 0x0C, kSaveOrRam = 0x10, kLength = 0x14;
constexpr u32 kTotalSize = 0x18, kSectorSize = 0x1C, kAllowedCommands = 0x4C;

u32 g_block = 0;          // address of the shared block
bool g_wait_block = false;
std::vector<u8> g_memory;
bool g_changed = false;

u8& at(u32 address) {
    const u32 total = rd32(g_block + kTotalSize);
    const u32 size = total ? total : 1;
    if (g_memory.size() != size) g_memory.resize(size, 0xFF);  // erased beyond any loaded save
    return g_memory[address % size];
}

void run(u32 command) {
    wr32(g_block + kResult, kResultOk);
    if (!(rd32(g_block + kAllowedCommands) & (1u << command))) {
        wr32(g_block + kResult, kResultUnsupported);
        return;
    }
    const u32 a = rd32(g_block + kRamOrSave);
    const u32 b = rd32(g_block + kSaveOrRam);
    const u32 length = rd32(g_block + kLength);
    switch (command) {
        case 0:    // start
        case 1:    // acknowledge
        case 2:    // identify: the ARM7 only sets up its chip access here
            break;
        case 3:    // card id
            wr32(g_block + kCardId, 0x00001FC2);
            break;
        case 6:    // read: save address a to RAM address b
            for (u32 i = 0; i < length; ++i) wr8(b + i, at(a + i));
            break;
        case 7:    // write: RAM address a to save address b
        case 8:    // program
            for (u32 i = 0; i < length; ++i) at(b + i) = rd8(a + i);
            g_changed = true;
            break;
        case 9: {  // verify
            for (u32 i = 0; i < length; ++i) {
                if (at(b + i) != rd8(a + i)) {
                    wr32(g_block + kResult, kResultVerifyFailed);
                    break;
                }
            }
            break;
        }
        case 11: { // erase sectors
            const u32 sector = rd32(g_block + kSectorSize);
            if ((b | length) & (sector - 1)) {
                wr32(g_block + kResult, kResultBadAlignment);
                break;
            }
            for (u32 i = 0; i < length; ++i) at(b + i) = 0xFF;
            g_changed = true;
            break;
        }
        case 12:   // erase the chip
            at(0);
            g_memory.assign(g_memory.size(), 0xFF);
            g_changed = true;
            break;
        default:
            wr32(g_block + kResult, kResultUnsupported);
            break;
    }
}

void receive(u32 data) {
    u32 command = data;
    if (g_wait_block) {
        g_wait_block = false;
        g_block = data;
        command = 0;
    } else if (data == 0) {
        g_wait_block = true;   // the start command is followed by the block's address
        return;
    } else if (g_block == 0) {
        runtime_abort("save command before the start command", data);
    }
    run(command);
    arm7_reply(11, 1, true);   // arm7.bin 0x85B4: tag 11, data 1, flag set
}
}  // namespace backup

// Power and sound-chip settings. A request spans several words; bits 24-25 count down to 1 on
// the last. The reply is the command with a result byte (see the callback at 0x0206B34C).
// The settings change nothing here, so each one is accepted and logged.
u32 g_power_first_word = 0;   // the request being received (hw_reset clears it)
bool g_power_in_request = false;

void arm7_power(u32 data) {
    u32& first_word = g_power_first_word;
    bool& in_request = g_power_in_request;
    const u32 words_left = (data >> 24) & 3;
    if (!in_request) {
        first_word = data;
        in_request = true;
    }
    if (words_left > 1) return;
    in_request = false;
    if (++g_tag_counts[8] <= 12) {
        std::printf("power request %#x, last word %#x: accepted\n", first_word, data);
    }
    arm7_reply(8, first_word & 0x7F00);  // result 0
}

// Touch screen. The game asks for one sample (request at 0x0206A8E4); the ARM7 leaves it at
// 0x027FFFAA and replies with bit 24 set, command 0, result 0 (callback at 0x0206A430).
// Word layout: x bits 0-11, y bits 12-23, touched bit 24, validity bits 25-26. Raw x and y are
// pixels times 16, which is what the calibration data in the boot area says. With no touch the
// word is 0x06000000 (measured in melonDS).
u32 g_touch_word = 0x06000000;

void arm7_touch(u32 data) {
    constexpr u32 kSampleRequest = 0x03000000;
    constexpr u32 kTouchArea = 0x027FFFAA;
    if (data != kSampleRequest) {
        if (++g_tag_counts[6] <= 12) std::printf("touch request %#x: not handled\n", data);
        return;
    }
    wr16(kTouchArea, u16(g_touch_word));
    wr16(kTouchArea + 2, u16(g_touch_word >> 16));
    arm7_reply(6, 0x01000000);
}

// Microphone. A request spans several words: the first holds command and format, the next ones a
// running number in bits 16-19 and 16 bits of a value, and bit 24 marks the last (see 0x0206B090).
// The reply is the command with a result byte (callback at 0x0206AF68). The buffer fills as the
// ARM7 would fill it. The input is only "blowing or not", so it can travel with the other keys.
namespace mic {
bool g_running = false, g_loop = false;
float g_level = 0;  // 0 silent to 1 as loud as the microphone goes
u32 g_type = 0, g_buffer = 0, g_size = 0, g_position = 0, g_timer = 1, g_rest = 0, g_counter = 0;
u32 g_words[8];
u32 g_command = 0;
constexpr u32 kLastAddress = 0x027FFF90, kOneSample = 0x027FFF94;
constexpr u32 kBusCyclesPerLine = 2130;  // 33.51 MHz / 15734 lines

// A wave around the middle whose height is the level: quiet sound swings a little, loud sound fully.
void write_sample(u32 at) {
    ++g_counter;
    const bool sixteen = g_type & 1;
    const bool is_signed = (g_type & 2) != 0;
    const s32 full = sixteen ? 0x7FF0 : 0x7F;
    const s32 swing = s32(float(full) * std::clamp(g_level, 0.0f, 1.0f));
    const s32 sample = (g_counter & 1) ? swing : -swing;  // signed, around 0
    u32 value;
    if (is_signed) value = u32(sample) & (sixteen ? 0xFFFFu : 0xFFu);
    else value = u32(sample + (sixteen ? 0x8000 : 0x80));
    if (sixteen) wr16(at, u16(value));
    else wr8(at, u8(value));
}

void line() {
    if (!g_running) return;
    g_rest += kBusCyclesPerLine;
    const u32 bytes = (g_type & 1) ? 2 : 1;
    while (g_rest >= g_timer) {
        g_rest -= g_timer;
        write_sample(g_buffer + g_position);
        wr32(kLastAddress, g_buffer + g_position);
        g_position += bytes;
        if (g_position >= g_size) {
            g_position = 0;
            if (!g_loop) {
                g_running = false;
                arm7_reply(9, 0x51u << 8);  // buffer full
                return;
            }
        }
    }
}

void receive(u32 data) {
    if (((data >> 24) & 2) != 0) {  // first word
        g_command = (data >> 8) & 0x7F;
        g_type = data & 0xFF;
    } else {
        g_words[(data >> 16) & 7] = data & 0xFFFF;
    }
    if (!(data & 0x01000000)) return;
    switch (g_command) {
        case 0x40:  // one sample
            write_sample(kOneSample);
            break;
        case 0x41: {  // keeps sampling into a buffer
            g_loop = g_type & 0x10;
            g_type &= 0x0F;
            g_buffer = (g_words[1] << 16) | g_words[2];
            g_size = (g_words[3] << 16) | g_words[4];
            g_timer = std::max(1u, (g_words[5] << 16) | g_words[6]);
            g_position = 0;
            g_rest = 0;
            g_running = g_size != 0;
            break;
        }
        case 0x42:  // stop
            g_running = false;
            break;
        default:
            std::printf("microphone command %#x not built\n", g_command);
            break;
    }
    arm7_reply(9, g_command << 8);  // result 0: success
}
}  // namespace mic

void arm7_receive(u32 word) {
    const u32 tag = word & 31;
    const u32 data = word >> 6;
    if (tag == 5) {
        arm7_rtc(data);
        return;
    }
    if (tag == 11) {
        backup::receive(data);
        return;
    }
    if (tag == 8) {
        arm7_power(data);
        return;
    }
    if (tag == 6) {
        arm7_touch(data);
        return;
    }
    if (tag == 7) {
        sound_receive(data);
        return;
    }
    if (tag == 9) {
        mic::receive(data);
        return;
    }
    if (++g_tag_counts[tag] <= 12) {
        std::printf("PXI to ARM7: tag %u data %#x\n", tag, data);
    }
}

u32 io32(u32 offset) {
    u32 value;
    std::memcpy(&value, &g_machine.io[offset], 4);
    return value;
}
void set_io32(u32 offset, u32 value) { std::memcpy(&g_machine.io[offset], &value, 4); }
u16 io16(u32 offset) {
    u16 value;
    std::memcpy(&value, &g_machine.io[offset], 2);
    return value;
}
void set_io16(u32 offset, u16 value) { std::memcpy(&g_machine.io[offset], &value, 2); }

// What the BIOS does on an interrupt: save registers, call the handler the game stored at
// the top of DTCM, and restore on return.
void bios_irq_entry(Cpu& c) {
    u32 sp = c.r[13] - 24;
    c.r[13] = sp;
    wr32(sp, c.r[0]);
    wr32(sp + 4, c.r[1]);
    wr32(sp + 8, c.r[2]);
    wr32(sp + 12, c.r[3]);
    wr32(sp + 16, c.r[12]);
    wr32(sp + 20, c.r[14]);
    c.r[0] = 0x04000000;
    c.r[14] = kIrqReturn;
    c.branch_exchange(rd32(g_machine.dtcm_base + 0x3FFC));
}

void bios_irq_return(Cpu& c) {
    u32 sp = c.r[13];
    c.r[0] = rd32(sp);
    c.r[1] = rd32(sp + 4);
    c.r[2] = rd32(sp + 8);
    c.r[3] = rd32(sp + 12);
    c.r[12] = rd32(sp + 16);
    c.r[14] = rd32(sp + 20);
    c.r[13] = sp + 24;
    c.exception_return(c.r[14] - 4);
}

// DMA ---------------------------------------------------------------------------------------
void dma_run(int channel) {
    const u32 base = 0xB0 + channel * 12;
    u32 src = io32(base), dst = io32(base + 4);
    const u32 control = io32(base + 8);
    u32 count = control & 0x1FFFFF;
    if (count == 0) count = 0x200000;
    const bool words = control >> 26 & 1;
    const u32 step = words ? 4 : 2;
    const u32 dst_mode = (control >> 21) & 3, src_mode = (control >> 23) & 3;
    u32 d = dst, s = src;
    for (u32 i = 0; i < count; ++i) {
        if (words) wr32(d, rd32(s)); else wr16(d, rd16(s));
        if (dst_mode == 0 || dst_mode == 3) d += step; else if (dst_mode == 1) d -= step;
        if (src_mode == 0) s += step; else if (src_mode == 1) s -= step;
    }
    set_io32(base, s);
    if (dst_mode != 3) set_io32(base + 4, d);
    const bool repeat = control >> 25 & 1;
    const u32 timing = (control >> 27) & 7;
    if (!repeat || timing == 0 || timing == 7) set_io32(base + 8, control & 0x7FFFFFFFu);
    if (control >> 30 & 1) hw_raise(1u << (8 + channel));
}

void dma_trigger(u32 timing) {
    for (int channel = 0; channel < 4; ++channel) {
        const u32 control = io32(0xB0 + channel * 12 + 8);
        if ((control >> 31) && ((control >> 27) & 7) == timing) dma_run(channel);
    }
}

// Timers: counted from blocks, so they are exactly repeatable.
constexpr u32 kCyclesPerBlock = 11;
u32 g_timer_counter[4] = {};
u32 g_timer_reload[4] = {};
u32 g_timer_control[4] = {};
u32 g_timer_rest[4] = {};

void timers_step() {
    bool overflow_previous = false;
    for (int t = 0; t < 4; ++t) {
        const u32 control = g_timer_control[t];
        bool overflow = false;
        if (control & 0x80) {
            u32 ticks = 0;
            if (control & 4) {
                ticks = (t > 0 && overflow_previous) ? 1 : 0;
            } else {
                static const u32 kDividers[4] = {1, 64, 256, 1024};
                const u32 divider = kDividers[control & 3];
                g_timer_rest[t] += kCyclesPerBlock;
                ticks = g_timer_rest[t] / divider;
                g_timer_rest[t] %= divider;
            }
            g_timer_counter[t] += ticks;
            while (g_timer_counter[t] >= 0x10000) {
                g_timer_counter[t] = g_timer_counter[t] - 0x10000 + g_timer_reload[t];
                overflow = true;
                if (control & 0x40) hw_raise(irq::kTimer0 << t);
            }
        }
        overflow_previous = overflow;
    }
}

// Math accelerator ----------------------------------------------------------------------------
u64 io64(u32 offset) {
    u64 value;
    std::memcpy(&value, &g_machine.io[offset], 8);
    return value;
}
void set_io64(u32 offset, u64 value) { std::memcpy(&g_machine.io[offset], &value, 8); }

void divide() {
    const u32 mode = io16(0x280) & 3;
    const s64 numerator = mode == 0 ? s64(s32(io32(0x290))) : s64(io64(0x290));
    const s64 denominator = mode == 1 || mode == 0 ? s64(s32(io32(0x298))) : s64(io64(0x298));
    set_io16(0x280, u16(io16(0x280) & ~0x4000u) | (io64(0x298) == 0 ? 0x4000 : 0));
    if (denominator == 0) {
        set_io64(0x2A0, numerator < 0 ? 1 : u64(-1));
        set_io64(0x2A8, u64(numerator));
        return;
    }
    if (numerator == INT64_MIN && denominator == -1) {
        set_io64(0x2A0, u64(numerator));
        set_io64(0x2A8, 0);
        return;
    }
    set_io64(0x2A0, u64(numerator / denominator));
    set_io64(0x2A8, u64(numerator % denominator));
}

void square_root() {
    const bool wide = io16(0x2B0) & 1;
    u64 value = wide ? io64(0x2B8) : io32(0x2B8);
    u64 root = 0, bit = u64(1) << 62;
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
    set_io32(0x2B4, u32(root));
}

// Game card ---------------------------------------------------------------------------------
// The game reads the card through registers: an 8-byte command, then words from 0x04100010,
// often by DMA. Only what comes back matters here, so the card's encryption does not apply.
constexpr u32 kChipId = 0x00001FC2;  // as melonDS reports it for this ROM
u32 g_card_addr = 0, g_card_left = 0;
bool g_card_is_id = false;

void card_finish() {
    set_io32(0x1A4, io32(0x1A4) & ~0x80800000u);
    if (io16(0x1A0) & 0x4000) hw_raise(1u << 19);
}

u32 card_next_word() {
    if (g_card_left == 0) return 0;
    u32 value = kChipId;
    if (!g_card_is_id) {
        const std::vector<u8>& rom = *g_machine.rom;
        const u32 at = g_card_addr < 0x8000 ? 0x8000 + (g_card_addr & 0x1FF) : g_card_addr;
        value = 0;
        if (at + 4 <= rom.size()) std::memcpy(&value, &rom[at], 4);
        g_card_addr += 4;
    }
    g_card_left -= 4;
    if (g_card_left == 0) card_finish();
    return value;
}

void card_start(u32 control) {
    const u8* command = &g_machine.io[0x1A8];
    const u32 code = (control >> 24) & 7;
    static const u32 kSizes[8] = {0, 0x200, 0x400, 0x800, 0x1000, 0x2000, 0x4000, 4};
    g_card_left = kSizes[code];
    g_card_is_id = command[0] == 0xB8;
    g_card_addr = (u32(command[1]) << 24) | (u32(command[2]) << 16) | (u32(command[3]) << 8) | command[4];
    if (command[0] != 0xB7 && command[0] != 0xB8) {
        static int shown = 0;
        if (shown++ < 8) std::printf("card command %02x not handled\n", command[0]);
        g_card_left = 0;
    }
    set_io32(0x1A4, control | (g_card_left ? 0x80800000u : 0));
    if (g_card_left == 0) {
        card_finish();
        return;
    }
    // A DMA channel set to card timing takes each word as it becomes ready.
    for (int channel = 0; channel < 4 && g_card_left; ++channel) {
        const u32 base = 0xB0 + channel * 12;
        const u32 dma = io32(base + 8);
        if ((dma >> 31) && ((dma >> 27) & 7) == 5) {
            while (g_card_left && (io32(base + 8) >> 31)) dma_run(channel);
        }
    }
}

void next_line() {
    g_hw.line_blocks = 0;
    sound_line();
    mic::line();
    ++g_hw.line;
    if (g_hw.line >= kLinesPerFrame) {
        g_hw.line = 0;
        ++g_hw.frames;
    }
    u16 stat = io16(0x04);
    const bool vblank_line = g_hw.line >= kFirstVBlankLine && g_hw.line < kLinesPerFrame - 1;
    if (g_hw.line == kFirstVBlankLine) {
        gpu3d_vblank();
        if (stat & 0x08) hw_raise(irq::kVBlank);
        dma_trigger(1);
    }
    if (g_hw.line < kFirstVBlankLine) {
        ppu_render_line(g_hw.line);
        dma_trigger(2);  // HBlank
    }
    if (g_hw.line == kFirstVBlankLine) ppu_finish_frame();
    if (stat & 0x10) hw_raise(irq::kHBlank);
    stat = u16((stat & ~1u) | (vblank_line ? 1u : 0u));
    const u32 target = (stat >> 8) | ((stat & 0x80u) << 1);
    const bool match = g_hw.line == target;
    stat = u16((stat & ~4u) | (match ? 4u : 0u));
    if (match && (stat & 0x20)) hw_raise(irq::kVCount);
    set_io16(0x04, stat);
}

}  // namespace

void hw_reset() {
    g_hw = HwState{};
    g_ipc_out = 0;
    g_ipc_recv = 0;
    g_ipc_sync_irq_on = g_fifo_send_irq_on = g_fifo_recv_irq_on = g_fifo_on = false;
    g_fifo_to_arm9.clear();
    g_fifo_last = 0;
    g_tag_counts.clear();
    g_keys_pressed = 0;
    backup::g_block = 0;
    backup::g_wait_block = false;
    backup::g_memory.clear();
    backup::g_changed = false;
    g_power_first_word = 0;
    g_power_in_request = false;
    g_touch_word = 0x06000000;
    mic::g_running = mic::g_loop = false;
    mic::g_level = 0;
    mic::g_type = mic::g_buffer = mic::g_size = mic::g_position = mic::g_rest = mic::g_counter = 0;
    mic::g_timer = 1;
    std::fill(std::begin(mic::g_words), std::end(mic::g_words), 0u);
    mic::g_command = 0;
    for (u32* timer : {g_timer_counter, g_timer_reload, g_timer_control, g_timer_rest}) std::fill_n(timer, 4, 0u);
    g_card_addr = g_card_left = 0;
    g_card_is_id = false;
}

void hw_raise(u32 flags) { set_io32(0x214, io32(0x214) | flags); }

void hw_set_keys(u32 pressed) {
    g_keys_pressed = pressed;
    // X and Y travel through the ARM7: bits 10-11 high when released, debug bit 13 stays high.
    wr16(0x027FFFA8u, u16(0x2000u | (~pressed & 0x0C00u)));
}

void hw_set_touch(bool touching, u32 x, u32 y) {
    g_touch_word = touching ? ((x << 4) | (y << 16) | (1u << 24)) : 0x06000000u;
}

void hw_set_mic(bool blowing) { mic::g_level = blowing ? 1.0f : 0.0f; }
void hw_set_mic_level(float level) { mic::g_level = level; }

void hw_set_save(const std::vector<u8>& contents) { backup::g_memory = contents; }

const std::vector<u8>& hw_save() { return backup::g_memory; }

bool hw_save_changed() {
    const bool changed = backup::g_changed;
    backup::g_changed = false;
    return changed;
}

void hw_step(Cpu& c) {
    if (++g_hw.line_blocks >= kBlocksPerLine) next_line();
    timers_step();
    const u32 pending = io32(0x210) & io32(0x214);
    if (!pending) return;
    g_hw.halted = false;
    if ((io16(0x208) & 1) && !c.irq_off) {
        const u32 old = c.cpsr();
        const u32 return_to = c.pc + 4;
        c.switch_mode(kIrq);
        c.spsr = old;
        c.r[14] = return_to;
        c.irq_off = 1;
        c.thumb = 0;
        c.pc = kIrqEntry;
    }
}

bool hw_read(u32 addr, u32 size, u32& value) {
    (void)size;
    if (addr >= 0x04000600 && addr < 0x040006A4) {
        value = gpu3d_read(addr);
        return true;
    }
    if (addr >= 0x04000100 && addr < 0x04000110) {
        const int timer = (addr - 0x04000100) / 4;
        if (addr & 2) {
            value = g_timer_control[timer];
        } else {
            value = (g_timer_counter[timer] & 0xFFFF) | (g_timer_control[timer] << 16);
        }
        return true;
    }
    switch (addr) {
        case 0x04100010: value = card_next_word(); return true;
        case 0x04000006: value = g_hw.line; return true;
        case 0x04000130: value = 0x03FF & ~g_keys_pressed; return true;  // low means pressed
        case 0x04000180:  // IPCSYNC: low nibble comes from the ARM7
            value = g_ipc_recv | (u32(g_ipc_out) << 8) | (g_ipc_sync_irq_on ? 0x4000u : 0);
            return true;
        case 0x04000184:  // IPCFIFOCNT; the ARM7 stand-in takes words at once, so send is empty
            value = 1u | (g_fifo_send_irq_on ? 4u : 0) | (g_fifo_to_arm9.empty() ? 0x100u : 0)
                    | (g_fifo_recv_irq_on ? 0x400u : 0) | (g_fifo_on ? 0x8000u : 0);
            return true;
        case 0x04100000:  // IPCFIFORECV
            if (g_fifo_to_arm9.empty()) {
                value = g_fifo_last;
            } else {
                value = g_fifo_last = g_fifo_to_arm9.front();
                g_fifo_to_arm9.pop_front();
            }
            return true;
        default: return false;
    }
}

bool hw_write(u32 addr, u32 size, u32 value) {
    if (addr >= 0x04000400 && addr < 0x04000600) {
        gpu3d_write(addr, size, value);
        return true;
    }
    if (addr == 0x04000600) {
        gpu3d_write_status(value);
        return true;
    }
    if (addr >= 0x04000100 && addr < 0x04000110) {
        const int timer = (addr - 0x04000100) / 4;
        const auto set_control = [&](u32 control) {
            if ((control & 0x80) && !(g_timer_control[timer] & 0x80)) {
                g_timer_counter[timer] = g_timer_reload[timer];
                g_timer_rest[timer] = 0;
            }
            g_timer_control[timer] = control & 0xFFFF;
        };
        if (addr & 2) {
            set_control(value & 0xFFFF);
        } else {
            g_timer_reload[timer] = value & 0xFFFF;
            if (size == 4) set_control(value >> 16);
        }
        return true;
    }
    if (addr >= 0x040000B0 && addr < 0x040000E0) {  // DMA
        const u32 offset = addr & 0xFFFF;
        const u32 relative = (offset - 0xB0) % 12;
        if (size == 4) set_io32(offset, value);
        else if (size == 2) set_io16(offset, u16(value));
        else g_machine.io[offset] = u8(value);
        if (relative >= 8) {
            const int channel = (offset - 0xB0) / 12;
            const u32 control = io32(0xB0 + channel * 12 + 8);
            const u32 timing = (control >> 27) & 7;
            // Timing 7 feeds the geometry FIFO. Its commands run at once, so it is never full.
            if ((control >> 31) && (timing == 0 || timing == 7)) dma_run(channel);
        }
        return true;
    }
    if (addr >= 0x04000280 && addr < 0x040002C0) {  // math
        const u32 offset = addr & 0xFFFF;
        if (size == 4) set_io32(offset, value);
        else if (size == 2) set_io16(offset, u16(value));
        else g_machine.io[offset] = u8(value);
        if (offset < 0x2B0) divide(); else square_root();
        return true;
    }
    if (addr == 0x040001A4 && size == 4) {  // card control
        if (value >> 31) {
            card_start(value & 0x7F7FFFFFu);
        } else {
            set_io32(0x1A4, value);
        }
        return true;
    }
    if (addr == 0x04000180) {  // IPCSYNC
        g_ipc_out = u8((value >> 8) & 15);
        g_ipc_sync_irq_on = value >> 14 & 1;
        // The game counts through values with the ARM7 during PXI setup; answer one higher.
        g_ipc_recv = u8((g_ipc_out + 1) & 15);
        return true;
    }
    if (addr == 0x04000184) {  // IPCFIFOCNT
        g_fifo_send_irq_on = value >> 2 & 1;
        g_fifo_recv_irq_on = value >> 10 & 1;
        g_fifo_on = value >> 15 & 1;
        if (value & 8) g_fifo_to_arm9.clear();
        return true;
    }
    if (addr == 0x04000188) {  // IPCFIFOSEND
        arm7_receive(value);
        return true;
    }
    if (addr == 0x04000214) {  // IF: writing 1 clears a flag
        const u32 mask = size == 4 ? value : size == 2 ? (value & 0xFFFF) : (value & 0xFF);
        set_io32(0x214, io32(0x214) & ~mask);
        return true;
    }
    if (addr < 0x0400024A && addr + size > 0x04000240) {  // VRAM bank control
        for (u32 i = 0; i < size; ++i) g_machine.io[(addr & 0xFFFF) + i] = u8(value >> (i * 8));
        vram_remap();
        return true;
    }
    if (addr == 0x04000004 && size >= 2) {  // DISPSTAT: flag bits are read only
        const u16 old = io16(0x04);
        set_io16(0x04, u16((value & 0xFFB8) | (old & 7)));
        return true;
    }
    return false;
}

BlockFn bios_block(u32 pc) {
    if (pc == kIrqEntry) return bios_irq_entry;
    if (pc == kIrqReturn) return bios_irq_return;
    return nullptr;
}
