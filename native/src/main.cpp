// The game in a window: two screens one above the other, keyboard, mouse and gamepad.
//
// Usage: MarioPartyDS.exe [rom.nds] [--scale N] [--fullscreen] [--frames N] [--save FILE]
//        [--screenshot FILE.rgba] [--smoke-test] [--local] [--menu]
//
// Starts with the start screen (start or choose the ROM, settings, online play) unless --local or
// --frames is given; the ROM is only needed from there on.
// --frames N       closes after N pictures, for tests; starts the local game directly
// --screenshot F   writes the last picture to F as raw RGBA, for tests
// --smoke-test     opens the window and closes it again, without a game
// --local          the local game, without the menu
// --menu           the menu even with --frames
// --settings FILE  settings from FILE instead of settings.ini in the program's data folder
// --force-mode M   for tests: straight from the title into mode M (for example 0x31, a minigame)
// In the game, Esc asks whether to go back to the main menu.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "display.h"
#include "dlss.h"
#include "gpu3d.h"
#include "hw.h"
#include "launcher.h"
#include "machine.h"
#include "microphone.h"
#include "rom_mods.h"
#include "upscale.h"
#include "net.h"
#include "ppu.h"
#include "sound.h"
#include "start_screen.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <string>
#include <vector>

namespace {

constexpr int kWidth = kScreenWidth;
constexpr int kHeight = kScreenHeight * 2;
constexpr int kStartWindowWidth = 1280;  // the start screen's window, 16:9
constexpr int kStartWindowHeight = 720;
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
bool g_select_swaps = false;  // the gamepad's Select swaps the screens (hybrid layout)

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
        // In the hybrid layout Select swaps the screens instead.
        if (binding.button == SDL_GAMEPAD_BUTTON_BACK && g_select_swaps) continue;
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
    static Uint64 last_used = 0;
    const Uint64 now = SDL_GetTicks();
    if (std::abs(rx) > kStickDeadZone) { g_pad.pointer_x += rx * kPixelsPerFrame; last_used = now; }
    if (std::abs(ry) > kStickDeadZone) { g_pad.pointer_y += ry * kPixelsPerFrame; last_used = now; }
    g_pad.pointer_x = std::clamp(g_pad.pointer_x, 0.0f, kScreenWidth - 1.0f);
    g_pad.pointer_y = std::clamp(g_pad.pointer_y, 0.0f, kScreenHeight - 1.0f);
    g_pad.touching = SDL_GetGamepadButton(g_pad.handle, SDL_GAMEPAD_BUTTON_RIGHT_STICK)
                     || SDL_GetGamepadAxis(g_pad.handle, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000;
    if (g_pad.touching) last_used = now;
    // The pointer shows while it is used and for 3 seconds after, then it is out of the way.
    g_pad.pointer_shown = last_used != 0 && now - last_used < 3000;
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

// The gamepad's pointer, in a picture of (256 x 384) times `s`.
void draw_pointer(u8* picture, int s) {
    if (!g_pad.pointer_shown) return;
    const int w = kWidth * s, h = kHeight * s;
    const int cx = int(g_pad.pointer_x * float(s)), cy = int((g_pad.pointer_y + kScreenHeight) * float(s));
    for (int i = -3 * s; i <= 3 * s; ++i) {
        for (int t = 0; t < s; ++t) {
            for (const auto& [x, y] : {std::pair{cx + i, cy + t}, std::pair{cx + t, cy + i}}) {
                if (x < 0 || y < kScreenHeight * s || x >= w || y >= h) continue;
                u8* pixel = picture + (size_t(y) * size_t(w) + size_t(x)) * 4;
                pixel[0] = g_pad.touching ? 255 : 0;
                pixel[1] = 0;
                pixel[2] = g_pad.touching ? 0 : 255;
                pixel[3] = 255;
            }
        }
    }
}

// While the network waits for the other players: the window keeps answering and says so.
SDL_Window* g_window = nullptr;
SDL_Renderer* g_renderer = nullptr;
SDL_Texture* g_texture = nullptr;
std::string g_rom_path, g_settings_arg;
bool g_wait_asks_to_leave = false;
int g_layout = kLayoutStacked, g_aspect = kAspect4x3;
ScreenRects g_rects{};
bool g_swapped = false;     // the top screen where the bottom one goes and the other way round (Tab)
SDL_FRect g_touch_rect{};   // where the touch screen is shown

// The 3D layer: the screen's high-resolution 3D picture, drawn on the graphics card between the
// frame (below) and the frame's 2D parts (above, see-through where the game shows only 3D).
SDL_Texture* g_texture_3d = nullptr;  // null: the frame alone, as one picture
Ppu3dPlacement g_placement{};
bool g_has_3d = false;
std::string g_grab;  // --grab FILE.bmp
std::vector<std::pair<long long, u32>> g_presses;  // --press FRAME:KEY

u32 scripted_keys(long long frame) {
    u32 keys = 0;
    for (const auto& [at, mask] : g_presses) {
        if (frame >= at && frame < at + 6) keys |= mask;
    }
    return keys;
}

// One DS screen at `place`: the frame below, the 3D picture, the frame's 2D parts on top.
void draw_screen_layers(const SDL_FRect& frame_part, int screen, const SDL_FRect& place) {
    if (g_texture_3d == nullptr) {
        SDL_RenderTexture(g_renderer, g_texture, &frame_part, &place);
        return;
    }
    SDL_SetTextureBlendMode(g_texture, SDL_BLENDMODE_NONE);
    SDL_RenderTexture(g_renderer, g_texture, &frame_part, &place);
    if (g_has_3d && g_placement.screen == screen) {
        float w3 = 0, h3 = 0;
        SDL_GetTextureSize(g_texture_3d, &w3, &h3);
        const float k = w3 / float(kScreenWidth);  // 3D texels per DS pixel
        const u32 s = g_placement.scroll & 0x1FF;
        // Screen columns [first, first + count) show 3D columns from `from` on.
        const float first = s < 256 ? 0.0f : float(512 - s);
        const float count = s < 256 ? float(256 - s) : float(s - 256);
        const float from = s < 256 ? float(s) : 0.0f;
        if (count > 0) {
            const SDL_FRect src{from * k, 0, count * k, h3};
            const SDL_FRect dst{place.x + first / 256.0f * place.w, place.y, count / 256.0f * place.w, place.h};
            SDL_RenderTexture(g_renderer, g_texture_3d, &src, &dst);
        }
    }
    SDL_SetTextureBlendMode(g_texture, SDL_BLENDMODE_BLEND);
    SDL_RenderTexture(g_renderer, g_texture, &frame_part, &place);
}

// Both screens where the layout puts them; afterwards drawing is in the DS's 256 x 384 again, for
// the boxes over the game.
void draw_screens() {
    SDL_SetRenderLogicalPresentation(g_renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);
    int ow = 0, oh = 0;
    SDL_GetCurrentRenderOutputSize(g_renderer, &ow, &oh);
    g_rects = screen_rects(float(ow), float(oh), g_layout, g_aspect);
    float tw = 0, th = 0;
    SDL_GetTextureSize(g_texture, &tw, &th);
    const SDL_FRect top{0, 0, tw, th / 2}, bottom{0, th / 2, tw, th / 2};
    const SDL_FRect& top_place = g_swapped ? g_rects.bottom : g_rects.top;
    const SDL_FRect& bottom_place = g_swapped ? g_rects.top : g_rects.bottom;
    draw_screen_layers(top, 0, top_place);
    draw_screen_layers(bottom, 1, bottom_place);
    g_touch_rect = bottom_place;
    SDL_SetRenderLogicalPresentation(g_renderer, kWidth, kHeight, SDL_LOGICAL_PRESENTATION_LETTERBOX);
}

// --compare-compose: draws the layers into a picture of (256 x 384) times `s`, reads it back and
// counts the pixels that differ from the processor's composition. Logged; for checking.
void compare_compose(int s) {
    std::vector<u8> cpu;
    int scale = 1;
    if (!ppu_compose_display(cpu, scale) || scale != s) return;
    const int w = kWidth * s, h = kHeight * s;
    SDL_Texture* target = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, w, h);
    if (target == nullptr) return;
    SDL_Texture* previous = SDL_GetRenderTarget(g_renderer);
    SDL_SetRenderTarget(g_renderer, target);
    SDL_SetRenderLogicalPresentation(g_renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);
    float tw = 0, th = 0;
    SDL_GetTextureSize(g_texture, &tw, &th);
    draw_screen_layers({0, 0, tw, th / 2}, 0, {0, 0, float(w), float(h) / 2});
    draw_screen_layers({0, th / 2, tw, th / 2}, 1, {0, float(h) / 2, float(w), float(h) / 2});
    SDL_Surface* read = SDL_RenderReadPixels(g_renderer, nullptr);
    SDL_SetRenderTarget(g_renderer, previous);
    SDL_SetRenderLogicalPresentation(g_renderer, kWidth, kHeight, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_DestroyTexture(target);
    if (read == nullptr) return;
    SDL_Surface* rgba = SDL_ConvertSurface(read, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(read);
    if (rgba == nullptr) return;
    long long differ = 0;
    for (int y = 0; y < h; ++y) {
        const u8* g = static_cast<const u8*>(rgba->pixels) + size_t(y) * size_t(rgba->pitch);
        const u8* c = &cpu[size_t(y) * size_t(w) * 4];
        for (int x = 0; x < w; ++x) {
            if (g[x * 4] != c[x * 4] || g[x * 4 + 1] != c[x * 4 + 1] || g[x * 4 + 2] != c[x * 4 + 2]) ++differ;
        }
    }
    SDL_DestroySurface(rgba);
    std::printf("compare compose: %lld of %d pixels differ\n", differ, w * h);
}

// The frame (RGBA, (256 x 384) times `f`) see-through where the game shows only 3D.
void mask_pure_3d(std::vector<u8>& frame, int f) {
    const u16* map = ppu_frame_map();
    const size_t w = size_t(kWidth) * size_t(f);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const u8 alpha = map[size_t(y) * kWidth + size_t(x)] == 0xFFFF ? 255 : 0;
            for (int j = 0; j < f; ++j) {
                u8* p = &frame[((size_t(y) * size_t(f) + size_t(j)) * w + size_t(x) * size_t(f)) * 4 + 3];
                for (int i = 0; i < f; ++i, p += 4) *p = alpha;
            }
        }
    }
}

// The mouse on the touch screen, in DS pixels; false when it is elsewhere.
bool mouse_on_touch_screen(float window_x, float window_y, float& x, float& y) {
    int ww = 0, wh = 0, ow = 0, oh = 0;
    SDL_GetWindowSize(g_window, &ww, &wh);
    SDL_GetCurrentRenderOutputSize(g_renderer, &ow, &oh);
    if (ww <= 0 || wh <= 0) return false;
    const float px = window_x * float(ow) / float(ww), py = window_y * float(oh) / float(wh);
    const SDL_FRect& r = g_touch_rect;
    if (r.w <= 0 || r.h <= 0 || px < r.x || py < r.y || px >= r.x + r.w || py >= r.y + r.h) return false;
    x = (px - r.x) / r.w * float(kScreenWidth);
    y = (py - r.y) / r.h * float(kScreenHeight);
    return true;
}

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
            dlss_stop();  // before SDL takes the Direct3D device away
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
                dlss_stop();  // before SDL takes the Direct3D device away
                SDL_Quit();
                std::exit(0);
            }
        }
    }
    update_online_title();
    std::fflush(stdout);
    std::string first, second;
    net_wait_text(first, second);
            draw_screens();
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
                dlss_stop();  // before SDL takes the Direct3D device away
                SDL_Quit();
                std::exit(0);
            }
            if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                || event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                done = true;
            }
        }
        if (done) break;
                        draw_screens();
        draw_message(g_renderer, text, "Weiter mit einer Taste");
        SDL_RenderPresent(g_renderer);
        SDL_Delay(30);
    }
    restart_with_menu(g_rom_path);
    dlss_stop();  // before SDL takes the Direct3D device away
    SDL_Quit();
    std::exit(0);
}

}  // namespace

