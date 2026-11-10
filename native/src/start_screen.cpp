#include "start_screen.h"

#include "display.h"
#include "microphone.h"
#include "dlss.h"
#include "rom_mods.h"

#include <fstream>
#include "ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

namespace {

using ui::Font;
using ui::Rgba;
using ui::TextStyle;

constexpr float kW = float(kStartWidth);
constexpr float kH = float(kStartHeight);

constexpr Rgba kWhite{255, 255, 255, 255};
constexpr Rgba kInk{24, 16, 46, 255};        // outlines and the chosen item's text
constexpr Rgba kChosen{24, 140, 222, 255};   // the bar behind the chosen item
constexpr Rgba kPanel{28, 52, 172, 232};     // the settings window
constexpr Rgba kPanelEdge{46, 168, 240, 255};
constexpr Rgba kErrorText{255, 150, 150, 255};

const char* const kLanguages[] = {"", "English", "Fran\xC3\xA7" "ais", "Deutsch", "Italiano", "Espa\xC3\xB1ol"};

// --- The file dialog: SDL may answer from another thread ---
struct Dialog {
    std::mutex lock;
    bool open = false;
    bool answered = false;
    std::string path;
};
Dialog g_dialog;

void SDLCALL on_file_chosen(void*, const char* const* files, int) {
    std::lock_guard<std::mutex> guard(g_dialog.lock);
    g_dialog.path = (files != nullptr && files[0] != nullptr) ? files[0] : "";
    g_dialog.answered = true;
}

void open_rom_dialog(SDL_Window* window) {
    static const SDL_DialogFileFilter kFilter[] = {{"Nintendo DS ROM (*.nds)", "nds"}};
    {
        std::lock_guard<std::mutex> guard(g_dialog.lock);
        g_dialog.open = true;
        g_dialog.answered = false;
        g_dialog.path.clear();
    }
    SDL_ShowOpenFileDialog(on_file_chosen, nullptr, window, kFilter, 1, nullptr, false);
}

// --- Input of one picture ---
struct Input {
    bool up = false, down = false, left = false, right = false, activate = false, back = false;
    bool tab_left = false, tab_right = false, backspace = false;
    std::string typed;
    float mouse_x = -1, mouse_y = -1;
    bool moved = false, clicked = false;
};

bool inside(const Input& in, const SDL_FRect& box) {
    return in.mouse_x >= box.x && in.mouse_x < box.x + box.w && in.mouse_y >= box.y && in.mouse_y < box.y + box.h;
}

// A five-pointed star around (x, y), as in the corners of a party logo.
void star(SDL_Renderer* r, float x, float y, float outer, Rgba fill, Rgba edge) {
    auto points = [&](float size) {
        std::vector<SDL_FPoint> p;
        for (int i = 0; i < 10; ++i) {
            const float a = -3.14159265f / 2 + float(i) * 3.14159265f / 5;
            const float d = (i % 2 == 0) ? size : size * 0.45f;
            p.push_back({x + d * std::cos(a), y + d * std::sin(a)});
        }
        return p;
    };
    for (const auto& [size, colour] : {std::pair{outer + 2.5f, edge}, std::pair{outer, fill}}) {
        const std::vector<SDL_FPoint> p = points(size);
        std::vector<SDL_Vertex> v{{{x, y}, {colour.r / 255.f, colour.g / 255.f, colour.b / 255.f, colour.a / 255.f}, {0, 0}}};
        for (const SDL_FPoint& q : p) v.push_back({q, v[0].color, {0, 0}});
        std::vector<int> idx;
        for (int i = 0; i < 10; ++i) {
            idx.push_back(0);
            idx.push_back(i + 1);
            idx.push_back((i + 1) % 10 + 1);
        }
        SDL_RenderGeometry(r, nullptr, v.data(), int(v.size()), idx.data(), int(idx.size()));
    }
}

// Purple stripes, when the background picture is missing.
void stripes(SDL_Renderer* r) {
    ui::fill_vertical_gradient(r, {0, 0, kW, kH}, {92, 48, 150, 255}, {60, 28, 104, 255});
    const SDL_FColor light{1, 1, 1, 0.07f};
    for (float x = -kH; x < kW; x += 64) {
        const SDL_Vertex v[4] = {{{x, kH}, light, {0, 0}}, {{x + 30, kH}, light, {0, 0}},
                                 {{x + 30 + kH, 0}, light, {0, 0}}, {{x + kH, 0}, light, {0, 0}}};
        const int idx[6] = {0, 1, 2, 0, 2, 3};
        SDL_RenderGeometry(r, nullptr, v, 4, idx, 6);
    }
}

std::string file_name(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string build_date() {
    // __DATE__ is "Mmm dd yyyy".
    static const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const std::string d = __DATE__;
    int month = 1;
    for (int i = 0; i < 12; ++i) {
        if (d.compare(0, 3, kMonths[i]) == 0) month = i + 1;
    }
    char text[32];
    SDL_snprintf(text, sizeof text, "%s-%02d-%02d", d.substr(7, 4).c_str(), month, std::atoi(d.substr(4, 2).c_str()));
    return text;
}

class StartScreen {
public:
    StartScreen(SDL_Window* window, SDL_Renderer* renderer, AppSettings& settings, const std::filesystem::path& settings_file,
                std::string& rom_path, const std::function<std::string(const std::string&)>& check_rom,
                const std::string& rejoin_room, const std::string& notice)
        : window_(window), r_(renderer), text_(renderer), settings_(settings), settings_file_(settings_file),
          rom_path_(rom_path), check_rom_(check_rom), rejoin_room_(rejoin_room), status_(notice) {
        const char* base = SDL_GetBasePath();
        const std::string picture = std::string(base ? base : "") + "menu_background.png";
        if (SDL_Surface* surface = SDL_LoadPNG(picture.c_str())) {
            background_ = SDL_CreateTextureFromSurface(r_, surface);
            SDL_SetTextureScaleMode(background_, SDL_SCALEMODE_LINEAR);
            SDL_DestroySurface(surface);
        }
    }

    ~StartScreen() {
        if (background_ != nullptr) SDL_DestroyTexture(background_);
        for (SDL_Gamepad* pad : pads_) SDL_CloseGamepad(pad);
        if (editing_ >= 0) SDL_StopTextInput(window_);
    }

    StartChoice run() {
        for (;;) {
            Input in;
            if (!read_input(in)) return {};
            if (const auto answer = take_dialog_answer()) {
                if (auto choice = on_rom_chosen(*answer)) return *choice;
            }
            SDL_SetRenderDrawBlendMode(r_, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(r_, 0, 0, 0, 255);
            SDL_RenderClear(r_);
            draw_background();
            std::optional<StartChoice> choice;
            const bool busy = dialog_open();
            if (settings_open_) {
                draw_main_menu(Input{}, choice);
                draw_settings(busy ? Input{} : in);
            } else {
                draw_main_menu(busy ? Input{} : in, choice);
            }
            SDL_RenderPresent(r_);
            if (choice) return *choice;
            std::fflush(stdout);
            SDL_Delay(16);
        }
    }

private:
    enum class After { Nothing, Local, Online, Rejoin };

    bool rom_ready() const {
        std::error_code missing;
        return !rom_path_.empty() && std::filesystem::exists(std::filesystem::u8path(rom_path_), missing);
    }

    bool dialog_open() {
        std::lock_guard<std::mutex> guard(g_dialog.lock);
        return g_dialog.open;
    }

    std::optional<std::string> take_dialog_answer() {
        std::lock_guard<std::mutex> guard(g_dialog.lock);
        if (!g_dialog.open || !g_dialog.answered) return std::nullopt;
        g_dialog.open = false;
        return g_dialog.path;
    }

    void choose_rom(After after) {
        after_ = after;
        open_rom_dialog(window_);
    }

    // With a ROM ready: checks it and goes on; without: asks for one first.
    std::optional<StartChoice> start(After after) {
        if (!rom_ready()) {
            choose_rom(after);
            return std::nullopt;
        }
        const std::string problem = check_rom_(rom_path_);
        if (!problem.empty()) {
            show_error(problem);
            return std::nullopt;
        }
        return choice_for(after);
    }

    std::optional<StartChoice> choice_for(After after) {
        StartChoice choice;
        switch (after) {
            case After::Local: choice.kind = StartChoice::Kind::Local; return choice;
            case After::Online: choice.kind = StartChoice::Kind::Online; return choice;
            case After::Rejoin: choice.kind = StartChoice::Kind::Rejoin; return choice;
            default: return std::nullopt;
        }
    }

    std::optional<StartChoice> on_rom_chosen(const std::string& path) {
        if (path.empty()) return std::nullopt;  // cancelled
        const std::string problem = check_rom_(path);
        if (!problem.empty()) {
            show_error(file_name(path) + ": " + problem);
            return std::nullopt;
        }
        rom_path_ = path;
        settings_.rom = path;
        save_settings(settings_file_, settings_);
        status_.clear();
        status_is_error_ = false;
        return choice_for(after_);
    }

    void show_error(const std::string& text) {
        status_ = text;
        status_is_error_ = true;
        std::printf("start screen: %s\n", text.c_str());
    }

    bool read_input(Input& in) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_EVENT_QUIT: return false;
                case SDL_EVENT_KEY_DOWN:
                    switch (event.key.scancode) {
                        case SDL_SCANCODE_UP: in.up = true; break;
                        case SDL_SCANCODE_DOWN: in.down = true; break;
                        case SDL_SCANCODE_LEFT: in.left = true; break;
                        case SDL_SCANCODE_RIGHT: in.right = true; break;
                        case SDL_SCANCODE_TAB:
                            if (event.key.mod & SDL_KMOD_SHIFT) in.tab_left = true;
                            else in.tab_right = true;
                            break;
                        case SDL_SCANCODE_PAGEUP: in.tab_left = true; break;
                        case SDL_SCANCODE_PAGEDOWN: in.tab_right = true; break;
                        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: in.activate = true; break;
                        case SDL_SCANCODE_ESCAPE: in.back = true; break;
                        case SDL_SCANCODE_BACKSPACE: in.backspace = true; break;
                        case SDL_SCANCODE_X: if (editing_ < 0) in.activate = true; break;
                        case SDL_SCANCODE_Z: if (editing_ < 0) in.back = true; break;
                        case SDL_SCANCODE_Q: if (editing_ < 0) in.tab_left = true; break;
                        case SDL_SCANCODE_W: if (editing_ < 0) in.tab_right = true; break;
                        case SDL_SCANCODE_F11:
                            settings_.fullscreen = !settings_.fullscreen;
                            SDL_SetWindowFullscreen(window_, settings_.fullscreen);
                            save_settings(settings_file_, settings_);
                            break;
                        default: break;
                    }
                    break;
                case SDL_EVENT_TEXT_INPUT: in.typed += event.text.text; break;
                case SDL_EVENT_MOUSE_MOTION: in.moved = true; break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    if (event.button.button == SDL_BUTTON_LEFT) in.clicked = true;
                    break;
                case SDL_EVENT_GAMEPAD_ADDED: pads_.push_back(SDL_OpenGamepad(event.gdevice.which)); break;
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                    switch (event.gbutton.button) {
                        case SDL_GAMEPAD_BUTTON_DPAD_UP: in.up = true; break;
                        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: in.down = true; break;
                        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: in.left = true; break;
                        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: in.right = true; break;
                        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: in.tab_left = true; break;
                        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: in.tab_right = true; break;
                        case SDL_GAMEPAD_BUTTON_EAST: case SDL_GAMEPAD_BUTTON_START: in.activate = true; break;
                        case SDL_GAMEPAD_BUTTON_SOUTH: in.back = true; break;
                        default: break;
                    }
                    break;
                default: break;
            }
        }
        float mx = 0, my = 0;
        SDL_GetMouseState(&mx, &my);
        SDL_RenderCoordinatesFromWindow(r_, mx, my, &in.mouse_x, &in.mouse_y);
        return true;
    }

