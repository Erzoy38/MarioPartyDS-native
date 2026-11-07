// The game in a window: two screens one above the other, keyboard, mouse and gamepad.
//
// Usage: MarioPartyDS.exe [rom.nds] [--scale N] [--fullscreen] [--frames N] [--save FILE]
//        [--screenshot FILE.rgba] [--smoke-test] [--local] [--menu]
//
// Starts with the menu (local game, online play, settings) unless --local or --frames is given.
// --frames N       closes after N pictures, for tests; starts the local game directly
// --screenshot F   writes the last picture to F as raw RGBA, for tests
// --smoke-test     opens the window and closes it again, without a game
// --local          the local game, without the menu
// --menu           the menu even with --frames
// --settings FILE  settings from FILE instead of settings.ini in the program's data folder
// In the game, Esc asks whether to go back to the main menu.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "hw.h"
#include "launcher.h"
#include "machine.h"
#include "net.h"
#include "ppu.h"
#include "sound.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr int kWidth = kScreenWidth;
constexpr int kHeight = kScreenHeight * 2;
constexpr double kFramesPerSecond = 59.8261;  // the DS's picture rate

// DS key bits, in the order the hardware uses.
enum Key : u32 {
    kA = 1, kB = 2, kSelect = 4, kStart = 8, kRight = 0x10, kLeft = 0x20, kUp = 0x40, kDown = 0x80,
    kR = 0x100, kL = 0x200, kX = 0x400, kY = 0x800,
};

u32 keyboard_keys() {
    const bool* state = SDL_GetKeyboardState(nullptr);
    struct Binding { SDL_Scancode scancode; u32 key; };
    static constexpr Binding kBindings[] = {
        {SDL_SCANCODE_X, kA},        {SDL_SCANCODE_Z, kB},      {SDL_SCANCODE_S, kX},
        {SDL_SCANCODE_A, kY},        {SDL_SCANCODE_Q, kL},      {SDL_SCANCODE_W, kR},
        {SDL_SCANCODE_RETURN, kStart}, {SDL_SCANCODE_RSHIFT, kSelect},
        {SDL_SCANCODE_RIGHT, kRight}, {SDL_SCANCODE_LEFT, kLeft},
        {SDL_SCANCODE_UP, kUp},      {SDL_SCANCODE_DOWN, kDown},
    };
    u32 keys = 0;
    for (const Binding& binding : kBindings) {
        if (state[binding.scancode]) keys |= binding.key;
    }
    return keys;
}

// The gamepad works the DS keys, and its right stick moves a pointer for the touch screen.
struct Pad {
    SDL_Gamepad* handle = nullptr;
    float pointer_x = kScreenWidth / 2.0f;
    float pointer_y = kScreenHeight / 2.0f;
    bool pointer_shown = false;
    bool touching = false;
};
Pad g_pad;

constexpr int kStickDeadZone = 12000;

u32 pad_keys() {
    if (g_pad.handle == nullptr) return 0;
    struct Binding { SDL_GamepadButton button; u32 key; };
    // Named by position, as on a Nintendo pad: right is A, bottom is B.
    static constexpr Binding kBindings[] = {
        {SDL_GAMEPAD_BUTTON_EAST, kA},           {SDL_GAMEPAD_BUTTON_SOUTH, kB},
        {SDL_GAMEPAD_BUTTON_NORTH, kX},          {SDL_GAMEPAD_BUTTON_WEST, kY},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kL},  {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kR},
        {SDL_GAMEPAD_BUTTON_START, kStart},      {SDL_GAMEPAD_BUTTON_BACK, kSelect},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kRight}, {SDL_GAMEPAD_BUTTON_DPAD_LEFT, kLeft},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, kUp},       {SDL_GAMEPAD_BUTTON_DPAD_DOWN, kDown},
    };
    u32 keys = 0;
    for (const Binding& binding : kBindings) {
        if (SDL_GetGamepadButton(g_pad.handle, binding.button)) keys |= binding.key;
    }
    const int lx = SDL_GetGamepadAxis(g_pad.handle, SDL_GAMEPAD_AXIS_LEFTX);
    const int ly = SDL_GetGamepadAxis(g_pad.handle, SDL_GAMEPAD_AXIS_LEFTY);
    if (lx > kStickDeadZone) keys |= kRight;
    if (lx < -kStickDeadZone) keys |= kLeft;
    if (ly > kStickDeadZone) keys |= kDown;
    if (ly < -kStickDeadZone) keys |= kUp;
    return keys;
}

