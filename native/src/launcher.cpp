#include "launcher.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <functional>
#include <vector>

namespace {

constexpr float kWidth = 256;
constexpr float kHeight = 384;

// Colours of the game's menus: turquoise ground, player colours P1 blue, P2 red, P3 green, P4 yellow.
struct Colour { Uint8 r, g, b; };
constexpr Colour kGround{0x30, 0xD0, 0xD8};
constexpr Colour kGroundDark{0x20, 0xB8, 0xC4};
constexpr Colour kPanel{0xFF, 0xFF, 0xFF};
constexpr Colour kInk{0x20, 0x20, 0x28};
constexpr Colour kGrey{0x90, 0x98, 0xA0};
constexpr Colour kButton{0x50, 0xC8, 0x40};
constexpr Colour kButtonFocus{0xFF, 0xD0, 0x20};
constexpr Colour kTitle{0xE8, 0x30, 0x20};
constexpr Colour kPlayer[4] = {{0x20, 0x60, 0xE8}, {0xE0, 0x30, 0x20}, {0x30, 0xA8, 0x30}, {0xE8, 0xB8, 0x10}};

void set(SDL_Renderer* r, Colour c, Uint8 a = 255) { SDL_SetRenderDrawColor(r, c.r, c.g, c.b, a); }

void fill(SDL_Renderer* r, float x, float y, float w, float h, Colour c) {
    set(r, c);
    const SDL_FRect rect{x, y, w, h};
    SDL_RenderFillRect(r, &rect);
}

// A box with cut corners and an outline, like the game's buttons.
void box(SDL_Renderer* r, float x, float y, float w, float h, Colour c, Colour outline) {
    fill(r, x + 2, y, w - 4, h, outline);
    fill(r, x, y + 2, w, h - 4, outline);
    fill(r, x + 1, y + 1, w - 2, h - 2, outline);
    fill(r, x + 3, y + 1, w - 6, h - 2, c);
    fill(r, x + 1, y + 3, w - 2, h - 6, c);
    fill(r, x + 2, y + 2, w - 4, h - 4, c);
}

// The SDL font has ASCII only; umlauts are the base letter with two dots, ß is "ss".
void text(SDL_Renderer* r, float x, float y, const std::string& s, Colour c, float scale = 1) {
    set(r, c);
    SDL_SetRenderScale(r, scale, scale);
    float at = x / scale;
    const float top = y / scale;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char ch = static_cast<unsigned char>(s[i]);
        char base = 0;
        bool dots = false;
        if (ch == 0xC3 && i + 1 < s.size()) {
            const unsigned char next = static_cast<unsigned char>(s[++i]);
            switch (next) {
                case 0xA4: base = 'a'; dots = true; break;
                case 0xB6: base = 'o'; dots = true; break;
                case 0xBC: base = 'u'; dots = true; break;
                case 0x84: base = 'A'; dots = true; break;
                case 0x96: base = 'O'; dots = true; break;
                case 0x9C: base = 'U'; dots = true; break;
                case 0x9F: {
                    SDL_RenderDebugText(r, at, top, "ss");
                    at += 16;
                    continue;
                }
                default: base = '?';
            }
        } else if (ch >= 0x20 && ch < 0x7F) {
            base = char(ch);
        } else {
            continue;
        }
        const char one[2] = {base, 0};
        SDL_RenderDebugText(r, at, top, one);
        if (dots) {
            const float dy = (base >= 'a') ? top + 1 : top - 1;
            const SDL_FRect a{at + 2, dy, 1, 1}, b{at + 5, dy, 1, 1};
            SDL_RenderFillRect(r, &a);
            SDL_RenderFillRect(r, &b);
        }
        at += 8;
    }
    SDL_SetRenderScale(r, 1, 1);
}

float text_width(const std::string& s, float scale = 1) {
    float n = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(s[i]);
        if (ch == 0xC3 && i + 1 < s.size()) {
            n += static_cast<unsigned char>(s[++i]) == 0x9F ? 2 : 1;
        } else if (ch >= 0x20 && ch < 0x7F) {
            n += 1;
        }
    }
    return n * 8 * scale;
}