    void draw_background() {
        if (background_ != nullptr) {
            // Fills the screen, cut at the sides or the top if the picture has another shape.
            float w = 0, h = 0;
            SDL_GetTextureSize(background_, &w, &h);
            const float scale = std::max(kW / w, kH / h);
            const SDL_FRect where{(kW - w * scale) / 2, (kH - h * scale) / 2, w * scale, h * scale};
            SDL_RenderTexture(r_, background_, nullptr, &where);
            // Darker behind the menu, so the white text stays readable on a bright picture.
            ui::fill_horizontal_gradient(r_, {0, 0, 470, kH}, {20, 10, 50, 120}, {20, 10, 50, 0});
        } else {
            stripes(r_);
        }
        ui::fill_vertical_gradient(r_, {0, kH - 110, kW, 110}, {10, 6, 26, 0}, {10, 6, 26, 200});
    }

    void draw_logo() {
        const TextStyle presents{Font::Heavy, 15, kWhite, kInk, 2.5f};
        text_.draw("Erzoy38 presents", 78, 150, presents);
        static const Rgba kLetters[] = {{232, 48, 42, 255},  {45, 184, 75, 255},   {247, 197, 30, 255},
                                        {47, 123, 234, 255}, {230, 75, 168, 255},  {242, 140, 40, 255}};
        const std::string word = "MARIO PARTY DS";
        float size = 40;
        auto measure = [&](float s) {
            float total = 0;
            for (char c : word) {
                total += c == ' ' ? s * 0.32f : text_.width(std::string(1, c), {Font::Heavy, s, kWhite, kInk, 4.5f}) - 1;
            }
            return total;
        };
        while (size > 20 && measure(size) > 318) size -= 1;
        const float total = measure(size);
        float x = 92;
        const float y = 178;
        star(r_, x - 12, y + size * 0.62f, 9, {255, 214, 40, 255}, kInk);
        int coloured = 0;
        for (size_t i = 0; i < word.size(); ++i) {
            if (word[i] == ' ') {
                x += size * 0.32f;
                continue;
            }
            const TextStyle style{Font::Heavy, size, kLetters[coloured % 6], kInk, 4.5f};
            const float lift = (coloured % 2 == 0) ? 0.0f : 3.0f;
            const float angle = (coloured % 3 == 0) ? -5.0f : (coloured % 3 == 1) ? 3.0f : -2.0f;
            x += text_.draw(std::string(1, word[i]), x, y + lift, style, angle) - 1;
            ++coloured;
        }
        star(r_, 92 + total + 12, y + size * 0.62f, 9, {255, 214, 40, 255}, kInk);
    }