void update_pad_pointer() {
    g_pad.touching = false;
    if (g_pad.handle == nullptr) return;
    const int rx = SDL_GetGamepadAxis(g_pad.handle, SDL_GAMEPAD_AXIS_RIGHTX);
    const int ry = SDL_GetGamepadAxis(g_pad.handle, SDL_GAMEPAD_AXIS_RIGHTY);
    constexpr float kPixelsPerFrame = 3.0f / 32767.0f;
    if (std::abs(rx) > kStickDeadZone) { g_pad.pointer_x += rx * kPixelsPerFrame; g_pad.pointer_shown = true; }
    if (std::abs(ry) > kStickDeadZone) { g_pad.pointer_y += ry * kPixelsPerFrame; g_pad.pointer_shown = true; }
    g_pad.pointer_x = std::clamp(g_pad.pointer_x, 0.0f, kScreenWidth - 1.0f);
    g_pad.pointer_y = std::clamp(g_pad.pointer_y, 0.0f, kScreenHeight - 1.0f);
    g_pad.touching = SDL_GetGamepadButton(g_pad.handle, SDL_GAMEPAD_BUTTON_RIGHT_STICK)
                     || SDL_GetGamepadAxis(g_pad.handle, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000;
    if (g_pad.touching) g_pad.pointer_shown = true;
}

std::vector<u8> read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    return std::vector<u8>(std::istreambuf_iterator<char>(file), {});
}

// Writes next to the target first, so a crash never leaves half a save behind.
bool write_save(const std::filesystem::path& path, const std::vector<u8>& data) {
    std::filesystem::path temporary = path;
    temporary += ".new";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        if (!out) return false;
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    return !error;
}

void show_error(const std::string& text) {
    std::printf("error: %s\n", text.c_str());
    std::fflush(stdout);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Mario Party DS", text.c_str(), nullptr);
}

void on_abort(const char* reason, u32 address) {
    char text[200];
    SDL_snprintf(text, sizeof text,
                 "The game stopped: %s at %08x.\nDetails are in MarioPartyDS.log.", reason, address);
    show_error(text);
}

std::string g_dialog_path;
bool g_dialog_done = false;

void SDLCALL on_rom_chosen(void*, const char* const* files, int) {
    if (files != nullptr && files[0] != nullptr) g_dialog_path = files[0];
    g_dialog_done = true;
}

std::string ask_for_rom(SDL_Window* window) {
    static const SDL_DialogFileFilter kFilter[] = {{"Nintendo DS ROM", "nds"}};
    SDL_ShowOpenFileDialog(on_rom_chosen, nullptr, window, kFilter, 1, nullptr, false);
    while (!g_dialog_done) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) return {};
        }
        SDL_Delay(20);
    }
    return g_dialog_path;
}

std::filesystem::path log_path() {
    char* base = SDL_GetPrefPath("MarioPartyDS", "MarioPartyDS");
    std::filesystem::path path = base ? std::filesystem::path(base) : std::filesystem::path(".");
    SDL_free(base);
    return path;
}

void draw_pointer(u8* picture) {
    if (!g_pad.pointer_shown) return;
    const int cx = int(g_pad.pointer_x), cy = int(g_pad.pointer_y) + kScreenHeight;
    for (int i = -3; i <= 3; ++i) {
        for (const auto& [x, y] : {std::pair{cx + i, cy}, std::pair{cx, cy + i}}) {
            if (x < 0 || y < kScreenHeight || x >= kWidth || y >= kHeight) continue;
            u8* pixel = picture + size_t(y) * kWidth * 4 + size_t(x) * 4;
            pixel[0] = g_pad.touching ? 255 : 0;
            pixel[1] = 0;
            pixel[2] = g_pad.touching ? 0 : 255;
        }
    }
}