int main(int argc, char** argv) {
    std::string rom_path, save_arg, screenshot, settings_arg;
    int scale = -1;
    bool fullscreen = false, smoke_test = false, local = false, force_menu = false;
    bool cpu_compose = false;  // --cpu-compose: the frame put together on the processor, as before
    int compare_every = 0;     // --compare-compose N: every N pictures, graphics card against processor
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
        else if (arg == "--grab" && i + 1 < argc) g_grab = argv[++i];
        else if (arg == "--press" && i + 1 < argc) {  // FRAME:KEY, the key held for 6 frames (tests)
            static const char* const kNames[] = {"A", "B", "SELECT", "START", "RIGHT", "LEFT",
                                                 "UP", "DOWN", "R", "L", "X", "Y"};
            long long at = 0;
            char what[16] = {};
            if (std::sscanf(argv[++i], "%lld:%15s", &at, what) == 2) {
                for (u32 k = 0; k < 12; ++k) {
                    if (!std::strcmp(what, kNames[k])) g_presses.push_back({at, 1u << k});
                }
            }
        }
        else if (arg == "--settings" && i + 1 < argc) settings_arg = argv[++i];
        else if (arg == "--cpu-compose") cpu_compose = true;
        else if (arg == "--compare-compose" && i + 1 < argc) compare_every = std::atoi(argv[++i]);
        else if (arg == "--force-mode" && i + 1 < argc) machine_force_mode(int(std::strtol(argv[++i], nullptr, 0)));
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
    const bool scale_given = scale >= 0;
    if (!scale_given) scale = app.scale;
    fullscreen = fullscreen || app.fullscreen;
    const bool use_menu = force_menu || (!local && frame_limit < 0 && !smoke_test);
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    const int first_w = use_menu ? kStartWindowWidth : kWidth * scale;
    const int first_h = use_menu ? kStartWindowHeight : kHeight * scale;
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "direct3d11");
    if (!SDL_CreateWindowAndRenderer("Mario Party DS", first_w, first_h, SDL_WINDOW_RESIZABLE, &window, &renderer)) {
        SDL_ResetHint(SDL_HINT_RENDER_DRIVER);  // no Direct3D 11: whatever SDL has
        if (!SDL_CreateWindowAndRenderer("Mario Party DS", first_w, first_h, SDL_WINDOW_RESIZABLE, &window, &renderer)) {
            std::printf("window failed: %s\n", SDL_GetError());
            return 1;
        }
    }
    SDL_SetRenderLogicalPresentation(renderer, kWidth, kHeight, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    const auto in_window = [&] { return (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) == 0; };
    // The start screen is landscape, the game the DS's two screens one above the other.
    const auto start_layout = [&] {
        SDL_SetRenderLogicalPresentation(renderer, kStartWidth, kStartHeight, SDL_LOGICAL_PRESENTATION_LETTERBOX);
        if (in_window()) {
            SDL_SetWindowSize(window, kStartWindowWidth, kStartWindowHeight);
            SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        }
    };
    // The online menu is drawn in the DS's 256 x 384.
    const auto menu_layout = [&] {
        SDL_SetRenderLogicalPresentation(renderer, kWidth, kHeight, SDL_LOGICAL_PRESENTATION_LETTERBOX);
        if (in_window()) {
            SDL_SetWindowSize(window, kWidth * scale, kHeight * scale);
            SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        }
    };
    // The game: resolution, layout and shape from the settings.
    const auto game_layout = [&] {
        SDL_SetRenderLogicalPresentation(renderer, kWidth, kHeight, SDL_LOGICAL_PRESENTATION_LETTERBOX);
        apply_window_mode(window, app, scale, fullscreen);
    };
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                             SDL_TEXTUREACCESS_STREAMING, kWidth, kHeight);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
    if (fullscreen) SDL_SetWindowFullscreen(window, true);

    if (smoke_test) {
        SDL_RenderClear(renderer);
        SDL_RenderPresent(renderer);
        dlss_stop();  // before SDL takes the Direct3D device away
        SDL_Quit();
        return 0;
    }

    // With --settings the log goes next to that file, so test windows do not share one.
    std::filesystem::path log_file = data_folder / "MarioPartyDS.log";
    if (!settings_arg.empty()) log_file = std::filesystem::path(settings_file).replace_extension(".log");
    FILE* log = std::freopen(log_file.string().c_str(), "w", stdout);
    (void)log;
    machine_set_abort_hook(on_abort);
    // NVIDIA DLSS, where the card has it; the settings say why not otherwise.
    if (!dlss_start(renderer, data_folder)) std::printf("dlss: not available: %s\n", dlss_reason().c_str());

    std::error_code missing;
    if (rom_path.empty() && !app.rom.empty() && std::filesystem::exists(std::filesystem::u8path(app.rom), missing)) {
        rom_path = app.rom;
    }
    // The ROM's bytes after the last check that passed.
    std::vector<u8> rom;
    const std::function<std::string(const std::string&)> check_rom = [&](const std::string& path) {
        std::vector<u8> data = read_file(std::filesystem::u8path(path));
        std::string problem = data.empty() ? "The file could not be read." : machine_check_rom(data);
        if (problem.empty()) rom = std::move(data);
        return problem;
    };

    NetSettings net;
    MachineSettings settings;
    // A guest's way back into a game whose program ended without leaving it (closed, crashed).
    std::filesystem::path rejoin_file = data_folder / "rejoin.txt";
    if (!settings_arg.empty()) rejoin_file = std::filesystem::path(settings_file).replace_extension(".rejoin");
    NetSettings rejoin;
    std::string rejoin_room;
    if (net_load_rejoin(rejoin_file.string(), rejoin)) rejoin_room = rejoin.code.empty() ? "direkt" : rejoin.code;
    if (!use_menu) {
        // Straight into the game (tests, --local): the ROM from the command line, the settings or a dialog.
        if (rom_path.empty()) rom_path = ask_for_rom(window);
        if (rom_path.empty()) {
            dlss_stop();  // before SDL takes the Direct3D device away
            SDL_Quit();
            return 0;
        }
        const std::string problem = check_rom(rom_path);
        if (!problem.empty()) {
            show_error(problem);
            dlss_stop();  // before SDL takes the Direct3D device away
            SDL_Quit();
            return 2;
        }
        if (app.rom != rom_path) {
            app.rom = rom_path;
            save_settings(settings_file, app);
        }
    }
    // The start screen; online play goes on in the lobby menu, which can come back here.
    while (use_menu) {
        start_layout();
        const StartChoice picked = run_start_screen(window, renderer, app, settings_file, rom_path, check_rom, rejoin_room, "");
        if (picked.kind == StartChoice::Kind::Quit) {
            dlss_stop();  // before SDL takes the Direct3D device away
            SDL_Quit();
            return 0;
        }
        if (picked.kind == StartChoice::Kind::Local) break;
        if (picked.kind == StartChoice::Kind::Rejoin) {
            net = rejoin;
            net.rejoin_file = rejoin_file.string();
            settings.boot_type = 2;
            app.name = net.name;
            SDL_SetWindowTitle(window, ("Mario Party DS - online, zurück als P" + std::to_string(net.slot + 1)).c_str());
            break;
        }
        if (!scale_given) scale = app.scale;
        menu_layout();
        LaunchChoice choice = run_launcher(window, renderer, app, settings_file);
        if (choice.kind == LaunchChoice::Kind::Quit) {
            dlss_stop();  // before SDL takes the Direct3D device away
            SDL_Quit();
            return 0;
        }
        if (choice.kind == LaunchChoice::Kind::Back) continue;
        {
            const LobbyStart& start = choice.start;
            net.role = start.slot == 0 ? NetRole::Host : NetRole::Guest;
            net.relay = start.socket;
            net.relay_inbox = start.inbox;
            net.relay_channel = start.channel;
            net.relay_outbox = start.outbox;
            net.server_key = start.server_key;
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
        break;
    }
    if (use_menu) {
        // The settings may have changed on the start screen.
        if (!scale_given) scale = app.scale;
        fullscreen = app.fullscreen;
        game_layout();
        if (app.rom != rom_path) {  // a ROM given on the command line is remembered too
            app.rom = rom_path;
            save_settings(settings_file, app);
        }
    }
    if (!use_menu) game_layout();
    g_layout = app.layout;
    g_aspect = app.aspect;
    g_select_swaps = app.layout == kLayoutHybrid;
    // The picture for the screen: the 3D at a higher resolution and texture packs, only for showing.
    texture_pack_start(app.render_scale, app.hd_textures, app.dump_textures);
    const bool display_picture = app.render_scale > 1 || app.hd_textures;
    // The upscaler makes the 2D pixels larger to the 3D resolution, or 4 times on its own.
    const bool upscaling = app.upscale != 0 && (!display_picture || app.render_scale > 1);
    // With the 3D layer on the graphics card the frame only needs the upscaler's own size: FSR makes
    // 2x, Scale2x up to 4x; the card enlarges the rest. Without the 3D layer, 4x as before.
    const int shown_scale = display_picture ? (upscaling ? (app.upscale == kUpscaleFsr ? 2 : std::min(app.render_scale, 4)) : 1)
                                            : (upscaling ? 4 : 1);
    // Frame rate: the game always makes 60 pictures; 30 shows every second one, above 60 each picture
    // waits for the display's refresh (VSync).
    if (app.frame_rate > 60) SDL_SetRenderVSync(renderer, 1);
    const int show_every = app.frame_rate == 30 ? 2 : 1;
    // The old way puts the upscaled frame together with the 3D picture at the 3D resolution.
    const int upscale_factor = (cpu_compose && display_picture) ? app.render_scale : shown_scale;
    if (shown_scale != 1) {
        SDL_DestroyTexture(texture);
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, kWidth * shown_scale,
                                    kHeight * shown_scale);
    }
    SDL_SetTextureScaleMode(texture, app.smooth ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
    if (display_picture && !cpu_compose) {
        const int s3 = std::clamp(app.render_scale, 1, 12);
        g_texture_3d = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, kScreenWidth * s3,
                                         kScreenHeight * s3);
        SDL_SetTextureBlendMode(g_texture_3d, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(g_texture_3d, app.smooth ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
    }
    // NVIDIA DLSS: the 3D is drawn at DLSS's smaller size with a moving sample point, and DLSS makes
    // it the 3D resolution again. If anything of it fails, the 3D picture stays as before.
    bool use_dlss = false;
    int dlss_in_w = 0, dlss_in_h = 0;
    const int dlss_out_w = kScreenWidth * std::clamp(app.render_scale, 1, 12);
    const int dlss_out_h = kScreenHeight * std::clamp(app.render_scale, 1, 12);
    if (g_texture_3d != nullptr && app.dlss != kDlssOff && dlss_available()) {
        unsigned w = 0, h = 0;
        if (dlss_render_size(app.dlss, unsigned(dlss_out_w), unsigned(dlss_out_h), w, h)) {
            SDL_Texture* out = dlss_begin(renderer, app.dlss, int(w), int(h), dlss_out_w, dlss_out_h);
            if (out != nullptr) {
                SDL_DestroyTexture(g_texture_3d);
                g_texture_3d = out;
                SDL_SetTextureBlendMode(g_texture_3d, SDL_BLENDMODE_BLEND);
                SDL_SetTextureScaleMode(g_texture_3d, app.smooth ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
                dlss_in_w = int(w);
                dlss_in_h = int(h);
                gpu3d_set_display_size(dlss_in_w, dlss_in_h);
                use_dlss = true;
            }
        }
        if (!use_dlss) std::printf("dlss: not used: %s\n", dlss_reason().c_str());
    }
    if (cpu_compose && display_picture) {  // the old way, for comparing: everything at the 3D resolution
        SDL_DestroyTexture(texture);
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, kWidth * app.render_scale,
                                    kHeight * app.render_scale);
        SDL_SetTextureScaleMode(texture, app.smooth ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
    }
    {
        const char* name = SDL_GetRendererName(renderer);
        std::printf("renderer: %s, %s\n", name ? name : "?",
                    g_texture_3d ? "3D layer on the graphics card" : "frame as one picture");
    }
    settings.player_name = app.name;
    settings.language = u8(app.language);

    // The game's own save stays with this program; a save from an emulator is never touched.
    const std::filesystem::path save_path =
        save_arg.empty() ? data_folder / "MarioPartyDS.sav" : std::filesystem::u8path(save_arg);
    settings.save = read_file(save_path);
    if (app.file_mods) {
        std::string mod_log;
        const int replaced = apply_file_mods(rom, mods_folder(), mod_log);
        std::printf("file mods: %d files replaced\n%s", replaced, mod_log.c_str());
    }
    // Cheats on this player's own game; a guest's game comes from the host and keeps the host's.
    MachineCheats cheats;
    if (net.role != NetRole::Guest) {
        cheats.all_minigames = app.cheat_minigames;
        cheats.all_boards = app.cheat_boards;
    }
    machine_set_cheats(cheats);
    machine_boot(rom, settings);
    g_window = window;
    g_renderer = renderer;
    g_texture = texture;
    g_rom_path = rom_path;
    SDL_SetRenderLogicalPresentation(renderer, kWidth, kHeight, SDL_LOGICAL_PRESENTATION_LETTERBOX);
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
    if (audio != nullptr) {
        SDL_SetAudioStreamGain(audio, float(app.volume) / 100.0f);
        SDL_ResumeAudioStreamDevice(audio);
    } else {
        std::printf("no sound device: %s\n", SDL_GetError());
    }
    // The real microphone: loud enough counts as blowing, like Space or the left trigger.
    Microphone microphone;
    microphone.open(app.mic_device);
    std::vector<s16> samples;

    const u64 ticks_per_frame = u64(double(SDL_GetPerformanceFrequency()) / kFramesPerSecond);
    u64 next_frame = SDL_GetPerformanceCounter();
    long long frames = 0;
    int frames_since_change = -1;
    bool running = true;
    std::vector<u8> picture(size_t(kWidth) * kHeight * 4);
    std::vector<u8> shown;  // the screen's picture, with display_picture or the upscaler
    std::vector<u8> upscaled;  // the game's frame made larger, for the parts without 3D
    std::vector<u8> upscale_work, upscale_source(picture.size());  // the upscaler's own, while it runs
    std::future<void> upscale_job;
    std::vector<u8> layer_3d;  // the 3D picture for the graphics card
    u64 dlss_serial = ~0ull;   // the list DLSS was last given

    while (running && frame_limit != frames) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) running = false;
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                if (event.key.scancode == SDL_SCANCODE_TAB) {
                    g_swapped = !g_swapped;  // the big and the small screen change places
                } else if (event.key.scancode == SDL_SCANCODE_F11) {
                    fullscreen = !fullscreen;
                    apply_window_mode(window, app, scale, fullscreen);
                } else if (event.key.scancode == SDL_SCANCODE_F10 && !fullscreen) {
                    scale = scale % 4 + 1;  // automatic sizes, for this game only
                    app.resolution.clear();
                    apply_window_mode(window, app, scale, false);
                } else if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
                    asking_to_leave = !asking_to_leave;
                } else if (asking_to_leave && (event.key.scancode == SDL_SCANCODE_RETURN
                                               || event.key.scancode == SDL_SCANCODE_KP_ENTER)) {
                    back_to_menu = true;
                    running = false;
                }
            }
            if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && g_select_swaps
                && event.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) {
                g_swapped = !g_swapped;
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
        float touch_x = 0, touch_y = 0;
        const bool mouse_touch = (buttons & SDL_BUTTON_LMASK) && mouse_on_touch_screen(mouse_x, mouse_y, touch_x, touch_y);
        hw_set_keys(keyboard_keys() | pad_keys() | scripted_keys(frames));
        // Blowing into the microphone: the real one, a key, or the left trigger.
        // The keys blow at full strength, the real microphone as loud as it hears.
        microphone.update();
        const bool blowing_key = SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_SPACE]
                                 || (g_pad.handle != nullptr
                                     && SDL_GetGamepadAxis(g_pad.handle, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000);
        hw_set_mic_level(blowing_key ? 1.0f : microphone.game_level(app.mic_sensitivity));
        u32 auto_x = 0, auto_y = 0;
        if (net_auto_touch(auto_x, auto_y)) hw_set_touch(true, auto_x, auto_y);  // the lobby's character
        else if (mouse_touch) hw_set_touch(true, u32(touch_x), u32(touch_y));
        else if (g_pad.touching) hw_set_touch(true, u32(g_pad.pointer_x), u32(g_pad.pointer_y));
        else hw_set_touch(false, 0, 0);
        if (asking_to_leave) {  // the question takes the keys; the game goes on (others may be playing)
            hw_set_keys(0);
            hw_set_touch(false, 0, 0);
        }

        if (use_dlss) {  // the sample point for the 3D drawn in this frame
            float jx = 0, jy = 0;
            dlss_jitter(frames, dlss_in_w, dlss_out_w, jx, jy);
            gpu3d_set_display_jitter(jx, jy);
        }
        machine_run_frame();
        ++frames;
        std::fflush(stdout);  // the log stays complete if the program ends unexpectedly
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

        if (frames % show_every == 0) {
            // The upscaler works on the other cores while the next picture of the game is made; this
            // picture shows what it finished for the last one (1/60 s later, not to be seen).
            if (upscaling) {
                if (upscale_job.valid()) {
                    upscale_job.wait();
                } else {  // the very first picture: made here, before any job runs
                    std::memcpy(upscale_source.data(), ppu_frame(), upscale_source.size());
                    upscale_picture(upscale_source.data(), upscale_factor, app.upscale, upscale_work);
                }
                upscaled.swap(upscale_work);
                std::memcpy(upscale_source.data(), ppu_frame(), upscale_source.size());
                upscale_job = std::async(std::launch::async, [&upscale_source, &upscale_work, upscale_factor, mode = app.upscale] {
                    upscale_picture(upscale_source.data(), upscale_factor, mode, upscale_work);
                });
            }
            if (display_picture && g_texture_3d != nullptr) {
                // The graphics card puts it together: the frame (masked where only 3D shows) and
                // the 3D picture.
                Gpu3dDisplayInfo info;
                const int last_screen = g_has_3d ? g_placement.screen : -1;
                g_has_3d = ppu_3d_layer(layer_3d, info);
                g_placement = ppu_3d_placement();
                if (g_has_3d && use_dlss) {
                    // DLSS starts afresh where the 3D comes back or moves to the other screen. A
                    // picture shown again is not given twice: its motion is that of the first time.
                    const bool reset = last_screen != g_placement.screen;
                    if (info.serial != dlss_serial || reset) {
                        DlssFrame f;
                        f.rgba = layer_3d.data();
                        f.depth = info.depth;
                        f.motion = info.motion;
                        f.fresh = info.fresh;
                        f.jitter_x = info.jitter_x;
                        f.jitter_y = info.jitter_y;
                        f.reset = reset;
                        static const bool report = std::getenv("MPDS_MOTION_STATS") != nullptr;
                        static double eval_ms = 0;
                        static int evals = 0;
                        const Uint64 t0 = SDL_GetPerformanceCounter();
                        g_has_3d = info.w == dlss_in_w && info.h == dlss_in_h && dlss_evaluate(renderer, f);
                        if (report) {
                            eval_ms += double(SDL_GetPerformanceCounter() - t0) * 1000.0 / double(SDL_GetPerformanceFrequency());
                            if (++evals % 60 == 0) {
                                std::printf("timing: dlss upload and evaluate %.2f ms\n", eval_ms / 60);
                                eval_ms = 0;
                            }
                        }
                        dlss_serial = g_has_3d ? info.serial : ~0ull;
                    }
                } else if (g_has_3d) {
                    SDL_UpdateTexture(g_texture_3d, nullptr, layer_3d.data(), info.w * 4);
                }
                if (upscaling) shown = upscaled;
                else shown.assign(ppu_frame(), ppu_frame() + picture.size());
                mask_pure_3d(shown, shown_scale);
                draw_pointer(shown.data(), shown_scale);
                SDL_UpdateTexture(texture, nullptr, shown.data(), kWidth * shown_scale * 4);
                if (compare_every > 0 && frames % compare_every == 0 && !upscaling) compare_compose(app.render_scale);
            } else if (display_picture) {
                int s = 1;
                ppu_compose_display(shown, s, upscaling ? upscaled.data() : nullptr);
                if (s == app.render_scale) {
                    draw_pointer(shown.data(), s);
                    SDL_UpdateTexture(texture, nullptr, shown.data(), kWidth * s * 4);
                }
            } else if (upscaling) {
                shown = upscaled;
                draw_pointer(shown.data(), shown_scale);
                SDL_UpdateTexture(texture, nullptr, shown.data(), kWidth * shown_scale * 4);
            } else {
                std::memcpy(picture.data(), ppu_frame(), picture.size());
                draw_pointer(picture.data(), 1);
                SDL_UpdateTexture(texture, nullptr, picture.data(), kWidth * 4);
            }
            draw_screens();
            if (!g_grab.empty() && frames == frame_limit) {  // --grab: the window's last picture, for checking
                if (SDL_Surface* read = SDL_RenderReadPixels(renderer, nullptr)) {
                    SDL_SaveBMP(read, g_grab.c_str());
                    SDL_DestroySurface(read);
                }
            }
            if (asking_to_leave) draw_question(renderer, "Zurück zum Hauptmenü?", "Enter: ja    Esc: nein");
            SDL_RenderPresent(renderer);
        }

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
    microphone.close();  // before SDL ends, or closing it later reaches into nothing
    dlss_stop();  // before SDL takes the Direct3D device away
    SDL_Quit();
    return 0;
}