    void draw_main_menu(const Input& in, std::optional<StartChoice>& choice) {
        draw_logo();
        struct Item { std::string label; int action; };
        enum { kStart, kChoose, kRejoin, kSettings, kOnline, kQuit };
        std::vector<Item> items;
        items.push_back(rom_ready() ? Item{"START GAME", kStart} : Item{"SELECT ROM IMAGE", kChoose});
        if (!rejoin_room_.empty()) items.push_back({"BACK INTO THE ONLINE GAME", kRejoin});
        items.push_back({"SETTINGS", kSettings});
        items.push_back({"PLAY ONLINE", kOnline});
        items.push_back({"QUIT", kQuit});
        focus_ = std::clamp(focus_, 0, int(items.size()) - 1);
        if (in.down) focus_ = (focus_ + 1) % int(items.size());
        if (in.up) focus_ = (focus_ + int(items.size()) - 1) % int(items.size());

        int chosen = -1;
        if (in.activate) chosen = items[size_t(focus_)].action;
        const float top = 262, step = 40;
        for (size_t i = 0; i < items.size(); ++i) {
            const float y = top + step * float(i);
            const SDL_FRect bar{78, y - 7, 330, 34};
            if (in.moved && inside(in, bar)) focus_ = int(i);
            if (in.clicked && inside(in, bar)) {
                focus_ = int(i);
                chosen = items[i].action;
            }
            const bool focused = focus_ == int(i);
            if (focused) ui::fill_rounded_gradient(r_, bar, 6, kChosen, {kChosen.r, kChosen.g, kChosen.b, 0});
            const TextStyle style = focused ? TextStyle{Font::Heavy, 19, kInk} : TextStyle{Font::Heavy, 19, kWhite, kInk, 2.5f};
            text_.draw(items[i].label, 92, y - 3, style);
        }
        if (!rejoin_room_.empty()) {
            text_.draw("Online game " + rejoin_room_ + " is still running.", 78, 500,
                       {Font::Heavy, 13, kWhite, kInk, 2.0f});
        }

        std::string status = status_;
        if (status.empty()) status = rom_ready() ? "ROM: " + file_name(rom_path_) : "No ROM image selected.";
        text_.draw(status, 78, 528, {Font::Heavy, 14, status_is_error_ ? kErrorText : kWhite, kInk, 2.0f});
        const TextStyle version{Font::Heavy, 14, kWhite, kInk, 2.0f};
        const std::string build = "Build " + build_date();
        text_.draw(build, 990 - text_.width(build, version), 528, version);
        // The DLSS SDK's licence asks for the attribution on the start screen.
        const TextStyle credit{Font::Plain, 12, {255, 255, 255, 200}, kInk, 1.5f};
        const std::string uses = "Uses NVIDIA DLSS. NVIDIA and DLSS are trademarks of NVIDIA Corporation.";
        text_.draw(uses, 990 - text_.width(uses, credit), 552, credit);

        switch (chosen) {
            case kStart: choice = start(After::Local); break;
            case kChoose: choose_rom(After::Local); break;
            case kRejoin: choice = start(After::Rejoin); break;
            case kOnline: choice = start(After::Online); break;
            case kSettings:
                settings_open_ = true;
                row_ = 0;
                break;
            case kQuit: choice = StartChoice{}; break;
            default: break;
        }
    }