// While the network waits for the other players: the window keeps answering and says so.
SDL_Window* g_window = nullptr;
SDL_Renderer* g_renderer = nullptr;
SDL_Texture* g_texture = nullptr;
std::string g_rom_path, g_settings_arg;
bool g_wait_asks_to_leave = false;

// Starts this program again with the menu, after the game has ended.
void restart_with_menu(const std::string& rom_path) {
    const char* base = SDL_GetBasePath();
    const std::string exe = std::string(base ? base : "") + "MarioPartyDS.exe";
    std::vector<const char*> args{exe.c_str(), rom_path.c_str(), "--menu"};
    if (!g_settings_arg.empty()) {
        args.push_back("--settings");
        args.push_back(g_settings_arg.c_str());
    }
    args.push_back(nullptr);
    SDL_Process* process = SDL_CreateProcess(args.data(), false);
    if (process == nullptr) std::printf("restart failed: %s\n", SDL_GetError());
    else SDL_DestroyProcess(process);
}

void update_online_title() {
    static Uint64 last = 0;
    const Uint64 now = SDL_GetTicks();
    if (now - last < 1000) return;
    last = now;
    const std::string line = net_status_line();
    if (!line.empty()) SDL_SetWindowTitle(g_window, ("Mario Party DS - online, " + line).c_str());
}

void on_network_wait() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            std::printf("closed while waiting for the other players\n");
            net_leave();
            SDL_Quit();
            std::exit(0);
        }
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
            if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
                g_wait_asks_to_leave = !g_wait_asks_to_leave;
            } else if (g_wait_asks_to_leave && (event.key.scancode == SDL_SCANCODE_RETURN
                                                || event.key.scancode == SDL_SCANCODE_KP_ENTER)) {
                std::printf("left while waiting for the other players\n");
                net_leave();
                restart_with_menu(g_rom_path);
                SDL_Quit();
                std::exit(0);
            }
        }
    }
    update_online_title();
    std::string first, second;
    net_wait_text(first, second);
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);
    SDL_RenderTexture(g_renderer, g_texture, nullptr, nullptr);
    if (g_wait_asks_to_leave) draw_question(g_renderer, "Zurück zum Hauptmenü?", "Enter: ja    Esc: nein");
    else draw_message(g_renderer, first.empty() ? "Warte auf die anderen Spieler ..." : first, second);
    SDL_RenderPresent(g_renderer);
}

// The game is over for this player because of the network: says why, then back to the menu.
void on_network_end(const std::string& text) {
    const Uint64 until = SDL_GetTicks() + 8000;
    while (SDL_GetTicks() < until) {
        SDL_Event event;
        bool done = false;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                SDL_Quit();
                std::exit(0);
            }
            if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                || event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                done = true;
            }
        }
        if (done) break;
        SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
        SDL_RenderClear(g_renderer);
        SDL_RenderTexture(g_renderer, g_texture, nullptr, nullptr);
        draw_message(g_renderer, text, "Weiter mit einer Taste");
        SDL_RenderPresent(g_renderer);
        SDL_Delay(30);
    }
    restart_with_menu(g_rom_path);
    SDL_Quit();
    std::exit(0);
}

}  // namespace