void centred(SDL_Renderer* r, float y, const std::string& s, Colour c, float scale = 1) {
    text(r, (kWidth - text_width(s, scale)) / 2, y, s, c, scale);
}

void background(SDL_Renderer* r) {
    fill(r, 0, 0, kWidth, kHeight, kGround);
    for (int y = 0; y < int(kHeight); y += 16) {
        for (int x = (y / 16) % 2 * 16; x < int(kWidth); x += 32) fill(r, float(x), float(y), 16, 16, kGroundDark);
    }
    fill(r, 0, 191, kWidth, 2, kInk);  // the gap between the two screens
}

// --- Immediate-mode menu: items are laid out each frame; one has the focus ---
struct Input {
    bool up = false, down = false, left = false, right = false, activate = false, back = false;
    bool backspace = false;
    std::string typed;
    float mouse_x = -1, mouse_y = -1;
    bool clicked = false;
    bool moved = false;
};

class Menu {
public:
    Menu(SDL_Renderer* r, const Input& in, int& focus) : r_(r), in_(in), focus_(focus) {}

    // A button; returns true when chosen.
    bool button(float x, float y, float w, const std::string& label, bool enabled = true) {
        const int index = count_++;
        const bool hover = inside(x, y, w, 18);
        if (hover && in_.moved && enabled) focus_ = index;
        const bool focused = focus_ == index;
        box(r_, x, y, w, 18, !enabled ? kGrey : focused ? kButtonFocus : kButton, kInk);
        text(r_, x + (w - text_width(label)) / 2, y + 5, label, enabled ? kInk : kPanel);
        enabled_.push_back(enabled);
        return enabled && ((focused && in_.activate) || (hover && in_.clicked));
    }

    // A row with a value and arrows; returns -1, 0 or +1.
    int selector(float x, float y, float w, const std::string& label, const std::string& value) {
        const int index = count_++;
        const bool hover = inside(x, y, w, 18);
        if (hover && in_.moved) focus_ = index;
        const bool focused = focus_ == index;
        box(r_, x, y, w, 18, focused ? kButtonFocus : kPanel, kInk);
        text(r_, x + 6, y + 5, label, kInk);
        const float value_x = x + w - 18 - text_width(value) - 12;
        text(r_, x + w - 96, y + 5, "<", kInk);
        text(r_, std::max(value_x, x + w - 84), y + 5, value, kInk);
        text(r_, x + w - 14, y + 5, ">", kInk);
        enabled_.push_back(true);
        int change = 0;
        if (focused && in_.left) change = -1;
        if (focused && in_.right) change = 1;
        if (in_.clicked && inside(x + w - 100, y, 24, 18)) change = -1;
        if (in_.clicked && inside(x + w - 20, y, 20, 18)) change = 1;
        if (hover && in_.clicked && change == 0) focus_ = index;
        return change;
    }

    // A text field; edits `value` while focused. `filter` decides which characters may go in.
    void field(float x, float y, float w, const std::string& label, std::string& value, size_t limit,
               const std::function<char(char)>& filter) {
        const int index = count_++;
        const bool hover = inside(x, y, w, 18);
        if (hover && in_.clicked) focus_ = index;
        const bool focused = focus_ == index;
        box(r_, x, y, w, 18, focused ? kButtonFocus : kPanel, kInk);
        text(r_, x + 6, y + 5, label, kInk);
        if (focused) {
            for (char c : in_.typed) {
                const char kept = filter(c);
                if (kept != 0 && value.size() < limit) value += kept;
            }
            if (in_.backspace && !value.empty()) value.pop_back();
        }
        const std::string shown = value + (focused && (SDL_GetTicks() / 400) % 2 ? "_" : "");
        text(r_, x + w - 8 - text_width(value + "_"), y + 5, shown, kInk);
        enabled_.push_back(true);
        typing_ = typing_ || focused;
    }

