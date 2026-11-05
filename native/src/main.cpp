// The game in a window: two screens one above the other, keyboard, mouse and gamepad.
//
// Usage: MarioPartyDS.exe [rom.nds] [--scale N] [--fullscreen] [--frames N] [--save FILE]
//        [--screenshot FILE.rgba] [--smoke-test]
//
// --frames N       closes after N pictures, for tests
// --screenshot F   writes the last picture to F as raw RGBA, for tests
// --smoke-test     opens the window and closes it again, without a game
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "hw.h"
#include "machine.h"
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

}  // namespace

int main(int argc, char** argv) {
    std::string rom_path, save_arg, screenshot;
    int scale = 2;
    bool fullscreen = false, smoke_test = false;
    long long frame_limit = -1;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--smoke-test") smoke_test = true;
        else if (arg == "--fullscreen") fullscreen = true;
        else if (arg == "--scale" && i + 1 < argc) scale = std::clamp(std::atoi(argv[++i]), 1, 8);
        else if (arg == "--frames" && i + 1 < argc) frame_limit = std::atoll(argv[++i]);
        else if (arg == "--save" && i + 1 < argc) save_arg = argv[++i];
        else if (arg == "--screenshot" && i + 1 < argc) screenshot = argv[++i];
        else if (arg.rfind("--", 0) != 0) rom_path = arg;
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        std::printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
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

    const std::filesystem::path data_folder = log_path();
    FILE* log = std::freopen((data_folder / "MarioPartyDS.log").string().c_str(), "w", stdout);
    (void)log;
    machine_set_abort_hook(on_abort);

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

    // The game's own save stays with this program; a save from an emulator is never touched.
    const std::filesystem::path save_path =
        save_arg.empty() ? data_folder / "MarioPartyDS.sav" : std::filesystem::u8path(save_arg);
    MachineSettings settings;
    settings.save = read_file(save_path);
    machine_boot(rom, settings);

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
        if (mouse_touch) hw_set_touch(true, u32(logical_x), u32(logical_y) - kScreenHeight);
        else if (g_pad.touching) hw_set_touch(true, u32(g_pad.pointer_x), u32(g_pad.pointer_y));
        else hw_set_touch(false, 0, 0);

        machine_run_frame();
        ++frames;

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
    SDL_Quit();
    return 0;
}