int main(int argc, char** argv) {
    std::string rom_path, save_arg, screenshot, settings_arg;
    int scale = -1;
    bool fullscreen = false, smoke_test = false, local = false, force_menu = false;
    long long frame_limit = -1;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--smoke-test") smoke_test = true;
        else if (arg == "--fullscreen") fullscreen = true;
        else if (arg == "--local") local = true;
        else if (arg == "--menu") force_menu = true;
        else if (arg == "--scale" && i + 1 < argc) scale = std::clamp(std::atoi(argv[++i]), 1, 8);
        else if (arg == "--frames" && i + 1 < argc) frame_limit = std::atoll(argv[++i]);
        else if (arg == "--save" && i + 1 < argc) save_arg = argv[++i];
        else if (arg == "--screenshot" && i + 1 < argc) screenshot = argv[++i];
        else if (arg == "--settings" && i + 1 < argc) settings_arg = argv[++i];
        else if (arg.rfind("--", 0) != 0) rom_path = arg;
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        std::printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    const std::filesystem::path data_folder = log_path();
    const std::filesystem::path settings_file =
        settings_arg.empty() ? data_folder / "settings.ini" : std::filesystem::u8path(settings_arg);
    AppSettings app = load_settings(settings_file);
    if (scale < 0) scale = app.scale;
    fullscreen = fullscreen || app.fullscreen;
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer("Mario Party DS", kWidth * scale, kHeight * scale,
                                     SDL_WINDOW_RESIZABLE, &window, &renderer)) {
        std::printf("window failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetRenderLogicalPresentation(renderer, kWidth, kHeight, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                             SDL_TEXTUREACCESS_STREAMING, kWidth, kHeight);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
    if (fullscreen) SDL_SetWindowFullscreen(window, true);

    if (smoke_test) {
        SDL_RenderClear(renderer);
        SDL_RenderPresent(renderer);
        SDL_Quit();
        return 0;
    }

    // With --settings the log goes next to that file, so test windows do not share one.
    std::filesystem::path log_file = data_folder / "MarioPartyDS.log";
    if (!settings_arg.empty()) log_file = std::filesystem::path(settings_file).replace_extension(".log");
    FILE* log = std::freopen(log_file.string().c_str(), "w", stdout);
    (void)log;
    machine_set_abort_hook(on_abort);

    std::error_code missing;
    if (rom_path.empty() && !app.rom.empty() && std::filesystem::exists(std::filesystem::u8path(app.rom), missing)) {
        rom_path = app.rom;
    }
    if (rom_path.empty()) rom_path = ask_for_rom(window);
    if (rom_path.empty()) {
        SDL_Quit();
        return 0;
    }
    const std::vector<u8> rom = read_file(std::filesystem::u8path(rom_path));
    std::string problem = rom.empty() ? "The ROM could not be read." : machine_check_rom(rom);
    if (!problem.empty()) {
        show_error(problem);
        SDL_Quit();
        return 2;
    }
    if (app.rom != rom_path) {
        app.rom = rom_path;
        save_settings(settings_file, app);
    }

    // The menu: local game, or a room on the lobby server.
    NetSettings net;
    MachineSettings settings;
    // A guest's way back into a game whose program ended without leaving it (closed, crashed).
    std::filesystem::path rejoin_file = data_folder / "rejoin.txt";
    if (!settings_arg.empty()) rejoin_file = std::filesystem::path(settings_file).replace_extension(".rejoin");
    NetSettings rejoin;
    std::string rejoin_room;
    if (net_load_rejoin(rejoin_file.string(), rejoin)) rejoin_room = rejoin.code.empty() ? "direkt" : rejoin.code;
    if (force_menu || (!local && frame_limit < 0)) {
        LaunchChoice choice = run_launcher(window, renderer, app, settings_file, rejoin_room);
        if (choice.kind == LaunchChoice::Kind::Quit) {
            SDL_Quit();
            return 0;
        }
        if (choice.kind == LaunchChoice::Kind::Online) {
            const LobbyStart& start = choice.start;
            net.role = start.slot == 0 ? NetRole::Host : NetRole::Guest;
            net.relay = start.socket;
            net.relay_inbox = start.inbox;
            net.character = start.characters[size_t(start.slot)];
            net.name = app.name;
            net.address = start.address;
            net.port = start.port;
            net.code = start.code;
            net.slot = start.slot;
            net.token = start.token;
            net.tokens = start.tokens;
            net.players = start.players;
            net.input_delay = start.input_delay;
            net.rejoin_file = rejoin_file.string();
            if (net.role == NetRole::Guest) settings.boot_type = 2;
            const std::string title = "Mario Party DS - online, P" + std::to_string(start.slot + 1) + " von " +
                                      std::to_string(start.players);
            SDL_SetWindowTitle(window, title.c_str());
        }
        if (choice.kind == LaunchChoice::Kind::Rejoin) {
            net = rejoin;
            net.rejoin_file = rejoin_file.string();
            settings.boot_type = 2;
            app.name = net.name;
            SDL_SetWindowTitle(window, ("Mario Party DS - online, zurück als P" + std::to_string(net.slot + 1)).c_str());
        }
    }
    settings.player_name = app.name;

    // The game's own save stays with this program; a save from an emulator is never touched.
    const std::filesystem::path save_path =
        save_arg.empty() ? data_folder / "MarioPartyDS.sav" : std::filesystem::u8path(save_arg);
    settings.save = read_file(save_path);
    machine_boot(rom, settings);
    g_window = window;
    g_renderer = renderer;
    g_texture = texture;
    g_rom_path = rom_path;
    g_settings_arg = settings_arg;
    const bool online = net.role != NetRole::Off;
    net_set_wait_callback(on_network_wait);
    net_set_end_callback(on_network_end);
    net_start(net);
    bool asking_to_leave = false, back_to_menu = false;
    {
        // Gamepads that the menu opened came before this loop; take the first one.
        int count = 0;
        SDL_JoystickID* pads = SDL_GetGamepads(&count);
        if (pads != nullptr && count > 0) g_pad.handle = SDL_OpenGamepad(pads[0]);
        SDL_free(pads);
    }

    // Sound: samples are made in step with the game, and the device plays them as they come.
    const SDL_AudioSpec audio_spec{SDL_AUDIO_S16, 2, int(kSoundRate)};
    SDL_AudioStream* audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &audio_spec, nullptr, nullptr);
    if (audio != nullptr) SDL_ResumeAudioStreamDevice(audio);
    else std::printf("no sound device: %s\n", SDL_GetError());
    std::vector<s16> samples;

    const u64 ticks_per_frame = u64(double(SDL_GetPerformanceFrequency()) / kFramesPerSecond);
    u64 next_frame = SDL_GetPerformanceCounter();
    long long frames = 0;
    int frames_since_change = -1;
    bool running = true;
    std::vector<u8> picture(size_t(kWidth) * kHeight * 4);

    while (running && frame_limit != frames) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) running = false;
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                if (event.key.scancode == SDL_SCANCODE_F11) {
                    fullscreen = !fullscreen;
                    SDL_SetWindowFullscreen(window, fullscreen);
                } else if (event.key.scancode == SDL_SCANCODE_F10 && !fullscreen) {
                    scale = scale % 4 + 1;
                    SDL_SetWindowSize(window, kWidth * scale, kHeight * scale);
                } else if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
                    asking_to_leave = !asking_to_leave;
                } else if (asking_to_leave && (event.key.scancode == SDL_SCANCODE_RETURN
                                               || event.key.scancode == SDL_SCANCODE_KP_ENTER)) {
                    back_to_menu = true;
                    running = false;
                }
            }
            if (event.type == SDL_EVENT_GAMEPAD_ADDED && g_pad.handle == nullptr) {
                g_pad.handle = SDL_OpenGamepad(event.gdevice.which);
            }
            if (event.type == SDL_EVENT_GAMEPAD_REMOVED && g_pad.handle != nullptr
                && SDL_GetGamepadID(g_pad.handle) == event.gdevice.which) {
                SDL_CloseGamepad(g_pad.handle);
                g_pad.handle = nullptr;
                g_pad.pointer_shown = false;
            }
        }
        update_pad_pointer();

        float mouse_x = 0, mouse_y = 0;
        const SDL_MouseButtonFlags buttons = SDL_GetMouseState(&mouse_x, &mouse_y);
        float logical_x = 0, logical_y = 0;
        SDL_RenderCoordinatesFromWindow(renderer, mouse_x, mouse_y, &logical_x, &logical_y);
        const bool mouse_touch = (buttons & SDL_BUTTON_LMASK) && logical_x >= 0 && logical_x < kWidth
                                 && logical_y >= kScreenHeight && logical_y < kHeight;
        hw_set_keys(keyboard_keys() | pad_keys());
        // Blowing into the microphone: a key, or the left trigger.
        hw_set_mic(SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_SPACE]
                   || (g_pad.handle != nullptr
                       && SDL_GetGamepadAxis(g_pad.handle, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000));
        u32 auto_x = 0, auto_y = 0;
        if (net_auto_touch(auto_x, auto_y)) hw_set_touch(true, auto_x, auto_y);  // the lobby's character
        else if (mouse_touch) hw_set_touch(true, u32(logical_x), u32(logical_y) - kScreenHeight);
        else if (g_pad.touching) hw_set_touch(true, u32(g_pad.pointer_x), u32(g_pad.pointer_y));
        else hw_set_touch(false, 0, 0);
        if (asking_to_leave) {  // the question takes the keys; the game goes on (others may be playing)
            hw_set_keys(0);
            hw_set_touch(false, 0, 0);
        }

        machine_run_frame();
        ++frames;
        if (online) update_online_title();
        // Back in a game: the game so far is played again as fast as it goes, with a note now and then.
        if (online && net_catching_up()) {
            static Uint64 last_note = 0;
            if (SDL_GetTicks() - last_note >= 250) {
                last_note = SDL_GetTicks();
                std::string first, second;
                net_wait_text(first, second);
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                SDL_RenderClear(renderer);
                draw_message(renderer, first, second);
                SDL_RenderPresent(renderer);
            }
            sound_take_samples(samples);
            next_frame = SDL_GetPerformanceCounter();
            continue;
        }

        sound_take_samples(samples);
        if (audio != nullptr && !samples.empty()) {
            // Over a quarter of a second behind means the game ran ahead: drop rather than lag.
            const int queued = SDL_GetAudioStreamQueued(audio);
            if (queued < int(kSoundRate)) {
                SDL_PutAudioStreamData(audio, samples.data(), int(samples.size() * sizeof(s16)));
            }
        }

        // Writes shortly after the game stops changing its save, not in the middle of a save.
        if (machine_save_changed()) frames_since_change = 0;
        else if (frames_since_change >= 0 && ++frames_since_change >= 60) {
            if (!write_save(save_path, machine_save())) show_error("The save could not be written.");
            frames_since_change = -1;
        }

        std::memcpy(picture.data(), ppu_frame(), picture.size());
        draw_pointer(picture.data());
        SDL_UpdateTexture(texture, nullptr, picture.data(), kWidth * 4);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        SDL_RenderTexture(renderer, texture, nullptr, nullptr);
        if (asking_to_leave) draw_question(renderer, "Zurück zum Hauptmenü?", "Enter: ja    Esc: nein");
        SDL_RenderPresent(renderer);

        next_frame += ticks_per_frame;
        const u64 now = SDL_GetPerformanceCounter();
        if (now < next_frame) {
            SDL_DelayPrecise((next_frame - now) * 1000000000ull / SDL_GetPerformanceFrequency());
        } else if (now - next_frame > ticks_per_frame * 4) {
            next_frame = now;  // too far behind to catch up
        }
    }

    if (frames_since_change >= 0 && !write_save(save_path, machine_save())) {
        show_error("The save could not be written.");
    }
    if (!screenshot.empty()) {
        std::ofstream out(screenshot, std::ios::binary);
        out.write(reinterpret_cast<const char*>(ppu_frame()), std::streamsize(picture.size()));
    }
    std::printf("frames %lld, ended normally\n", frames);
    // Online, the others are told; the machine cannot be booted twice in one process, so the menu
    // comes back as a new one.
    if (online) net_leave();
    if (back_to_menu) restart_with_menu(rom_path);
    SDL_Quit();
    return 0;
}