    // --- Settings ---
    struct Row {
        std::string label, value, help;
        enum class Kind { Action, Choice, Field } kind = Kind::Action;
        std::function<void(int)> change;  // 0: chosen, -1 / +1: left / right
        std::string* field = nullptr;     // Kind::Field: the text being edited
        size_t limit = 0;
        bool (*allowed)(char) = nullptr;
        bool meter = false;  // shows the microphone's level instead of a value
    };

    static bool name_char(char c) { return c >= 0x20 && c < 0x7F; }
    static bool server_char(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == ':' || c == '-';
    }

    void save() { save_settings(settings_file_, settings_); }

    std::vector<Row> rows_for(int tab) {
        std::vector<Row> rows;
        if (tab == 0) {
            Row rom{"ROM IMAGE", rom_ready() ? file_name(rom_path_) : "(none)",
                    "The game's ROM: Mario Party DS (Europe, Rev 1), a file ending in .nds. Chosen once, it is "
                    "remembered. Each player needs a copy of their own."};
            rom.change = [this](int delta) {
                if (delta == 0) choose_rom(After::Nothing);
            };
            rows.push_back(rom);
            Row language{"LANGUAGE", kLanguages[std::clamp(settings_.language, 1, 5)],
                         "The language of the game's texts. Online, every player sees the host's language."};
            language.kind = Row::Kind::Choice;
            language.change = [this](int delta) {
                settings_.language = (settings_.language - 1 + (delta == 0 ? 1 : delta) + 5) % 5 + 1;
                save();
            };
            rows.push_back(language);
        } else if (tab == 1) {
            // Automatic first, then every resolution of the display.
            std::vector<std::string> sizes{""};
            for (const auto& [w, h] : display_resolutions()) sizes.push_back(std::to_string(w) + "x" + std::to_string(h));
            int aw = 0, ah = 0;
            automatic_window_size(settings_.layout, settings_.aspect, settings_.scale, aw, ah);
            const std::string automatic = "Auto (" + std::to_string(aw) + " x " + std::to_string(ah) + ")";
            int rw = 0, rh = 0;
            std::string shown = automatic;
            if (parse_resolution(settings_.resolution, rw, rh)) {
                shown = std::to_string(rw) + " x " + std::to_string(rh);
                if (rh == 720) shown += "  (720p)";
                else if (rh == 1080) shown += "  (1080p)";
                else if (rh == 1440) shown += "  (1440p)";
                else if (rh == 2160) shown += "  (4K)";
            }
            Row size{"RESOLUTION", shown,
                     "The size of the game window, or of the screen in fullscreen: your display's resolutions plus "
                     "720p, 1080p, 1440p and 4K. A size larger than the display only fits in fullscreen on a display "
                     "that has it. Auto makes the window fit the DS screens."};
            size.kind = Row::Kind::Choice;
            size.change = [this, sizes](int delta) {
                auto at = std::find(sizes.begin(), sizes.end(), settings_.resolution);
                const int n = int(sizes.size());
                const int i = at == sizes.end() ? 0 : int(at - sizes.begin());
                settings_.resolution = sizes[size_t((i + (delta == 0 ? 1 : delta) + n) % n)];
                save();
            };
            rows.push_back(size);
            Row layout{"SCREEN LAYOUT", layout_name(settings_.layout),
                       "Where the two DS screens go: one above the other as on the DS, side by side, or hybrid: "
                       "the top screen large with the touch screen small beside it, good for wide monitors. While "
                       "playing, Tab (or Select on a controller in hybrid) swaps the two screens."};
            layout.kind = Row::Kind::Choice;
            layout.change = [this](int delta) {
                settings_.layout = (settings_.layout + (delta == 0 ? 1 : delta) + kLayoutCount) % kLayoutCount;
                save();
            };
            rows.push_back(layout);
            Row aspect{"ASPECT RATIO", aspect_name(settings_.aspect),
                       "The shape of each screen. 4:3 is the DS's own. 16:9 stretches each screen wider. Widescreen "
                       "fills the whole window or monitor. The game itself does not show more of its world."};
            aspect.kind = Row::Kind::Choice;
            aspect.change = [this](int delta) {
                settings_.aspect = (settings_.aspect + (delta == 0 ? 1 : delta) + kAspectCount) % kAspectCount;
                save();
            };
            rows.push_back(aspect);
            // Per DS screen: 6x is about the height of a 1440p display, 11x of a 4K one.
            static const int kScales[] = {1, 2, 3, 4, 5, 6, 8, 11};
            constexpr int kScaleCount = 8;
            const int r = settings_.render_scale;
            std::string render_value = std::to_string(r) + "x  (" + std::to_string(256 * r) + " x " + std::to_string(192 * r) + ")";
            if (r == 6) render_value += " 1440p";
            else if (r == 11) render_value += " 4K";
            Row render{"3D RESOLUTION", render_value,
                       "The 3D graphics drawn sharper, up to 4K (11x the DS's resolution per screen; 6x is about "
                       "1440p). The processor draws them itself on all its cores: 4x runs smoothly on a modern PC; "
                       "from 5x on, busy 3D scenes can make the game run slower than it should, 4K about half speed. "
                       "2D pictures follow UPSCALING. Online, every player may choose their own."};
            render.kind = Row::Kind::Choice;
            render.change = [this](int delta) {
                int i = 0;
                for (int k = 0; k < kScaleCount; ++k) {
                    if (kScales[k] == settings_.render_scale) i = k;
                }
                settings_.render_scale = kScales[(i + (delta == 0 ? 1 : delta) + kScaleCount) % kScaleCount];
                save();
            };
            rows.push_back(render);
            static const int kRates[] = {30, 60, 120, 144, 240};
            const int fps = settings_.frame_rate;
            Row rate{"FRAME RATE", std::to_string(fps) + (fps > 60 ? " Hz (synced)" : " FPS"),
                     "The DS game makes 60 pictures a second; that never changes, or the game would run faster. "
                     "30 shows every second picture and saves power. 60 is normal. 120, 144 and 240 are for "
                     "monitors with that refresh rate: each picture is shown in step with the monitor (VSync), "
                     "which avoids stutter and tearing, but there are no extra pictures."};
            rate.kind = Row::Kind::Choice;
            rate.change = [this](int delta) {
                int i = 1;
                for (int k = 0; k < 5; ++k) {
                    if (kRates[k] == settings_.frame_rate) i = k;
                }
                settings_.frame_rate = kRates[(i + (delta == 0 ? 1 : delta) + 5) % 5];
                save();
            };
            rows.push_back(rate);
            static const char* const kUpscaleNames[] = {"Off", "Smooth edges (Scale2x)", "AMD FSR 1"};
            Row upscale{"UPSCALING", kUpscaleNames[std::clamp(settings_.upscale, 0, 2)],
                        "How the 2D pictures (menus, characters, text) are made larger. Smooth edges: pixel art with "
                        "smooth diagonals, crisp lines. AMD FSR 1: edge-aware upscaling with sharpening, softer and "
                        "more like a painting. Both run on the processor, on any PC; DLSS is not possible, it needs "
                        "an NVIDIA card and a 3D engine this game does not have. Combine with 3D RESOLUTION."};
            upscale.kind = Row::Kind::Choice;
            upscale.change = [this](int delta) {
                settings_.upscale = (settings_.upscale + (delta == 0 ? 1 : delta) + 3) % 3;
                save();
            };
            rows.push_back(upscale);
            // DLSS: what the card offers, and the 3D size it would draw for the chosen 3D resolution.
            std::string dlss_value = "Not available";
            if (dlss_available()) {
                dlss_value = dlss_mode_name(settings_.dlss);
                unsigned w = 0, h = 0;
                const unsigned out_w = 256u * unsigned(settings_.render_scale), out_h = 192u * unsigned(settings_.render_scale);
                if (settings_.dlss != kDlssOff && dlss_render_size(settings_.dlss, out_w, out_h, w, h)) {
                    dlss_value += "  (" + std::to_string(w) + "x" + std::to_string(h) + " > " + std::to_string(out_w) + "x"
                                  + std::to_string(out_h) + ")";
                }
            }
            Row dlss{"NVIDIA DLSS", dlss_value,
                     dlss_available()
                         ? "NVIDIA DLSS on this card: the 3D graphics drawn smaller and made sharp again by the card's AI "
                           "cores, for more speed (Quality to Ultra Performance), or DLAA for smoother edges at full "
                           "size. The numbers show the size drawn and the 3D RESOLUTION it becomes (set that above 1x). "
                           "Movement is found by matching the game's polygons from picture to picture."
                         : "NVIDIA DLSS needs an NVIDIA RTX graphics card, a current driver and nvngx_dlss.dll next to "
                           "the game. Here: " + dlss_reason()};
            if (dlss_available()) {
                dlss.kind = Row::Kind::Choice;
                dlss.change = [this](int delta) {
                    settings_.dlss = (settings_.dlss + (delta == 0 ? 1 : delta) + kDlssModeCount) % kDlssModeCount;
                    save();
                };
            }
            rows.push_back(dlss);
            Row full{"FULLSCREEN", settings_.fullscreen ? "On" : "Off", "The whole screen instead of a window. F11 switches it at any time."};
            full.kind = Row::Kind::Choice;
            full.change = [this](int) {
                settings_.fullscreen = !settings_.fullscreen;
                SDL_SetWindowFullscreen(window_, settings_.fullscreen);
                save();
            };
            rows.push_back(full);
            Row smooth{"PICTURE", settings_.smooth ? "Smooth" : "Sharp pixels",
                       "How the DS picture is made larger: sharp pixels as on the DS, or smoothed."};
            smooth.kind = Row::Kind::Choice;
            smooth.change = [this](int) {
                settings_.smooth = !settings_.smooth;
                save();
            };
            rows.push_back(smooth);
        } else if (tab == 2) {
            Row volume{"VOLUME", std::to_string(settings_.volume) + " %",
                       "How loud the game is. 0 % is silent."};
            volume.kind = Row::Kind::Choice;
            volume.change = [this](int delta) {
                if (delta == 0) settings_.volume = settings_.volume >= 100 ? 0 : settings_.volume + 10;
                else settings_.volume = std::clamp(settings_.volume + delta * 10, 0, 100);
                save();
            };
            rows.push_back(volume);
            // Off, Windows' default, then every recording device that is connected now.
            std::vector<std::string> choices{kMicOff, ""};
            for (const std::string& name : Microphone::devices()) choices.push_back(name);
            std::string shown = settings_.mic_device == kMicOff ? "Off (Space key only)"
                                : settings_.mic_device.empty() ? "Windows default"
                                                               : settings_.mic_device;
            Row device{"MICROPHONE", shown,
                       "Which microphone the game listens to: the PC's, a headset's or a controller's. A PS5 "
                       "controller (DualSense) shows up here when it is connected with a USB cable; over Bluetooth "
                       "Windows gets no sound from it. Space and the left trigger always blow at full strength."};
            device.kind = Row::Kind::Choice;
            device.change = [this, choices](int delta) {
                const auto at = std::find(choices.begin(), choices.end(), settings_.mic_device);
                const int n = int(choices.size());
                const int i = at == choices.end() ? 1 : int(at - choices.begin());
                settings_.mic_device = choices[size_t((i + (delta == 0 ? 1 : delta) + n) % n)];
                save();
            };
            rows.push_back(device);
            Row sensitivity{"MIC SENSITIVITY", std::to_string(settings_.mic_sensitivity) + " %",
                            "How strongly your microphone is amplified for the game. Speak normally and set it so the bar "
                            "below stays green: that is the game's 'perfect volume'. Orange is too quiet, red too loud."};
            sensitivity.kind = Row::Kind::Choice;
            sensitivity.change = [this](int delta) {
                const int step = delta == 0 ? 10 : delta * 10;
                int next = settings_.mic_sensitivity + step;
                if (delta == 0 && next > 100) next = 10;
                settings_.mic_sensitivity = std::clamp(next, 10, 100);
                save();
            };
            rows.push_back(sensitivity);
            Row level{"MIC LEVEL", "", "What the game hears, with the zones of its own microphone test: grey nothing, orange too quiet, "
                                       "green just right, red too loud. Blowing is meant to reach red."};
            level.meter = true;
            rows.push_back(level);
        } else if (tab == 3) {
            Row minigames{"UNLOCK ALL MINIGAMES", settings_.cheat_minigames ? "On" : "Off",
                          "Every minigame shows up in Minigame Mode's Free Play, as if it had been played. "
                          "The game saves this with your progress: once saved, it stays even if you turn the "
                          "cheat off again."};
            minigames.kind = Row::Kind::Choice;
            minigames.change = [this](int) {
                settings_.cheat_minigames = !settings_.cheat_minigames;
                save();
            };
            rows.push_back(minigames);
            Row boards{"UNLOCK ALL BOARDS", settings_.cheat_boards ? "On" : "Off",
                       "Bowser's Pinball Machine, the board you normally get by finishing Story Mode, can be "
                       "chosen in Party Mode. Saved with your progress like the minigames."};
            boards.kind = Row::Kind::Choice;
            boards.change = [this](int) {
                settings_.cheat_boards = !settings_.cheat_boards;
                save();
            };
            rows.push_back(boards);
        } else if (tab == 4) {
            Row hd{"HD TEXTURES", settings_.hd_textures ? "On" : "Off",
                   "Texture mods: the game's 3D textures replaced by pictures from the folder \"textures\" next to "
                   "the game, for example HD textures. A pack goes into its own folder there. Best together with "
                   "a higher 3D resolution."};
            hd.kind = Row::Kind::Choice;
            hd.change = [this](int) {
                settings_.hd_textures = !settings_.hd_textures;
                save();
            };
            rows.push_back(hd);
            Row dump{"DUMP TEXTURES", settings_.dump_textures ? "On" : "Off",
                     "For making a pack: every texture the game shows is saved once to textures\\dump as a PNG. "
                     "Draw a better version (for example 4 times as large), keep the file name, and put it into "
                     "a folder of your pack."};
            dump.kind = Row::Kind::Choice;
            dump.change = [this](int) {
                settings_.dump_textures = !settings_.dump_textures;
                save();
            };
            rows.push_back(dump);
            Row folder{"OPEN TEXTURE FOLDER", "", "Opens the folder \"textures\" in Explorer; it is made if missing."};
            folder.change = [](int delta) {
                if (delta == 0) open_texture_folder();
            };
            rows.push_back(folder);
            const std::vector<ModInfo> mods = find_mods(mods_folder());
            int mod_files = 0;
            for (const ModInfo& m : mods) mod_files += m.files;
            Row file_mods{"FILE MODS",
                          settings_.file_mods ? "On (" + std::to_string(mods.size()) + " mods, " + std::to_string(mod_files) + " files)"
                                              : "Off",
                          "Mods that replace the game's files, for example a character's model, textures and "
                          "pictures, so another character appears in its place. Each mod is a folder in "
                          "\"mods\" next to the game, holding files with the same paths as the game's own. "
                          "Online, every player needs the same mods."};
            file_mods.kind = Row::Kind::Choice;
            file_mods.change = [this](int) {
                settings_.file_mods = !settings_.file_mods;
                save();
            };
            rows.push_back(file_mods);
            Row mods_folder_row{"OPEN MODS FOLDER", "", "Opens the folder \"mods\" in Explorer; it is made if missing."};
            mods_folder_row.change = [](int delta) {
                if (delta != 0) return;
                std::error_code error;
                std::filesystem::create_directories(mods_folder(), error);
                std::string url = "file:///";
                const std::u8string text = mods_folder().u8string();
                for (char c : std::string(text.begin(), text.end())) {
                    if (c == '\\') url += '/';
                    else if (c == ' ') url += "%20";
                    else url += c;
                }
                SDL_OpenURL(url.c_str());
            };
            rows.push_back(mods_folder_row);
            Row extract{"EXTRACT GAME FILES", "",
                        "For making a mod: writes all of the game's files to mods\\_game_files. Copy the files "
                        "you change into a folder of your mod, keeping their paths. Needs the ROM to be chosen."};
            extract.change = [this](int delta) {
                if (delta != 0) return;
                std::ifstream in(std::filesystem::u8path(rom_path_), std::ios::binary);
                const std::vector<std::uint8_t> rom((std::istreambuf_iterator<char>(in)), {});
                if (rom.empty()) {
                    settings_note_ = "Choose the ROM first.";
                    return;
                }
                settings_note_ = std::to_string(extract_game_files(rom, mods_folder())) + " files written to mods\\_game_files.";
            };
            rows.push_back(extract);
        } else {
            Row name{"PLAYER NAME", settings_.name, "Your name in the game and in online rooms, up to 10 letters."};
            name.kind = Row::Kind::Field;
            name.field = &settings_.name;
            name.limit = size_t(lobby::kMaxName);
            name.allowed = name_char;
            rows.push_back(name);
            Row server{"SERVER", settings_.server,
                       "The lobby server for online play, as ADDRESS or ADDRESS:PORT. The host tells you this address; "
                       "see ONLINE.txt."};
            server.kind = Row::Kind::Field;
            server.field = &settings_.server;
            server.limit = 60;
            server.allowed = server_char;
            rows.push_back(server);
            Row forget{"FORGET SERVER KEY", "",
                       "Only if the host really set up the server again: the game then accepts this server's new key. "
                       "Ask the host first."};
            forget.change = [this](int delta) {
                if (delta != 0) return;
                const std::string server = settings_.server.find(':') == std::string::npos
                                               ? settings_.server + ":" + std::to_string(lobby::kDefaultPort)
                                               : settings_.server;
                secure::KnownServers(known_servers_file(settings_file_).string()).forget(server);
                settings_note_ = "The key of " + server + " is forgotten.";
            };
            rows.push_back(forget);
        }
        return rows;
    }