    // After all items: moves the focus with the keys.
    void finish() {
        if (count_ == 0) return;
        focus_ = std::clamp(focus_, 0, count_ - 1);
        auto step = [&](int delta) {
            for (int tries = 0; tries < count_; ++tries) {
                focus_ = (focus_ + delta + count_) % count_;
                if (enabled_[size_t(focus_)]) break;
            }
        };
        if (in_.down) step(1);
        if (in_.up) step(-1);
    }

    bool typing() const { return typing_; }

private:
    bool inside(float x, float y, float w, float h) const {
        return in_.mouse_x >= x && in_.mouse_x < x + w && in_.mouse_y >= y && in_.mouse_y < y + h;
    }
    SDL_Renderer* r_;
    const Input& in_;
    int& focus_;
    int count_ = 0;
    std::vector<bool> enabled_;
    bool typing_ = false;
};

char name_char(char c) { return (c >= 0x20 && c < 0x7F) ? c : 0; }
char server_char(char c) {
    return ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == ':' || c == '-')
               ? c : 0;
}
char code_char(char c) {
    if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    return lobby::valid_code_char(c) ? c : 0;
}

enum class Screen { Main, Online, Join, Room, Settings };

void panel(SDL_Renderer* r, float y, float h) { box(r, 12, y, kWidth - 24, h, kPanel, kInk); }

void top_screen(SDL_Renderer* r, const std::string& subtitle) {
    centred(r, 40, "Mario Party DS", kTitle, 2);
    centred(r, 64, "PC", kInk, 1);
    if (!subtitle.empty()) {
        panel(r, 100, 28);
        centred(r, 110, subtitle, kInk);
    }
}

}  // namespace

void draw_question(SDL_Renderer* renderer, const std::string& first, const std::string& second) {
    box(renderer, 16, 160, kWidth - 32, 64, kPanel, kInk);
    centred(renderer, 176, first, kInk);
    centred(renderer, 196, second, kInk);
}

void draw_message(SDL_Renderer* renderer, const std::string& message, const std::string& last) {
    const float limit = kWidth - 48;
    std::vector<std::string> lines;
    std::string line, word;
    auto put_word = [&] {
        if (word.empty()) return;
        const std::string longer = line.empty() ? word : line + " " + word;
        if (!line.empty() && text_width(longer) > limit) {
            lines.push_back(line);
            line = word;
        } else {
            line = longer;
        }
        word.clear();
    };
    for (char ch : message) {
        if (ch == ' ') put_word();
        else word += ch;
    }
    put_word();
    if (!line.empty()) lines.push_back(line);
    if (!last.empty()) lines.push_back(""), lines.push_back(last);
    const float height = 24 + 14 * float(lines.size());
    const float top = 192 - height / 2;
    box(renderer, 16, top, kWidth - 32, height, kPanel, kInk);
    for (size_t i = 0; i < lines.size(); ++i) centred(renderer, top + 12 + 14 * float(i), lines[i], kInk);
}

void split_server(const std::string& server, std::string& address, std::uint16_t& port) {
    address = server;
    port = lobby::kDefaultPort;
    const size_t colon = server.rfind(':');
    if (colon != std::string::npos) {
        address = server.substr(0, colon);
        const int value = std::atoi(server.c_str() + colon + 1);
        if (value > 0 && value < 65536) port = std::uint16_t(value);
    }
}

AppSettings load_settings(const std::filesystem::path& file) {
    AppSettings settings;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "name" && !value.empty()) settings.name = value.substr(0, size_t(lobby::kMaxName));
        else if (key == "server" && !value.empty()) settings.server = value;
        else if (key == "scale") settings.scale = std::clamp(std::atoi(value.c_str()), 1, 4);
        else if (key == "fullscreen") settings.fullscreen = value == "1";
        else if (key == "character") settings.character = std::clamp(std::atoi(value.c_str()), 0, lobby::kCharacters - 1);
        else if (key == "rom") settings.rom = value;
    }
    return settings;
}

