#include "overlay.h"

#include "memory.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace {

constexpr u32 kLoadOverlay = 0x02069978;    // r0 = cpu, r1 = id
constexpr u32 kUnloadOverlay = 0x020699CC;  // r0 = cpu, r1 = id
constexpr u32 kCpuArm9 = 0;

std::vector<const OverlayInfo*> g_loaded;

const OverlayInfo* info_for(u32 id) {
    return id < kOverlayCount ? &kOverlays[id] : nullptr;
}

void mark_loaded(u32 id) {
    const OverlayInfo* info = info_for(id);
    if (info == nullptr) runtime_abort("load of an unknown overlay", id);
    // Whatever shared these addresses has been overwritten.
    std::erase_if(g_loaded, [info](const OverlayInfo* other) {
        return other->start < info->end && info->start < other->end;
    });
    g_loaded.push_back(info);
    std::printf("overlay %u loaded: %08x-%08x\n", id, info->start, info->end);
}

void mark_unloaded(u32 id) {
    const OverlayInfo* info = info_for(id);
    std::erase(g_loaded, info);
    std::printf("overlay %u unloaded\n", id);
}

}  // namespace

void overlay_watch_calls(const Cpu& c) {
    if (c.pc == kLoadOverlay && c.r[0] == kCpuArm9) mark_loaded(c.r[1]);
    else if (c.pc == kUnloadOverlay && c.r[0] == kCpuArm9) mark_unloaded(c.r[1]);
}

BlockFn overlay_find_block(u32 pc, u32 thumb, int* overlay_id) {
    *overlay_id = -1;
    for (const OverlayInfo* info : g_loaded) {
        if (pc < info->start || pc >= info->end) continue;
        *overlay_id = int(info->id);
        const u32 key = pc | thumb;
        const OverlayBlock* end = info->blocks + info->count;
        const OverlayBlock* hit = std::lower_bound(info->blocks, end, key,
            [](const OverlayBlock& b, u32 k) { return b.key < k; });
        if (hit == end || hit->key != key) return nullptr;
        const u32 actual = thumb ? rd16(pc) : rd32(pc);
        if (actual != hit->first) {
            std::printf("overlay %u: RAM has %08x, translated code has %08x\n", info->id, actual,
                        hit->first);
            runtime_abort("code in RAM differs from the translated overlay", pc);
        }
        return hit->fn;
    }
    return nullptr;
}
