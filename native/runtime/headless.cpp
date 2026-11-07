// Runs the translated game without a window, up to a stop address, and dumps memory for
// comparison. Reads the player's ROM at run time; nothing from it is built into this file.
//
// Usage: MarioPartyDS-headless <rom.nds> --stop 0x02003000 --dump <folder> [--max-blocks N]
//        [--dump-tick N]... [--dump-step N]... [--watch ADDR]... [--name NAME] [--save FILE (read only)]
//        [--input TICK:WHAT[:FRAMES]]... [--language N (1 en, 2 fr, 3 de, 4 it, 5 es)]
//
// Through the lobby server instead of --net-host/--net-guest:
//        --lobby ADDRESS[:PORT] --character NAME (--create CODEFILE | --join CODE|@CODEFILE)
//        [--start-players N] [--lobby-exit 1]
// --create makes a room and writes its code to CODEFILE ("-": nowhere); the host starts once N
// players (default 4) are in and ready. --join @FILE waits for FILE and joins with its code; guests
// are ready at once. --lobby-exit 1 ends right after the start, for lobby tests.
#define _CRT_SECURE_NO_WARNINGS  // sscanf reads the command line here, with field widths
#include "cpu.h"
#include "gpu3d.h"
#include "hw.h"
#include "lobby.h"
#include "machine.h"
#include "memory.h"
#include "net.h"
#include "ppu.h"
#include "sound.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

std::vector<u8> read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        std::exit(2);
    }
    return std::vector<u8>(std::istreambuf_iterator<char>(file), {});
}