void save_settings(const std::filesystem::path& file, const AppSettings& s) {
    std::ofstream out(file, std::ios::trunc);
    out << "name=" << s.name << "\nserver=" << s.server << "\nscale=" << s.scale << "\nfullscreen="
        << (s.fullscreen ? 1 : 0) << "\ncharacter=" << s.character << "\nrom=" << s.rom << "\n";
}

LaunchChoice run_launcher(SDL_Window* window, SDL_Renderer* renderer, AppSettings& settings,
                          const std::filesystem::path& settings_file, const std::string& rejoin_room) {
    LobbyClient client;
    Screen screen = Screen::Main;
    int focus = 0;
    std::string message;
    std::string code;
    enum class Pending { None, Create, Join } pending = Pending::None;
    bool ready = false;
    std::vector<SDL_Gamepad*> pads;
    bool text_input = false;
    AppSettings edited = settings;

    auto go = [&](Screen next) {
        screen = next;
        focus = 0;
    };

    for (;;) {
        Input in;
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_EVENT_QUIT: return {};
                case SDL_EVENT_KEY_DOWN:
                    switch (event.key.scancode) {
                        case SDL_SCANCODE_UP: in.up = true; break;
                        case SDL_SCANCODE_DOWN: case SDL_SCANCODE_TAB: in.down = true; break;
                        case SDL_SCANCODE_LEFT: in.left = true; break;
                        case SDL_SCANCODE_RIGHT: in.right = true; break;
                        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: in.activate = true; break;
                        case SDL_SCANCODE_ESCAPE: in.back = true; break;
                        case SDL_SCANCODE_BACKSPACE: in.backspace = true; break;
                        case SDL_SCANCODE_X: if (!text_input) in.activate = true; break;
                        case SDL_SCANCODE_Z: if (!text_input) in.back = true; break;
                        case SDL_SCANCODE_F11:
                            settings.fullscreen = !settings.fullscreen;
                            SDL_SetWindowFullscreen(window, settings.fullscreen);
                            break;
                        default: break;
                    }
                    break;
                case SDL_EVENT_TEXT_INPUT: in.typed += event.text.text; break;
                case SDL_EVENT_MOUSE_MOTION: in.moved = true; break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    if (event.button.button == SDL_BUTTON_LEFT) in.clicked = true;
                    break;
                case SDL_EVENT_GAMEPAD_ADDED: pads.push_back(SDL_OpenGamepad(event.gdevice.which)); break;
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                    switch (event.gbutton.button) {
                        case SDL_GAMEPAD_BUTTON_DPAD_UP: in.up = true; break;
                        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: in.down = true; break;
                        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: in.left = true; break;
                        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: in.right = true; break;
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
        SDL_RenderCoordinatesFromWindow(renderer, mx, my, &in.mouse_x, &in.mouse_y);

        client.poll();
        const LobbyClient::Phase phase = client.phase();
        if (phase == LobbyClient::Phase::Failed) {
            message = client.message();
            pending = Pending::None;
            if (screen == Screen::Room) go(Screen::Online);
            client.disconnect();
        }
        if (phase == LobbyClient::Phase::Menu && pending != Pending::None) {
            if (pending == Pending::Create) client.create(settings.character);
            else client.join(code, settings.character);
            pending = Pending::None;
        }
        if (!client.message().empty() && phase != LobbyClient::Phase::Failed) message = client.message();
        if (phase == LobbyClient::Phase::InRoom && screen != Screen::Room) {
            go(Screen::Room);
            ready = client.own_slot() == 0;
            message.clear();
        }
        if (phase == LobbyClient::Phase::Menu && screen == Screen::Room) go(Screen::Online);  // left, closed or expired
        if (phase == LobbyClient::Phase::Started) {
            for (SDL_Gamepad* pad : pads) SDL_CloseGamepad(pad);
            if (text_input) SDL_StopTextInput(window);
            save_settings(settings_file, settings);
            LaunchChoice choice;
            choice.kind = LaunchChoice::Kind::Online;
            choice.start = client.take_start();
            return choice;
        }

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        background(renderer);
        Menu menu(renderer, in, focus);
        const float bx = 40, bw = kWidth - 80;
        switch (screen) {
            case Screen::Main: {
                top_screen(renderer, "Hauptmenü");
                if (menu.button(bx, 214, bw, "Lokales Spiel")) {
                    for (SDL_Gamepad* pad : pads) SDL_CloseGamepad(pad);
                    if (text_input) SDL_StopTextInput(window);
                    LaunchChoice choice;
                    choice.kind = LaunchChoice::Kind::Local;
                    return choice;
                }
                if (menu.button(bx, 240, bw, "Online spielen")) { message.clear(); go(Screen::Online); }
                if (menu.button(bx, 266, bw, "Einstellungen")) { edited = settings; go(Screen::Settings); }
                if (menu.button(bx, 292, bw, "Beenden") || in.back) return {};
                if (!rejoin_room.empty()) {
                    centred(renderer, 162, "Laufende Partie: " + rejoin_room, kInk);
                    if (menu.button(bx, 322, bw, "Zurück in die Partie")) {
                        for (SDL_Gamepad* pad : pads) SDL_CloseGamepad(pad);
                        if (text_input) SDL_StopTextInput(window);
                        LaunchChoice choice;
                        choice.kind = LaunchChoice::Kind::Rejoin;
                        return choice;
                    }
                }
                break;
            }
            case Screen::Online: {
                top_screen(renderer, "Online spielen");
                text(renderer, 24, 140, "Server: " + settings.server, kInk);
                text(renderer, 24, 152, "Name:   " + settings.name, kInk);
                const int change = menu.selector(24, 204, kWidth - 48, "Figur", lobby::character_name(settings.character));
                if (change != 0) settings.character = (settings.character + change + lobby::kCharacters) % lobby::kCharacters;
                const bool busy = pending != Pending::None || phase == LobbyClient::Phase::Connecting;
                if (menu.button(bx, 232, bw, "Raum erstellen", !busy)) {
                    message = "Verbinde mit " + settings.server + " ...";
                    std::string address;
                    std::uint16_t port;
                    split_server(settings.server, address, port);
                    if (phase != LobbyClient::Phase::Menu) client.connect(address, port, settings.name);
                    pending = Pending::Create;
                }
                if (menu.button(bx, 258, bw, "Raum beitreten", !busy)) { message.clear(); go(Screen::Join); }
                if (menu.button(bx, 284, bw, "Zurück zum Hauptmenü") || in.back) {
                    client.disconnect();
                    pending = Pending::None;
                    go(Screen::Main);
                }
                break;
            }
            case Screen::Join: {
                top_screen(renderer, "Raum beitreten");
                centred(renderer, 140, "Einladungscode mit 6 Zeichen", kInk);
                menu.field(24, 204, kWidth - 48, "Code", code, size_t(lobby::kCodeLength), code_char);
                const bool busy = pending != Pending::None || phase == LobbyClient::Phase::Connecting;
                if (menu.button(bx, 232, bw, "Beitreten", code.size() == size_t(lobby::kCodeLength) && !busy)) {
                    message = "Verbinde mit " + settings.server + " ...";
                    std::string address;
                    std::uint16_t port;
                    split_server(settings.server, address, port);
                    if (phase != LobbyClient::Phase::Menu) client.connect(address, port, settings.name);
                    pending = Pending::Join;
                }
                if (menu.button(bx, 258, bw, "Zurück") || in.back) go(Screen::Online);
                break;
            }
            case Screen::Room: {
                const bool host = client.own_slot() == 0;
                centred(renderer, 12, "Einladungscode", kInk);
                box(renderer, 56, 26, kWidth - 112, 30, kPanel, kInk);
                centred(renderer, 33, client.code(), kTitle, 2);
                int present = 0, ready_count = 0;
                for (int i = 0; i < lobby::kMaxPlayers; ++i) {
                    const LobbySeat& seat = client.seats()[size_t(i)];
                    const float y = 66 + float(i) * 30;
                    box(renderer, 12, y, kWidth - 24, 26, seat.present ? kPlayer[i] : kGrey, kInk);
                    text(renderer, 18, y + 4, "P" + std::to_string(i + 1), kPanel);
                    if (!seat.present) {
                        text(renderer, 46, y + 9, "frei", kPanel);
                        continue;
                    }
                    ++present;
                    ready_count += seat.ready;
                    text(renderer, 46, y + 4, seat.name + (i == client.own_slot() ? " (du)" : ""), kPanel);
                    text(renderer, 46, y + 15, lobby::character_name(seat.character), kPanel);
                    const std::string state = i == 0 ? "Host" : seat.ready ? "bereit" : "wartet";
                    text(renderer, kWidth - 20 - text_width(state), y + 4, state, kPanel);
                    const std::string ping = std::to_string(seat.ping_ms) + " ms";
                    text(renderer, kWidth - 20 - text_width(ping), y + 15, ping, kPanel);
                }
                const int own = client.own_slot() >= 0 ? client.seats()[size_t(client.own_slot())].character : 0;
                const int change = menu.selector(24, 204, kWidth - 48, "Figur", lobby::character_name(own));
                if (change != 0) {
                    const int next = (own + change + lobby::kCharacters) % lobby::kCharacters;
                    client.pick(next);
                    settings.character = next;
                }
                if (host) {
                    const bool can = present >= 2 && ready_count == present;
                    if (menu.button(bx, 232, bw, "Spiel starten", can)) client.start();
                    if (!can) centred(renderer, 292, present < 2 ? "Warte auf Mitspieler ..." : "Warte, bis alle bereit sind ...", kInk);
                } else {
                    if (menu.button(bx, 232, bw, ready ? "Nicht bereit" : "Bereit")) {
                        ready = !ready;
                        client.set_ready(ready);
                    }
                    centred(renderer, 292, "Der Host startet das Spiel.", kInk);
                }
                if (menu.button(bx, 258, bw, "Raum verlassen") || in.back) {
                    client.leave();
                    message.clear();
                }
                break;
            }
            case Screen::Settings: {
                top_screen(renderer, "Einstellungen");
                menu.field(24, 204, kWidth - 48, "Name", edited.name, size_t(lobby::kMaxName), name_char);
                menu.field(24, 226, kWidth - 48, "Server", edited.server, 60, server_char);
                const int scale = menu.selector(24, 248, kWidth - 48, "Fenster", std::to_string(edited.scale) + "x");
                if (scale != 0) edited.scale = std::clamp(edited.scale + scale, 1, 4);
                const int full = menu.selector(24, 270, kWidth - 48, "Vollbild", edited.fullscreen ? "an" : "aus");
                if (full != 0) edited.fullscreen = !edited.fullscreen;
                if (menu.button(bx, 298, bw, "Speichern", !edited.name.empty() && !edited.server.empty())) {
                    settings = edited;
                    save_settings(settings_file, settings);
                    SDL_SetWindowFullscreen(window, settings.fullscreen);
                    if (!settings.fullscreen) SDL_SetWindowSize(window, int(kWidth) * settings.scale, int(kHeight) * settings.scale);
                    go(Screen::Main);
                }
                if (menu.button(bx, 322, bw, "Zurück") || in.back) go(Screen::Main);
                break;
            }
        }
        menu.finish();
        if (menu.typing() != text_input) {
            text_input = menu.typing();
            if (text_input) SDL_StartTextInput(window);
            else SDL_StopTextInput(window);
        }
        if (!message.empty()) {
            box(renderer, 8, kHeight - 30, kWidth - 16, 22, kPanel, kInk);
            text(renderer, 14, kHeight - 23, message.substr(0, 29), kInk);
        }
        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }
}