    void stop_editing(bool keep) {
        if (editing_ < 0) return;
        if (!keep || edit_target_ == nullptr || edit_text_.empty()) {
            // An empty name or server is not taken.
        } else {
            *edit_target_ = edit_text_;
            save();
        }
        editing_ = -1;
        edit_target_ = nullptr;
        SDL_StopTextInput(window_);
    }

    void start_editing(int index, Row& row) {
        editing_ = index;
        edit_target_ = row.field;
        edit_text_ = *row.field;
        SDL_StartTextInput(window_);
    }

    void update_microphone() {
        const bool wanted = settings_open_ && tab_ == 2;
        if (!wanted) {
            if (mic_.is_open()) mic_.close();
            mic_tried_ = false;
            return;
        }
        if (!mic_.is_open() || mic_.device() != settings_.mic_device) {
            if (mic_.device() != settings_.mic_device || !mic_tried_) {
                mic_.open(settings_.mic_device);
                mic_tried_ = true;
            }
        }
        mic_.update();
    }

    void draw_settings(const Input& in) {
        update_microphone();
        static const char* const kTabs[] = {"PRELAUNCH", "VIDEO", "SOUND", "CHEATS", "MODS", "ONLINE"};
        constexpr int kTabCount = 6;
        const SDL_FRect panel{78, 60, 868, 458};
        ui::fill_rounded(r_, panel, 12, kPanel);
        ui::outline_rounded(r_, panel, 12, kPanelEdge);

        // Tabs.
        if (editing_ < 0) {
            if (in.tab_left) tab_ = (tab_ + kTabCount - 1) % kTabCount, row_ = 0, settings_note_.clear();
            if (in.tab_right) tab_ = (tab_ + 1) % kTabCount, row_ = 0, settings_note_.clear();
        }
        const TextStyle tab_on{Font::Semibold, 14, kWhite, {0, 0, 0, 0}, 0, 0.6f};
        const TextStyle tab_off{Font::Semibold, 14, {255, 255, 255, 110}, {0, 0, 0, 0}, 0, 0.6f};
        float tx = panel.x + 24;
        for (int i = 0; i < kTabCount; ++i) {
            const float w = text_.width(kTabs[i], tab_on);
            const SDL_FRect hit{tx - 12, panel.y, w + 24, 40};
            if (in.clicked && inside(in, hit) && editing_ < 0) {
                tab_ = i;
                row_ = 0;
                settings_note_.clear();
            }
            text_.draw(kTabs[i], tx, panel.y + 12, i == tab_ ? tab_on : tab_off);
            if (i == tab_) ui::fill_rounded(r_, {tx - 12, panel.y + 36, w + 24, 3}, 1.5f, kPanelEdge);
            tx += w + 46;
        }
        const SDL_FRect close{panel.x + panel.w - 40, panel.y + 6, 30, 30};
        const TextStyle close_style{Font::Plain, 20, inside(in, close) ? kWhite : Rgba{255, 255, 255, 170}};
        text_.draw("\xC3\x97", close.x + 8, close.y + 1, close_style);  // ×
        ui::fill_rounded(r_, {panel.x + 1, panel.y + 40, panel.w - 2, 1}, 0, {46, 168, 240, 200});
        const float middle = panel.x + panel.w / 2;
        ui::fill_rounded(r_, {middle, panel.y + 41, 1, panel.h - 42}, 0, {46, 168, 240, 160});

        std::vector<Row> rows = rows_for(tab_);
        row_ = std::clamp(row_, 0, int(rows.size()) - 1);
        if (editing_ < 0) {
            if (in.down) row_ = (row_ + 1) % int(rows.size());
            if (in.up) row_ = (row_ + int(rows.size()) - 1) % int(rows.size());
        }
        int change_row = -1, change = 0;
        const float rx = panel.x + 22, rw = panel.w / 2 - 44, rh = 30;
        for (size_t i = 0; i < rows.size(); ++i) {
            Row& row = rows[i];
            const SDL_FRect box{rx, panel.y + 58 + float(i) * (rh + 8), rw, rh};
            if (in.moved && inside(in, box) && editing_ < 0) row_ = int(i);
            if (in.clicked && inside(in, box)) {
                if (editing_ >= 0 && editing_ != int(i)) stop_editing(true);
                row_ = int(i);
                change_row = int(i);
                change = 0;
                // On a choice, the left part of the value (‹) goes back, the right part (›) forward.
                if (row.kind == Row::Kind::Choice && i < value_left_.size() && value_left_[i] > 0) {
                    const float left = value_left_[i], right = box.x + box.w - 8;
                    if (in.mouse_x >= left - 12) change = in.mouse_x < (left + right) / 2 ? -1 : 1;
                }
            }
            const bool focused = row_ == int(i);
            ui::fill_rounded(r_, box, 14, focused ? Rgba{255, 255, 255, 46} : Rgba{255, 255, 255, 16});
            ui::outline_rounded(r_, box, 14, focused ? Rgba{120, 205, 255, 255} : Rgba{140, 180, 255, 110});
            const TextStyle label{Font::Plain, 14, kWhite, {0, 0, 0, 0}, 0, 0.4f};
            text_.draw(row.label, box.x + 16, box.y + 6, label);
            if (row.meter) {
                // The level as a bar, with the mark where blowing starts.
                // The game's own zones: nothing heard, too quiet, just right, too loud.
                const SDL_FRect bar{box.x + box.w * 0.45f, box.y + 10, box.w * 0.55f - 16, box.h - 20};
                ui::fill_rounded(r_, bar, 4, {255, 255, 255, 40});
                const float lvl = mic_.game_level(settings_.mic_sensitivity);
                const Rgba colour = lvl >= kMicLoud ? Rgba{255, 110, 90, 255}
                                    : lvl >= kMicGood ? Rgba{90, 230, 120, 255}
                                    : lvl >= kMicHeard ? Rgba{255, 200, 80, 255}
                                                       : Rgba{170, 190, 220, 255};
                if (lvl > 0.003f) ui::fill_rounded(r_, {bar.x, bar.y, bar.w * lvl, bar.h}, 4, colour);
                for (float mark : {kMicHeard, kMicGood, kMicLoud}) {
                    ui::fill_rounded(r_, {bar.x + bar.w * mark - 1, bar.y - 3, 2, bar.h + 6}, 1, {255, 255, 255, 200});
                }
                if (!mic_.is_open()) {
                    const TextStyle off{Font::Plain, 13, {255, 255, 255, 160}};
                    const char* const text = settings_.mic_device == kMicOff ? "microphone off" : "no microphone";
                    text_.draw(text, bar.x + 8, bar.y - 1, off);
                }
                continue;
            }
            std::string value = row.value;
            if (editing_ == int(i)) value = edit_text_ + ((SDL_GetTicks() / 400) % 2 ? "_" : " ");
            if (row.kind == Row::Kind::Choice && focused) value = "\xE2\x80\xB9  " + value + "  \xE2\x80\xBA";  // ‹ ›
            const TextStyle value_style{Font::Plain, 15, kWhite};
            // A long value (a file name) is cut at the end, so it never runs into the label.
            const float room = box.w - 48 - text_.width(row.label, label);
            if (text_.width(value, value_style) > room) {
                const std::string dots = "\xE2\x80\xA6";  // …
                while (value.size() > 1 && text_.width(value + dots, value_style) > room) {
                    value.pop_back();
                    while (!value.empty() && (static_cast<unsigned char>(value.back()) & 0xC0) == 0x80) value.pop_back();
                }
                value += dots;
            }
            const float vw = text_.width(value, value_style);
            text_.draw(value, box.x + box.w - 16 - vw, box.y + 5, value_style);
            if (value_left_.size() <= i) value_left_.resize(i + 1, 0);
            value_left_[i] = box.x + box.w - 16 - vw;
        }
        // Keys for the row in focus.
        if (editing_ >= 0) {
            Row& row = rows[size_t(editing_)];
            for (char c : in.typed) {
                if (row.allowed(c) && edit_text_.size() < row.limit) edit_text_ += c;
            }
            if (in.backspace && !edit_text_.empty()) edit_text_.pop_back();
            if (in.activate) stop_editing(true);
            else if (in.back) stop_editing(false);
        } else {
            if (in.activate) change_row = row_, change = 0;
            if (in.left && rows[size_t(row_)].kind == Row::Kind::Choice) change_row = row_, change = -1;
            if (in.right && rows[size_t(row_)].kind == Row::Kind::Choice) change_row = row_, change = 1;
            if (in.back || (in.clicked && inside(in, close))) {
                settings_open_ = false;
                settings_note_.clear();
                return;
            }
        }
        if (change_row >= 0 && editing_ != change_row) {
            Row& row = rows[size_t(change_row)];
            if (row.kind == Row::Kind::Field) start_editing(change_row, row);
            else if (row.change) row.change(change);
        }

        // Right half: what the row in focus does.
        const Row& shown = rows[size_t(row_)];
        const TextStyle help{Font::Plain, 15, {255, 255, 255, 225}};
        float hy = panel.y + 62;
        text_.draw(shown.label, middle + 26, hy, {Font::Semibold, 16, kWhite, {0, 0, 0, 0}, 0, 0.4f});
        hy += 32;
        for (const std::string& line : ui::wrap(text_, shown.help, help, panel.w / 2 - 52)) {
            text_.draw(line, middle + 26, hy, help);
            hy += 22;
        }
        if (!settings_note_.empty()) text_.draw(settings_note_, middle + 26, hy + 12, {Font::Semibold, 15, {150, 230, 255, 255}});
        const char* const keys = editing_ >= 0 ? "Type, then Enter.\nEsc: back without change."
                                               : "Up/Down: choose \xC2\xB7 Left/Right: change \xC2\xB7 Enter: open\n"
                                                 "Esc: close \xC2\xB7 Q/W: tabs";
        const TextStyle key_style{Font::Plain, 12, {255, 255, 255, 150}};
        const std::vector<std::string> key_lines = ui::wrap(text_, keys, key_style, panel.w / 2 - 52);
        float ky = panel.y + panel.h - 18 - 18 * float(key_lines.size());
        for (const std::string& line : key_lines) {
            text_.draw(line, middle + 26, ky, key_style);
            ky += 18;
        }
    }

