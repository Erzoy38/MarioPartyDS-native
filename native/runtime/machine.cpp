#include "machine.h"

#include "hw.h"
#include "memory.h"
#include "net.h"
#include "overlay.h"
#include "sound.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

Cpu g_cpu;
AbortHook g_abort_hook = nullptr;

// Finding a block by search is slow, and most blocks run again soon. Emptied when overlays change,
// because the same address then means other code. It only saves time: the result stays the same.
struct BlockCacheEntry {
    u32 key;
    BlockFn fn;
};
constexpr u32 kBlockCacheSize = 1u << 16;
BlockCacheEntry g_block_cache[kBlockCacheSize];

u32 header_u32(const std::vector<u8>& rom, std::size_t offset) {
    u32 value;
    std::memcpy(&value, &rom[offset], 4);
    return value;
}

void print_state(const Cpu& c) {
    std::printf("pc=%08x cpsr=%08x\n", c.pc, c.cpsr());
    for (int i = 0; i < 16; ++i) {
        std::printf("r%-2d=%08x%s", i, c.r[i], (i % 4 == 3) ? "\n" : " ");
    }
    std::printf("unknown memory: reads %u, writes %u\n", g_machine.unknown_reads,
                g_machine.unknown_writes);
    std::printf("code addresses on the stack:");
    int shown = 0;
    for (u32 i = 0; i < 256 && shown < 16; ++i) {
        const u32 value = rd32(c.r[13] + i * 4);
        if ((value >= 0x02000000 && value < 0x021A5000) || (value >= 0x01FF8000 && value < 0x01FFD300)) {
            std::printf(" %08x", value);
            ++shown;
        }
    }
    std::printf("\n");
    std::fflush(stdout);
}

// The owner settings the firmware leaves at 0x027FFC80 for the game, in the layout seen in
// melonDS: version, birthday, name, touch calibration, language. Fixed except for the name, so
// every player's game starts alike.
void write_user_settings(const std::string& name) {
    constexpr u32 kSettings = 0x027FFC80;
    constexpr u32 kLanguageEnglish = 1;
    for (u32 i = 0; i < 0x70; ++i) wr8(kSettings + i, 0);
    wr16(kSettings, 5);       // version
    wr8(kSettings + 3, 1);    // birthday, month
    wr8(kSettings + 4, 1);    // birthday, day
    const std::size_t length = std::min<std::size_t>(name.size(), 10);
    for (std::size_t i = 0; i < length; ++i) wr16(kSettings + 6 + u32(i) * 2, u8(name[i]));
    wr16(kSettings + 0x1A, u16(length));
    // Calibration: raw 0x0FF0 is pixel 255, raw 0x0BF0 is pixel 191, raw 0 is pixel 0.
    wr16(kSettings + 0x5E, 0x0FF0);
    wr16(kSettings + 0x60, 0x0BF0);
    wr8(kSettings + 0x62, 0xFF);
    wr8(kSettings + 0x63, 0xBF);
    wr8(kSettings + 0x64, u8(0x30 | kLanguageEnglish));  // upper bits as seen in melonDS
}

}  // namespace

[[noreturn]] void runtime_abort(const char* reason, u32 addr) {
    std::printf("ABORT: %s at %08x\n", reason, addr);
    print_state(g_cpu);
    if (g_abort_hook) g_abort_hook(reason, addr);
    std::exit(3);
}

void software_interrupt(Cpu& c, u32 number) { bios_swi(c, number); }

void cp15_write(Cpu&, u32 op1, u32 crn, u32 crm, u32 op2, u32 value) {
    // Caches and protection do not matter here; the TCM base and the idle wait do.
    if (op1 == 0 && crn == 9 && crm == 1 && op2 == 0) {
        g_machine.dtcm_base = value & 0xFFFFF000u;
    }
    if (op1 == 0 && crn == 7 && crm == 0 && op2 == 4) {  // wait for interrupt
        g_hw.halted = true;
    }
}

u32 cp15_read(Cpu&, u32, u32, u32, u32) { return 0; }

std::string machine_check_rom(const std::vector<u8>& rom) {
    // The translated code belongs to exactly this release.
    if (rom.size() < 0x200 || std::memcmp(&rom[0x0C], "A8TP", 4) != 0) {
        return "This is not Mario Party DS (Europe).";
    }
    if (rom[0x1E] != 1) return "This is another revision of the game; Rev 1 is needed.";
    return {};
}

void machine_boot(const std::vector<u8>& rom, const MachineSettings& settings) {
    memory_init();
    g_machine.rom = &rom;
    sound_init(rom);
    const u32 rom_offset = header_u32(rom, 0x20);
    const u32 entry = header_u32(rom, 0x24);
    const u32 ram_address = header_u32(rom, 0x28);
    const u32 size = header_u32(rom, 0x2C);
    for (u32 i = 0; i < size; ++i) wr8(ram_address + i, rom[rom_offset + i]);
    for (u32 i = 0; i < 0x170; ++i) wr8(0x027FFE00u + i, rom[i]);
    wr16(0x027FFC40u, settings.boot_type);  // boot type: 1 card, 2 download play
    wr32(0x027FFF8Cu, 0x0003FFF0u);  // ARM7 services ready: the channels set in melonDS
    hw_set_keys(0);
    write_user_settings(settings.player_name);
    if (!settings.save.empty()) hw_set_save(settings.save);
    // Boot data the firmware leaves behind, as seen in melonDS. The game compares the chip id
    // here with the card's to detect a removed card.
    for (u32 base : {0x027FF800u, 0x027FFC00u}) {
        wr32(base, 0x00001FC2u);
        wr32(base + 4, 0x00001FC2u);
    }
    g_cpu.pc = entry;
}

Cpu& machine_cpu() { return g_cpu; }

void machine_step() {
    Cpu& c = g_cpu;
    hw_step(c);
    if (g_hw.halted) return;  // time passes until an interrupt wakes the CPU
    if (overlay_watch_calls(c)) std::memset(g_block_cache, 0, sizeof g_block_cache);
    if (c.pc == kGameTick) net_tick();
    if (net_hook(c)) return;
    const u32 key = c.pc | c.thumb;
    BlockCacheEntry& cached = g_block_cache[(key >> 1) & (kBlockCacheSize - 1)];
    if (cached.key == key && cached.fn != nullptr) {
        cached.fn(c);
        return;
    }
    BlockFn block = bios_block(c.pc);
    int overlay_id = -1;
    if (block == nullptr) {
        if (in_overlay_area(c.pc)) {
            block = overlay_find_block(c.pc, c.thumb, &overlay_id);
            if (overlay_id < 0) runtime_abort("no overlay loaded at this address", c.pc);
        } else {
            block = find_block(c.pc | c.thumb);
        }
    }
    if (block == nullptr) {
        if (overlay_id >= 0) std::printf("MISSING %08x %d ov%d\n", c.pc, c.thumb, overlay_id);
        else std::printf("MISSING %08x %d\n", c.pc, c.thumb);
        runtime_abort(c.thumb ? "no translated Thumb code" : "no translated code", c.pc);
    }
    cached = {key, block};
    block(c);
}

void machine_run_frame() {
    const u64 start = g_hw.frames;
    while (g_hw.frames == start) machine_step();
}

void machine_set_abort_hook(AbortHook hook) { g_abort_hook = hook; }

const std::vector<u8>& machine_save() { return hw_save(); }

bool machine_save_changed() { return hw_save_changed(); }