void write_dump(const std::filesystem::path& folder, const char* name, const std::vector<u8>& data) {
    std::ofstream out(folder / name, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
}

void write_all_dumps(const std::string& path) {
    std::filesystem::create_directories(path);
    const std::filesystem::path folder(path);
    write_dump(folder, "main_ram.bin", g_machine.main_ram);
    write_dump(folder, "itcm.bin", g_machine.itcm);
    write_dump(folder, "dtcm.bin", g_machine.dtcm);
    write_dump(folder, "palette.bin", g_machine.palette);
    write_dump(folder, "oam.bin", g_machine.oam);
    std::ofstream picture(folder / "frame.rgba", std::ios::binary);
    picture.write(reinterpret_cast<const char*>(ppu_frame()),
                  std::streamsize(kScreenWidth) * kScreenHeight * 2 * 4);
}

// --wav FILE: writes the sound of the whole run, to hear and check it.
std::string g_wav_path;
std::vector<s16> g_wav_samples;
std::vector<s16> g_wav_chunk;

void collect_sound() {
    sound_take_samples(g_wav_chunk);
    g_wav_samples.insert(g_wav_samples.end(), g_wav_chunk.begin(), g_wav_chunk.end());
}

void write_wav() {
    if (g_wav_path.empty()) return;
    std::ofstream out(g_wav_path, std::ios::binary);
    const u32 bytes = u32(g_wav_samples.size() * 2);
    const auto put32 = [&](u32 v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    const auto put16 = [&](u16 v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("RIFF", 4); put32(36 + bytes); out.write("WAVEfmt ", 8); put32(16);
    put16(1); put16(2); put32(kSoundRate); put32(kSoundRate * 4); put16(4); put16(16);
    out.write("data", 4); put32(bytes);
    out.write(reinterpret_cast<const char*>(g_wav_samples.data()), bytes);
    std::printf("wrote %s: %.1f seconds\n", g_wav_path.c_str(), double(g_wav_samples.size()) / 2 / kSoundRate);
}

// --dump-tick N, repeatable: also dumps to <dump>/tickN when game tick N starts.
std::set<u64> g_dump_ticks;
// --dump-step N, repeatable: dumps to <dump>/stepN right after exchange step N. Network players are
// at the same point of the game there, while their tick counts can differ.
std::set<u64> g_dump_steps;

// --frame-range FROM:TO, repeatable: writes only the picture at each game tick in the range, to
// <dump>/frames/N.rgba. For finding the frame that matches a screenshot.
std::vector<std::pair<u64, u64>> g_frame_ranges;

void write_frame_if_wanted(const std::string& dump, u64 tick) {
    for (const auto& [from, to] : g_frame_ranges) {
        if (tick < from || tick > to) continue;
        const std::filesystem::path folder = std::filesystem::path(dump) / "frames";
        std::filesystem::create_directories(folder);
        std::ofstream picture(folder / (std::to_string(tick) + ".rgba"), std::ios::binary);
        picture.write(reinterpret_cast<const char*>(ppu_frame()),
                      std::streamsize(kScreenWidth) * kScreenHeight * 2 * 4);
        return;
    }
}

// Sampled block addresses, to show where a stuck run spends its time.
std::map<u32, u32> g_hot;

// In a network run, a player that stops taking part blocks everybody. Shows where this one is when
// no exchange step has come for 10 s (once per stall).
std::map<u32, u32> g_stall_hot;
void watch_network_stall(const Cpu& c, u64 blocks) {
    static u64 last_steps = ~0ull;
    static auto last_change = std::chrono::steady_clock::now();
    static bool reported = false;
    if ((blocks & 0xFFF) == 0) ++g_stall_hot[c.pc];
    if ((blocks & 0xFFFFF) != 0) return;
    const auto now = std::chrono::steady_clock::now();
    if (net_stats().steps != last_steps) {
        last_steps = net_stats().steps;
        last_change = now;
        reported = false;
        g_stall_hot.clear();
        return;
    }
    if (reported || now - last_change < std::chrono::seconds(10)) return;
    reported = true;
    std::printf("STALL: no exchange step for 10 s after step %llu, pc=%08x lr=%08x sp=%08x cpsr=%08x halted %d\n",
                static_cast<unsigned long long>(last_steps), c.pc, c.r[14], c.r[13], c.cpsr(), g_hw.halted);
    std::printf("STALL stack:");
    for (u32 i = 0, shown = 0; i < 128 && shown < 12; ++i) {
        const u32 value = rd32(c.r[13] + i * 4);
        if (value >= 0x02000000 && value < 0x021A5000 && (value & 3) == 0) {
            std::printf(" %08x", value);
            ++shown;
        }
    }
    std::printf("\nSTALL blocks:");
    std::vector<std::pair<u32, u32>> hot(g_stall_hot.begin(), g_stall_hot.end());
    std::sort(hot.begin(), hot.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (std::size_t i = 0; i < hot.size() && i < 12; ++i) std::printf(" %08x(%u)", hot[i].first, hot[i].second);
    std::printf("\n");
}

// Every 10 s of a network run: steps, pings, connections opened again.
void print_net_status() {
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(10)) return;
    last = now;
    const NetStats& stats = net_stats();
    std::printf("net status: step %llu, %s, server %u ms, reconnects %llu, resent %llu\n",
                static_cast<unsigned long long>(stats.steps), net_status_line().c_str(), stats.server_ping_ms,
                static_cast<unsigned long long>(stats.reconnects), static_cast<unsigned long long>(stats.resent));
}

void print_hot_blocks() {
    std::vector<std::pair<u32, u32>> hits;
    for (const auto& [pc, count] : g_hot) hits.push_back({count, pc});
    std::sort(hits.rbegin(), hits.rend());
    std::printf("most sampled blocks:");
    for (std::size_t i = 0; i < hits.size() && i < 10; ++i) {
        std::printf(" %08x(%u)", hits[i].second, hits[i].first);
    }
    std::printf("\n");
}

u64 g_game_ticks = 0;
u64 g_scene_changes = 0;

// How far the game itself has come, as opposed to how long the machine has run.
void print_progress() {
    std::printf("game ticks %llu, scene changes %llu, scene %d, mode %u, next mode %u, slots %d %d\n",
                static_cast<unsigned long long>(g_game_ticks),
                static_cast<unsigned long long>(g_scene_changes), s16(rd16(kModeState)),
                rd8(kModeState + 0xC), rd8(kModeState + 0xD), s16(rd16(kSlotScene)),
                s16(rd16(kSlotScene + 2)));
    std::printf("3D lines %llu, lists drawn %llu, last list %u polygons %u vertices, unknown 3D commands %llu\n",
                static_cast<unsigned long long>(ppu_gaps().lines_with_3d),
                static_cast<unsigned long long>(gpu3d_stats().lists_drawn), gpu3d_stats().last_polygons,
                gpu3d_stats().last_vertices, static_cast<unsigned long long>(gpu3d_stats().unknown_commands));
    std::printf("lines captured: %llu\n",
                static_cast<unsigned long long>(ppu_gaps().lines_with_capture));
}

// --watch ADDR, repeatable: counts how often a block start is reached. For finding a stuck spot.
std::map<u32, u64> g_watched;
std::set<u32> g_traced;  // --trace ADDR: every call is logged with r0, r1 and the return address

void print_watched() {
    for (const auto& [address, count] : g_watched) {
        std::printf("watched %08x: %llu\n", address, static_cast<unsigned long long>(count));
    }
}

// --input TICK:WHAT[:FRAMES], repeatable, from game tick TICK on for FRAMES ticks (default 6).
// WHAT is a key name, MIC for blowing into the microphone, or touch;X;Y in pixels. Same script, same run: this is how tests play.
constexpr u32 kMicBit = 1u << 16;

struct ScriptedInput {
    u64 tick;
    u32 keys;
    bool touch;
    u32 x, y;
    u64 frames;
};
std::vector<ScriptedInput> g_script;

void add_input(const char* text) {
    static const char* const kNames[] = {"A", "B", "SELECT", "START", "RIGHT", "LEFT",
                                         "UP", "DOWN", "R", "L", "X", "Y"};
    char what[32] = {};
    unsigned long long tick = 0, frames = 6;
    if (std::sscanf(text, "%llu:%31[^:]:%llu", &tick, what, &frames) < 2) {
        std::fprintf(stderr, "bad --input %s\n", text);
        std::exit(2);
    }
    ScriptedInput input{tick, 0, false, 0, 0, frames};
    if (std::sscanf(what, "touch;%u;%u", &input.x, &input.y) == 2) {
        input.touch = true;
    } else if (!std::strcmp(what, "MIC")) {
        input.keys = kMicBit;
    } else {
        for (u32 i = 0; i < 12; ++i) {
            if (!std::strcmp(what, kNames[i])) input.keys = 1u << i;
        }
        if (input.keys == 0) {
            std::fprintf(stderr, "bad key in --input %s\n", text);
            std::exit(2);
        }
    }
    g_script.push_back(input);
}

void apply_script(u64 tick) {
    u32 keys = 0;
    bool touching = false;
    u32 x = 0, y = 0;
    for (const ScriptedInput& input : g_script) {
        if (tick < input.tick || tick >= input.tick + input.frames) continue;
        keys |= input.keys;
        if (input.touch) {
            touching = true;
            x = input.x;
            y = input.y;
        }
    }
    u32 auto_x = 0, auto_y = 0;
    if (net_auto_touch(auto_x, auto_y)) {
        touching = true;
        x = auto_x;
        y = auto_y;
    }
    hw_set_keys(keys & 0xFFF);
    hw_set_mic(keys & kMicBit);
    hw_set_touch(touching, x, y);
}

struct LobbyOptions {
    std::string server;  // ADDRESS[:PORT]; empty: no lobby
    std::string create;  // code file, when this one makes the room
    std::string join;    // CODE or @FILE
    int character = 0;
    int start_players = 4;
    bool exit_after_start = false;
    std::string known_servers;  // --known-servers FILE: server keys seen before; empty: none kept
};

int character_by_name(const char* name) {
    for (int i = 0; i < lobby::kCharacters; ++i) {
        if (!_stricmp(name, lobby::character_name(i))) return i;
    }
    std::fprintf(stderr, "unknown character %s\n", name);
    std::exit(2);
}

// Goes through the lobby until the host starts. Returns 0, or 3 if refused or failed.
int run_lobby(const LobbyOptions& options, const std::string& name, NetSettings& net, MachineSettings& settings) {
    std::string address = options.server;
    u16 port = lobby::kDefaultPort;
    const size_t colon = address.rfind(':');
    if (colon != std::string::npos) {
        port = u16(std::atoi(address.c_str() + colon + 1));
        address.resize(colon);
    }
    std::string code = options.join;
    if (!code.empty() && code[0] == '@') {
        const std::string path = code.substr(1);
        for (int waited = 0; !std::filesystem::exists(path); waited += 50) {
            if (waited > 120000) {
                std::printf("lobby: no code file %s\n", path.c_str());
                return 3;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::ifstream file(path);
        std::getline(file, code);
    }
    LobbyClient client;
    client.set_known_servers(options.known_servers);
    client.connect(address, port, name);
    bool asked = false, ready_sent = false, start_sent = false, code_written = false;
    std::uint64_t seen_updates = 0;
    const auto begin = std::chrono::steady_clock::now();
    for (;;) {
        client.poll();
        const LobbyClient::Phase phase = client.phase();
        if (phase == LobbyClient::Phase::Failed) return 3;
        if (phase == LobbyClient::Phase::Menu) {
            if (asked && client.last_error() != 0) {
                std::printf("lobby: refused: %s (error %d)\n", client.message().c_str(), client.last_error());
                return 3;
            }
            if (!asked) {
                if (!options.create.empty()) client.create(options.character);
                else client.join(code, options.character);
                asked = true;
            }
        }
        if (phase == LobbyClient::Phase::InRoom) {
            if (client.room_updates() != seen_updates) {
                seen_updates = client.room_updates();
                std::printf("lobby: room %s, own slot %d:", client.code().c_str(), client.own_slot());
                int present = 0, ready = 0;
                for (const LobbySeat& seat : client.seats()) {
                    if (!seat.present) continue;
                    ++present;
                    ready += seat.ready;
                    std::printf(" [%s %s%s %dms]", seat.name.c_str(), lobby::character_name(seat.character),
                                seat.ready ? " ready" : "", seat.ping_ms);
                }
                std::printf("\n");
                if (!options.create.empty() && present >= options.start_players && ready == present && !start_sent) {
                    client.start();
                    start_sent = true;
                }
            }
            if (!options.create.empty() && !code_written) {
                code_written = true;
                if (options.create != "-") {
                    const std::string temporary = options.create + ".new";
                    { std::ofstream(temporary) << client.code() << "\n"; }
                    std::filesystem::rename(temporary, options.create);
                }
            }
            if (options.create.empty() && !ready_sent) {
                client.set_ready(true);
                ready_sent = true;
            }
            if (start_sent && client.last_error() != 0) {
                std::printf("lobby: start refused: %s\n", client.message().c_str());
                return 3;
            }
        }
        if (phase == LobbyClient::Phase::Started) break;
        if (std::chrono::steady_clock::now() - begin > std::chrono::seconds(180)) {
            std::printf("lobby: no start within 180 s\n");
            return 3;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    LobbyStart start = client.take_start();
    net.role = start.slot == 0 ? NetRole::Host : NetRole::Guest;
    net.relay = start.socket;
    net.relay_inbox = std::move(start.inbox);
    net.relay_channel = start.channel;
    net.relay_outbox = std::move(start.outbox);
    net.server_key = start.server_key;
    net.character = start.characters[size_t(start.slot)];
    net.address = start.address;
    net.port = start.port;
    net.code = start.code;
    net.slot = start.slot;
    net.token = start.token;
    net.tokens = start.tokens;
    net.players = start.players;
    net.input_delay = start.input_delay;
    if (net.role == NetRole::Guest) settings.boot_type = 2;
    std::printf("lobby: player %d of %d, %s\n", start.slot + 1, start.players, lobby::character_name(net.character));
    return 0;
}

// --force-mode MODE: the first time the game picks its next mode after the title (mode 0x86, the
// save select) it goes to MODE instead. A shortcut for tests; the state a real menu would have set
// up is missing, so a scene reached this way may behave differently.
int g_force_mode = -1;
constexpr u32 kSetNextMode = 0x0201A3AC;

void apply_forced_mode(Cpu& c) {
    if (g_force_mode >= 0 && c.pc == kSetNextMode && c.r[0] == 0x86) {
        std::printf("forcing mode %#x\n", g_force_mode);
        c.r[0] = u32(g_force_mode);
        g_force_mode = -1;
    }
}

// --unlock-minigames N: marks minigames 0..N-1 as played in memory, every tick, so Free Play lists
// them all. The list is a bit field in the save block (0x021547B8, read by 0x0204AE0C, set by
// 0x0204ADC0). A test aid: the save file is not written, and a real save would carry the bits.
int g_unlock_minigames = 0;
constexpr u32 kPlayedMinigames = 0x021547B8;

void apply_unlock(const Cpu& c) {
    if (g_unlock_minigames <= 0 || c.pc != kGameTick) return;
    for (int i = 0; i < g_unlock_minigames && i < 80; ++i) {
        const u32 address = kPlayedMinigames + u32(i / 8);
        wr8(address, u8(rd8(address) | (1u << (i % 8))));
    }
}

// --exit-frames FOLDER: in a network run, keeps the picture of each of the last 128 exchange steps and,
// when a minigame scene (1 to 77) ends at step S, writes the pictures of steps S-60 and S-120 as
// exit<scene>_step<N>.rgba. Every player writes the same steps, so their winner screens compare.
std::string g_exit_frames;
constexpr u32 kFrameRing = 128;
constexpr std::size_t kFrameBytes = std::size_t(kScreenWidth) * kScreenHeight * 2 * 4;
std::vector<std::vector<u8>> g_frame_ring;

void keep_step_frame(u64 step) {
    if (g_exit_frames.empty()) return;
    if (g_frame_ring.empty()) g_frame_ring.assign(kFrameRing, std::vector<u8>(kFrameBytes));
    std::memcpy(g_frame_ring[step % kFrameRing].data(), ppu_frame(), kFrameBytes);
}

// --end-after-minigame 1: the run ends when the first minigame scene ends.
bool g_end_after_minigame = false;
// --leave-at-tick N: this player leaves the network game on purpose at game tick N, as with Esc and
// "back to the main menu" in the window.
u64 g_leave_tick = 0;
std::string g_leave_file;  // --leave-file PATH: the same, as soon as PATH exists (looked at every 60 ticks)
bool g_minigame_ended = false;
u64 g_minigame_end_step = 0;  // the others still need steps from this player to see the end too

void save_exit_frames(s16 previous, s16 scene) {
    if (previous >= 1 && previous <= 77 && scene != previous && !g_minigame_ended) {
        g_minigame_ended = true;
        g_minigame_end_step = net_stats().steps;
    }
    if (g_exit_frames.empty() || previous < 1 || previous > 77 || scene == previous) return;
    const u64 step = net_stats().steps;
    std::filesystem::create_directories(g_exit_frames);
    for (u64 back : {60ull, 120ull}) {
        if (step < back) continue;
        const std::string name = "exit" + std::to_string(previous) + "_step" + std::to_string(step - back) + ".rgba";
        std::ofstream out(std::filesystem::path(g_exit_frames) / name, std::ios::binary);
        out.write(reinterpret_cast<const char*>(g_frame_ring[(step - back) % kFrameRing].data()),
                  std::streamsize(kFrameBytes));
    }
}

void watch_progress(const Cpu& c) {
    static s16 last_scene = -1;
    if (c.pc == kGameTick) apply_script(g_game_ticks + 1);
    if (!g_watched.empty()) {
        const auto hit = g_watched.find(c.pc);
        if (hit != g_watched.end() && hit->second++ == 0) {
            std::printf("watched %08x first reached at game tick %llu\n", c.pc, static_cast<unsigned long long>(g_game_ticks));
        }
    }
    if (!g_traced.empty() && g_traced.count(c.pc) != 0) {
        std::printf("trace %08x r0=%08x r1=%08x lr=%08x tick %llu\n", c.pc, c.r[0], c.r[1], c.r[14],
                    static_cast<unsigned long long>(g_game_ticks));
    }
    if (c.pc == kGameTick) ++g_game_ticks;
    const s16 scene = s16(rd16(kModeState));
    if (scene != last_scene) {
        const s16 previous = last_scene;
        last_scene = scene;
        ++g_scene_changes;
        std::printf("scene %d at game tick %llu, step %llu\n", scene, static_cast<unsigned long long>(g_game_ticks),
                    static_cast<unsigned long long>(net_stats().steps));
        save_exit_frames(previous, scene);
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // a process stopped from outside still leaves its log
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <rom.nds> --stop ADDR --dump FOLDER [--max-blocks N]\n", argv[0]);
        return 2;
    }
    u32 stop = 0x02003000;
    u64 max_blocks = 2000000000ULL;
    std::string dump;
    MachineSettings settings;
    NetSettings net;
    LobbyOptions lobby_options;
    int input_delay = -1;  // pictures; -1: the server's choice, or none on a direct connection
    std::string rejoin_from;  // --rejoin FILE: back into a running game, from what --rejoin-file wrote
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--lobby")) lobby_options.server = argv[i + 1];
        else if (!std::strcmp(argv[i], "--create")) lobby_options.create = argv[i + 1];
        else if (!std::strcmp(argv[i], "--join")) lobby_options.join = argv[i + 1];
        else if (!std::strcmp(argv[i], "--character")) lobby_options.character = character_by_name(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--start-players")) lobby_options.start_players = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--lobby-exit")) lobby_options.exit_after_start = std::atoi(argv[i + 1]) != 0;
        else if (!std::strcmp(argv[i], "--known-servers")) lobby_options.known_servers = argv[i + 1];
    }
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--stop")) stop = u32(std::strtoul(argv[i + 1], nullptr, 0));
        else if (!std::strcmp(argv[i], "--dump")) dump = argv[i + 1];
        else if (!std::strcmp(argv[i], "--max-blocks")) max_blocks = std::strtoull(argv[i + 1], nullptr, 0);
        else if (!std::strcmp(argv[i], "--watch")) g_watched[u32(std::strtoul(argv[i + 1], nullptr, 0))] = 0;
        else if (!std::strcmp(argv[i], "--trace")) g_traced.insert(u32(std::strtoul(argv[i + 1], nullptr, 0)));
        else if (!std::strcmp(argv[i], "--dump-tick")) g_dump_ticks.insert(std::strtoull(argv[i + 1], nullptr, 0));
        else if (!std::strcmp(argv[i], "--dump-step")) g_dump_steps.insert(std::strtoull(argv[i + 1], nullptr, 0));
        else if (!std::strcmp(argv[i], "--frame-range")) {
            unsigned long long from = 0, to = 0;
            if (std::sscanf(argv[i + 1], "%llu:%llu", &from, &to) == 2) g_frame_ranges.push_back({from, to});
        }
        else if (!std::strcmp(argv[i], "--wav")) g_wav_path = argv[i + 1];
        else if (!std::strcmp(argv[i], "--net-host")) { net.role = NetRole::Host; net.port = u16(std::atoi(argv[i + 1])); }
        else if (!std::strcmp(argv[i], "--net-guest")) { net.role = NetRole::Guest; net.port = u16(std::atoi(argv[i + 1])); }
        else if (!std::strcmp(argv[i], "--input-delay")) input_delay = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--give-up")) net.give_up_seconds = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--rejoin-file")) net.rejoin_file = argv[i + 1];
        else if (!std::strcmp(argv[i], "--rejoin")) rejoin_from = argv[i + 1];
        else if (!std::strcmp(argv[i], "--force-mode")) g_force_mode = int(std::strtol(argv[i + 1], nullptr, 0));
        else if (!std::strcmp(argv[i], "--end-after-minigame")) g_end_after_minigame = std::atoi(argv[i + 1]) != 0;
        else if (!std::strcmp(argv[i], "--leave-at-tick")) g_leave_tick = std::strtoull(argv[i + 1], nullptr, 0);
        else if (!std::strcmp(argv[i], "--leave-file")) g_leave_file = argv[i + 1];
        else if (!std::strcmp(argv[i], "--exit-frames")) g_exit_frames = argv[i + 1];
        else if (!std::strcmp(argv[i], "--unlock-minigames")) g_unlock_minigames = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--name")) settings.player_name = argv[i + 1];
        else if (!std::strcmp(argv[i], "--language")) settings.language = u8(std::atoi(argv[i + 1]));
        else if (!std::strcmp(argv[i], "--boot-type")) settings.boot_type = u16(std::strtoul(argv[i + 1], nullptr, 0));
        else if (!std::strcmp(argv[i], "--save")) settings.save = read_file(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--input")) add_input(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--repeat")) {
            // FROM:TO:STEP:WHAT[:FRAMES], the same as many --input
            unsigned long long from = 0, to = 0, step = 1;
            int used = 0;
            if (std::sscanf(argv[i + 1], "%llu:%llu:%llu:%n", &from, &to, &step, &used) == 3 && step > 0) {
                for (unsigned long long tick = from; tick <= to; tick += step) {
                    add_input((std::to_string(tick) + ":" + (argv[i + 1] + used)).c_str());
                }
            }
        }
    }

    const std::vector<u8> rom = read_file(argv[1]);
    const std::string problem = machine_check_rom(rom);
    if (!problem.empty()) {
        std::fprintf(stderr, "%s\n", problem.c_str());
        return 2;
    }
    machine_set_abort_hook([](const char*, u32) {
        print_progress();
        print_hot_blocks();
    });
    net.name = settings.player_name;
    if (!rejoin_from.empty()) {
        const std::string keep_file = net.rejoin_file;
        if (!net_load_rejoin(rejoin_from, net)) {
            std::printf("rejoin: cannot read %s\n", rejoin_from.c_str());
            return 3;
        }
        if (net.rejoin_file.empty()) net.rejoin_file = keep_file.empty() ? rejoin_from : keep_file;
        settings.boot_type = 2;
        settings.player_name = net.name;
    } else if (!lobby_options.server.empty()) {
        const int result = run_lobby(lobby_options, settings.player_name, net, settings);
        if (result != 0) return result;
        if (lobby_options.exit_after_start) return 0;
    }
    if (input_delay >= 0) net.input_delay = input_delay;
    machine_boot(rom, settings);
    net_start(net);
    const bool net_running = net.role != NetRole::Off;

    Cpu& c = machine_cpu();
    u64 blocks = 0;
    u64 last_dumped_tick = ~0ull;
    u64 last_seen_step = 0;
    u64 last_frame_tick = ~0ull;
    while (c.pc != stop) {
        if (blocks >= max_blocks) {
            std::printf("stopped: block limit, %llu frames, line %u, halted %d\n",
                        static_cast<unsigned long long>(g_hw.frames), g_hw.line, g_hw.halted);
            print_progress();
            print_watched();
            std::printf("sound: %llu notes, %u players and %u channels active, %llu commands not built\n",
                        static_cast<unsigned long long>(sound_stats().notes), sound_stats().active_players,
                        sound_stats().active_channels, static_cast<unsigned long long>(sound_stats().unknown_commands));
            write_wav();
            std::printf("net: %llu steps, %llu blocks, %u players, last checksum %08x\n",
                        static_cast<unsigned long long>(net_stats().steps), static_cast<unsigned long long>(net_stats().blocks), net_stats().players, net_stats().checksum);
            std::printf("pc=%08x cpsr=%08x lr=%08x sp=%08x stack:", c.pc, c.cpsr(), c.r[14], c.r[13]);
            for (u32 i = 0, shown = 0; i < 128 && shown < 12; ++i) {
                const u32 value = rd32(c.r[13] + i * 4);
                if (value >= 0x02000000 && value < 0x021A5000 && (value & 3) == 0) {
                    std::printf(" %08x", value);
                    ++shown;
                }
            }
            std::printf("\n");
            print_hot_blocks();
            return 4;
        }
        static u64 leave_checked = 0;
        bool leave_now = g_leave_tick != 0 && g_game_ticks >= g_leave_tick;
        if (!g_leave_file.empty() && g_hw.frames >= leave_checked + 60) {
            leave_checked = g_hw.frames;
            leave_now = leave_now || std::filesystem::exists(g_leave_file);
        }
        if (leave_now) {
            net_leave();
            std::printf("left the game at game tick %llu, step %llu\n", static_cast<unsigned long long>(g_game_ticks),
                        static_cast<unsigned long long>(net_stats().steps));
            return 0;
        }
        if (g_end_after_minigame && g_minigame_ended && net_stats().steps >= g_minigame_end_step + 120) {
            std::printf("minigame ended at step %llu\n", static_cast<unsigned long long>(net_stats().steps));
            std::printf("net: %llu steps, %llu blocks, %u players, last checksum %08x\n",
                        static_cast<unsigned long long>(net_stats().steps), static_cast<unsigned long long>(net_stats().blocks),
                        net_stats().players, net_stats().checksum);
            return 0;
        }
        ++blocks;
        apply_forced_mode(c);
        apply_unlock(c);
        watch_progress(c);
        if (!g_wav_path.empty() && (blocks & 0xFFFF) == 0) collect_sound();
        // Once per tick: while the CPU waits for an interrupt, pc stays at the tick start.
        if (net_stats().steps != last_seen_step) {
            last_seen_step = net_stats().steps;
            keep_step_frame(last_seen_step);
            if (g_dump_steps.count(last_seen_step) != 0) {
                write_all_dumps(dump + "/step" + std::to_string(last_seen_step));
                std::printf("dumped at step %llu, game tick %llu\n", static_cast<unsigned long long>(last_seen_step),
                            static_cast<unsigned long long>(g_game_ticks));
            }
        }
        if (c.pc == kGameTick && g_dump_ticks.count(g_game_ticks) != 0 && g_game_ticks != last_dumped_tick) {
            last_dumped_tick = g_game_ticks;
            write_all_dumps(dump + "/tick" + std::to_string(g_game_ticks));
            std::printf("dumped at game tick %llu\n", static_cast<unsigned long long>(g_game_ticks));
            gpu3d_print_last_list();
        }
        if (c.pc == kGameTick && g_game_ticks != last_frame_tick) {
            last_frame_tick = g_game_ticks;
            write_frame_if_wanted(dump, g_game_ticks);
        }
        if ((blocks & 0xFFF) == 0) ++g_hot[c.pc];
        if (net_running) {
            watch_network_stall(c, blocks);
            if ((blocks & 0xFFFFF) == 0) print_net_status();
        }
        machine_step();
    }
    std::printf("reached stop address, pc=%08x\n", c.pc);
    if (!dump.empty()) write_all_dumps(dump);
    return 0;
}