    SDL_Window* window_;
    SDL_Renderer* r_;
    ui::Text text_;
    AppSettings& settings_;
    const std::filesystem::path& settings_file_;
    std::string& rom_path_;
    const std::function<std::string(const std::string&)>& check_rom_;
    std::string rejoin_room_;
    std::string status_;
    bool status_is_error_ = false;
    SDL_Texture* background_ = nullptr;
    Microphone mic_;
    bool mic_tried_ = false;
    std::vector<SDL_Gamepad*> pads_;
    int focus_ = 0;
    After after_ = After::Nothing;
    bool settings_open_ = false;
    int tab_ = 0;
    int row_ = 0;
    int editing_ = -1;
    std::vector<float> value_left_;  // where each row's value began last picture, for clicks on ‹ ›
    std::string* edit_target_ = nullptr;
    std::string edit_text_;
    std::string settings_note_;
};

}  // namespace

StartChoice run_start_screen(SDL_Window* window, SDL_Renderer* renderer, AppSettings& settings,
                             const std::filesystem::path& settings_file, std::string& rom_path,
                             const std::function<std::string(const std::string&)>& check_rom,
                             const std::string& rejoin_room, const std::string& notice) {
    StartScreen screen(window, renderer, settings, settings_file, rom_path, check_rom, rejoin_room, notice);
    return screen.run();
}
