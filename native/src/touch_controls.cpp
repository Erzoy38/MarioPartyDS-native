#include "touch_controls.h"

#include "ds_keys.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>

namespace {

constexpr float kScreensPart = 0.62f;  // portrait: the share of the height the DS screens may take

// Sizes as parts of the unit (TouchControls::unit), at size 1.
constexpr float kDpadRadius = 0.17f;
constexpr float kFaceRadius = 0.19f;    // the four buttons together
constexpr float kButtonRadius = 0.062f;
constexpr float kButtonOffset = 0.125f; // from the cluster's middle to each button's
constexpr float kShoulderW = 0.24f, kShoulderH = 0.085f;
constexpr float kSmallW = 0.17f, kSmallH = 0.065f;
constexpr float kMenuRadius = 0.05f;

constexpr const char* kNames[] = {"dpad", "face", "l", "r", "select", "start", "menu"};

struct FaceButton {
    float dx, dy;
    Uint32 key;
    const char* label;
};
// As on the DS: X at the top, A right, B at the bottom, Y left.
constexpr FaceButton kFaceButtons[] = {
    {0, -1, kX, "X"}, {1, 0, kA, "A"}, {0, 1, kB, "B"}, {-1, 0, kY, "Y"},
};

Uint8 alpha(float part) { return Uint8(std::clamp(part, 0.0f, 1.0f) * 255.0f + 0.5f); }

// With the buttons hidden, the menu button sits small in the top left corner and fainter.
constexpr float kHiddenMenuOpacity = 0.45f;

bool inside(const SDL_FRect& r, float x, float y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

SDL_FRect circle_box(SDL_FPoint c, float radius) { return {c.x - radius, c.y - radius, 2 * radius, 2 * radius}; }

// Text centred on (x, y); the line is about 1.36 em high (Ascent + Descent).
void centred_text(ui::Text& text, const std::string& s, float x, float y, const ui::TextStyle& style) {
    const float w = text.width(s, style);
    text.draw(s, x - w / 2, y - style.size * 0.68f, style);
}

}  // namespace

TouchControls::Arrangement TouchControls::default_arrangement(bool portrait) {
    Arrangement a;
    if (portrait) {
        a.places[kL] = {0.13f, 0.665f, 1};
        a.places[kR] = {0.87f, 0.665f, 1};
        a.places[kMenu] = {0.5f, 0.665f, 1};
        a.places[kDpad] = {0.25f, 0.81f, 1};
        a.places[kFace] = {0.75f, 0.81f, 1};
        a.places[kSelect] = {0.38f, 0.955f, 1};
        a.places[kStart] = {0.62f, 0.955f, 1};
    } else {
        a.places[kL] = {0.09f, 0.09f, 1};
        a.places[kR] = {0.91f, 0.09f, 1};
        a.places[kMenu] = {0.21f, 0.09f, 1};
        a.places[kDpad] = {0.12f, 0.6f, 1};
        a.places[kFace] = {0.88f, 0.6f, 1};
        a.places[kSelect] = {0.07f, 0.93f, 1};
        a.places[kStart] = {0.93f, 0.93f, 1};
    }
    a.opacity = 0.6f;
    return a;
}

void TouchControls::load(const std::filesystem::path& file) {
    file_ = file;
    std::ifstream in(file);
    std::string line;
    int section = -1;  // 0 landscape, 1 portrait
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "[landscape]") section = 0;
        else if (line == "[portrait]") section = 1;
        if (section < 0) continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        Arrangement& a = arrangements_[size_t(section)];
        if (key == "opacity") {
            a.opacity = std::clamp(float(std::atof(value.c_str())), 0.15f, 1.0f);
            continue;
        }
        for (int c = 0; c < kControlCount; ++c) {
            Place p;
            if (key == kNames[c] && std::sscanf(value.c_str(), "%f,%f,%f", &p.x, &p.y, &p.size) == 3) {
                a.places[size_t(c)] = {std::clamp(p.x, 0.0f, 1.0f), std::clamp(p.y, 0.0f, 1.0f),
                                       std::clamp(p.size, 0.5f, 2.0f)};
            }
        }
    }
}

void TouchControls::save() const {
    if (file_.empty()) return;
    std::ofstream out(file_, std::ios::trunc);
    out << "# Mario Party DS: the touch controls, as the editor left them\n";
    for (int section = 0; section < 2; ++section) {
        const Arrangement& a = arrangements_[size_t(section)];
        out << (section == 0 ? "[landscape]\n" : "[portrait]\n");
        char line[96];
        SDL_snprintf(line, sizeof line, "opacity=%.2f\n", double(a.opacity));
        out << line;
        for (int c = 0; c < kControlCount; ++c) {
            const Place& p = a.places[size_t(c)];
            SDL_snprintf(line, sizeof line, "%s=%.4f,%.4f,%.2f\n", kNames[c], double(p.x), double(p.y), double(p.size));
            out << line;
        }
    }
}

void TouchControls::set_output(float w, float h, const SDL_FRect& safe) {
    out_w_ = w;
    out_h_ = h;
    safe_ = (safe.w > 0 && safe.h > 0) ? safe : SDL_FRect{0, 0, w, h};
    const bool portrait = safe_.h > safe_.w;
    if (portrait != portrait_) fingers_.clear();  // turned: the fingers' places mean something else now
    portrait_ = portrait;
}

SDL_FRect TouchControls::screen_area() const {
    if (!portrait_ || buttons_hidden_) return safe_;  // hidden: the whole screen for the game
    return {safe_.x, safe_.y, safe_.w, safe_.h * kScreensPart};
}

bool TouchControls::menu_in_corner() const { return buttons_hidden_ && mode_ == Mode::Play; }

// Inside TouchControls, kL, kR, kSelect and kStart name the controls (2 to 5), not the DS keys;
// the keys are named through their enum here.
Uint32 TouchControls::key_of(int control) {
    switch (control) {
        case kL: return DsKey::kL;
        case kR: return DsKey::kR;
        case kSelect: return DsKey::kSelect;
        case kStart: return DsKey::kStart;
        default: return 0;
    }
}

float TouchControls::unit() const {
    // Landscape: the height, but on squarer tablets no more than the sides leave room for.
    // Portrait: the width, but no more than the part below the screens allows.
    return portrait_ ? std::min(safe_.w, safe_.h * 0.58f) : std::min(safe_.h, safe_.w * 0.45f);
}

SDL_FPoint TouchControls::centre(int control) const {
    const Place& p = current().places[size_t(control)];
    if (control == kMenu && menu_in_corner()) {
        const float r = kMenuRadius * p.size * unit();
        return {safe_.x + 1.6f * r, safe_.y + 1.6f * r};
    }
    return {safe_.x + p.x * safe_.w, safe_.y + p.y * safe_.h};
}

SDL_FRect TouchControls::bounds(int control) const {
    const float s = current().places[size_t(control)].size * unit();
    const SDL_FPoint c = centre(control);
    switch (control) {
        case kDpad: return circle_box(c, kDpadRadius * s);
        case kFace: return circle_box(c, kFaceRadius * s);
        case kMenu: return circle_box(c, kMenuRadius * s);
        case kL: case kR: return {c.x - kShoulderW * s / 2, c.y - kShoulderH * s / 2, kShoulderW * s, kShoulderH * s};
        default: return {c.x - kSmallW * s / 2, c.y - kSmallH * s / 2, kSmallW * s, kSmallH * s};
    }
}

bool TouchControls::shown(int control) const {
    if (mode_ == Mode::Edit) return true;
    if (gamepad_in_use_) return false;
    return control == kMenu || !buttons_hidden_;
}

int TouchControls::control_at(float x, float y) const {
    const float u = unit();
    for (int c = kControlCount - 1; c >= 0; --c) {
        if (!shown(c)) continue;
        const SDL_FRect b = bounds(c);
        if (c == kDpad || c == kFace || c == kMenu) {
            const SDL_FPoint m = centre(c);
            const float r = std::max(b.w / 2 * 1.1f, c == kMenu ? 0.065f * u : 0.0f);
            if (std::hypot(x - m.x, y - m.y) <= r) return c;
        } else {
            const float grow = 0.015f * u;
            if (inside({b.x - grow, b.y - grow, b.w + 2 * grow, b.h + 2 * grow}, x, y)) return c;
        }
    }
    return -1;
}

Uint32 TouchControls::button_keys_at(float x, float y) const {
    Uint32 keys = 0;
    if (shown(kFace)) {
        const float s = current().places[kFace].size * unit();
        const SDL_FPoint m = centre(kFace);
        // A little more than each button's circle, so that between two both are pressed.
        for (const FaceButton& b : kFaceButtons) {
            const float bx = m.x + b.dx * kButtonOffset * s, by = m.y + b.dy * kButtonOffset * s;
            if (std::hypot(x - bx, y - by) <= kButtonRadius * s * 1.45f) keys |= b.key;
        }
    }
    keys |= key_of(control_at(x, y));
    return keys;
}

Uint32 TouchControls::dpad_keys_at(float x, float y) const {
    const SDL_FPoint m = centre(kDpad);
    const float radius = bounds(kDpad).w / 2;
    const float dx = x - m.x, dy = y - m.y, length = std::hypot(dx, dy);
    if (length < radius * 0.2f) return 0;
    // Eight directions: a diagonal where the finger is within 22.5 degrees of it.
    constexpr float kSide = 0.38f;  // sin 22.5 degrees
    Uint32 keys = 0;
    if (dx / length > kSide) keys |= kRight;
    if (dx / length < -kSide) keys |= kLeft;
    if (dy / length > kSide) keys |= kDown;
    if (dy / length < -kSide) keys |= kUp;
    return keys;
}

std::vector<TouchControls::Item> TouchControls::menu_items() const {
    const float u = unit();
    const float h = 0.1f * u, gap = 0.02f * u;
    const float w = std::min(safe_.w * 0.9f, 0.85f * u);
    const std::vector<std::string> labels{
        "Weiter", "Bildschirme tauschen", "Tasten anpassen",
        buttons_hidden_ ? "Tasten einblenden" : "Tasten ausblenden",
        leave_asked_ ? "Wirklich? Noch einmal tippen" : "Zur\xC3\xBC" "ck zum Hauptmen\xC3\xBC"};
    const float total = float(labels.size()) * h + float(labels.size() - 1) * gap;
    float y = safe_.y + (safe_.h - total) / 2;
    std::vector<Item> items;
    for (const std::string& label : labels) {
        items.push_back({label, {safe_.x + (safe_.w - w) / 2, y, w, h}});
        y += h + gap;
    }
    return items;
}

std::vector<TouchControls::Item> TouchControls::toolbar_items() const {
    const float u = unit();
    const float w = 0.22f * u, h = 0.09f * u, gap = 0.02f * u;
    const char* const labels[] = {"Kleiner", "Gr\xC3\xB6\xC3\x9F" "er", "Blasser", "Kr\xC3\xA4" "ftiger", "Standard", "Fertig"};
    const float x0 = safe_.x + (safe_.w - (3 * w + 2 * gap)) / 2;
    const float y0 = safe_.y + (safe_.h - (2 * h + gap)) / 2;
    std::vector<Item> items;
    for (int i = 0; i < 6; ++i) {
        items.push_back({labels[i], {x0 + float(i % 3) * (w + gap), y0 + float(i / 3) * (h + gap), w, h}});
    }
    return items;
}

int TouchControls::item_at(const std::vector<Item>& items, float x, float y) {
    for (size_t i = 0; i < items.size(); ++i) {
        if (inside(items[i].box, x, y)) return int(i);
    }
    return -1;
}

TouchControls::Action TouchControls::handle(const SDL_Event& event) {
    if (out_w_ <= 0 || out_h_ <= 0) return Action::None;
    const float x = event.tfinger.x * out_w_, y = event.tfinger.y * out_h_;
    switch (event.type) {
        case SDL_EVENT_FINGER_DOWN: return finger_down(event.tfinger.fingerID, x, y);
        case SDL_EVENT_FINGER_MOTION: finger_moved(event.tfinger.fingerID, x, y); return Action::None;
        case SDL_EVENT_FINGER_UP: return finger_up(event.tfinger.fingerID, x, y);
        case SDL_EVENT_FINGER_CANCELED: fingers_.erase(event.tfinger.fingerID); return Action::None;
        default: return Action::None;
    }
}

TouchControls::Action TouchControls::finger_down(SDL_FingerID id, float x, float y) {
    Finger f;
    f.x = x;
    f.y = y;
    if (mode_ == Mode::Menu) {
        f.hold = Hold::Tap;
        f.target = item_at(menu_items(), x, y);
    } else if (mode_ == Mode::Edit) {
        const int tool = item_at(toolbar_items(), x, y);
        const int c = tool < 0 ? control_at(x, y) : -1;
        if (tool >= 0) {
            f.hold = Hold::Tap;
            f.target = tool;
        } else if (c >= 0) {
            selected_ = c;
            const SDL_FPoint m = centre(c);
            f.hold = Hold::Drag;
            f.target = c;
            f.grab_x = x - m.x;
            f.grab_y = y - m.y;
        }
    } else {
        gamepad_in_use_ = false;  // touched: the controls come back
        const int c = control_at(x, y);
        bool stylus_taken = false;
        for (const auto& [other, held] : fingers_) stylus_taken = stylus_taken || held.hold == Hold::Stylus;
        if (c == kMenu) {
            f.hold = Hold::Tap;
            f.target = kMenu;
        } else if (c == kDpad) {
            f.hold = Hold::Dpad;
            f.keys = dpad_keys_at(x, y);
        } else if (c >= 0) {
            f.hold = Hold::Buttons;
            f.keys = button_keys_at(x, y);
        } else if (!stylus_taken && inside(touch_rect_, x, y)) {
            f.hold = Hold::Stylus;
        }
    }
    fingers_[id] = f;
    return Action::None;
}

void TouchControls::finger_moved(SDL_FingerID id, float x, float y) {
    auto found = fingers_.find(id);
    if (found == fingers_.end()) return;
    Finger& f = found->second;
    f.x = x;
    f.y = y;
    if (f.hold == Hold::Dpad) f.keys = dpad_keys_at(x, y);
    else if (f.hold == Hold::Buttons) f.keys = button_keys_at(x, y);  // sliding from one button to the next
    else if (f.hold == Hold::Drag && f.target >= 0) {
        Place& p = current().places[size_t(f.target)];
        p.x = std::clamp((x - f.grab_x - safe_.x) / safe_.w, 0.0f, 1.0f);
        p.y = std::clamp((y - f.grab_y - safe_.y) / safe_.h, 0.0f, 1.0f);
    }
}

TouchControls::Action TouchControls::finger_up(SDL_FingerID id, float x, float y) {
    auto found = fingers_.find(id);
    if (found == fingers_.end()) return Action::None;
    const Finger f = found->second;
    fingers_.erase(found);
    if (f.hold != Hold::Tap) return Action::None;
    if (mode_ == Mode::Play && f.target == kMenu) {
        if (control_at(x, y) == kMenu) back();
        return Action::None;
    }
    if (mode_ == Mode::Edit) {
        if (f.target >= 0 && item_at(toolbar_items(), x, y) == f.target) editor_tool(f.target);
        return Action::None;
    }
    if (mode_ != Mode::Menu) return Action::None;
    const std::vector<Item> items = menu_items();
    const int item = item_at(items, x, y);
    if (f.target < 0 && item < 0) {  // tapped beside the menu: closes it
        mode_ = Mode::Play;
        return Action::None;
    }
    if (item != f.target) return Action::None;
    switch (item) {
        case 0: mode_ = Mode::Play; break;
        case 1: mode_ = Mode::Play; return Action::SwapScreens;
        case 2: mode_ = Mode::Edit; break;
        case 3: buttons_hidden_ = !buttons_hidden_; mode_ = Mode::Play; break;
        case 4:
            if (leave_asked_) return Action::LeaveGame;
            leave_asked_ = true;
            break;
        default: break;
    }
    return Action::None;
}

void TouchControls::editor_tool(int item) {
    Arrangement& a = current();
    Place& p = a.places[size_t(selected_)];
    switch (item) {
        case 0: p.size = std::max(0.5f, p.size - 0.1f); break;
        case 1: p.size = std::min(2.0f, p.size + 0.1f); break;
        case 2: a.opacity = std::max(0.15f, a.opacity - 0.1f); break;
        case 3: a.opacity = std::min(1.0f, a.opacity + 0.1f); break;
        case 4: a = default_arrangement(portrait_); break;
        case 5: save(); mode_ = Mode::Play; break;
        default: break;
    }
}

void TouchControls::back() {
    // Whatever the fingers held is let go: the menu and the editor take them.
    for (auto& [id, f] : fingers_) f = Finger{};
    if (mode_ == Mode::Play) {
        mode_ = Mode::Menu;
        leave_asked_ = false;
    } else {
        if (mode_ == Mode::Edit) save();
        mode_ = Mode::Play;
    }
}

void TouchControls::set_gamepad_in_use(bool in_use) {
    gamepad_in_use_ = in_use;
    if (in_use) {
        for (auto& [id, f] : fingers_) {
            if (f.hold == Hold::Buttons || f.hold == Hold::Dpad) f = Finger{};
        }
    }
}

Uint32 TouchControls::keys() const {
    if (busy()) return 0;
    Uint32 keys = 0;
    for (const auto& [id, f] : fingers_) keys |= f.keys;
    return keys;
}

bool TouchControls::stylus(Uint32& x, Uint32& y) const {
    if (busy() || touch_rect_.w <= 0 || touch_rect_.h <= 0) return false;
    for (const auto& [id, f] : fingers_) {
        if (f.hold != Hold::Stylus) continue;
        x = Uint32(std::clamp((f.x - touch_rect_.x) / touch_rect_.w * 256.0f, 0.0f, 255.0f));
        y = Uint32(std::clamp((f.y - touch_rect_.y) / touch_rect_.h * 192.0f, 0.0f, 191.0f));
        return true;
    }
    return false;
}

void TouchControls::draw(SDL_Renderer* renderer, ui::Text& text) {
    if (out_w_ <= 0 || out_h_ <= 0) return;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    if (mode_ == Mode::Play) draw_controls(renderer, text, current().opacity);
    else if (mode_ == Mode::Menu) draw_menu(renderer, text);
    else draw_editor(renderer, text);
}

void TouchControls::draw_controls(SDL_Renderer* renderer, ui::Text& text, float opacity) {
    Uint32 held = 0;
    for (const auto& [id, f] : fingers_) held |= f.keys;
    const ui::Rgba base{10, 12, 20, alpha(opacity * 0.55f)};
    const ui::Rgba pressed{255, 255, 255, alpha(opacity * 0.55f)};
    const ui::Rgba line{255, 255, 255, alpha(opacity * 0.8f)};
    ui::TextStyle label;
    label.fill = {255, 255, 255, alpha(opacity)};
    const float u = unit();
    for (int c = 0; c < kControlCount; ++c) {
        if (!shown(c)) continue;
        const float s = current().places[size_t(c)].size * u;
        const SDL_FPoint m = centre(c);
        const SDL_FRect b = bounds(c);
        if (c == kDpad) {
            const float r = b.w / 2;
            ui::fill_rounded(renderer, b, r, base);
            ui::outline_rounded(renderer, b, r, line);
            const float arm = 0.3f * r;
            const ui::Rgba cross{200, 205, 215, alpha(opacity * 0.35f)};
            ui::fill_rounded(renderer, {m.x - 0.85f * r, m.y - arm, 1.7f * r, 2 * arm}, arm * 0.4f, cross);
            ui::fill_rounded(renderer, {m.x - arm, m.y - 0.85f * r, 2 * arm, 1.7f * r}, arm * 0.4f, cross);
            const struct { Uint32 key; SDL_FRect part; } arms[] = {
                {kRight, {m.x + arm, m.y - arm, 0.85f * r - arm, 2 * arm}},
                {kLeft, {m.x - 0.85f * r, m.y - arm, 0.85f * r - arm, 2 * arm}},
                {kDown, {m.x - arm, m.y + arm, 2 * arm, 0.85f * r - arm}},
                {kUp, {m.x - arm, m.y - 0.85f * r, 2 * arm, 0.85f * r - arm}},
            };
            for (const auto& a : arms) {
                if (held & a.key) ui::fill_rounded(renderer, a.part, arm * 0.4f, pressed);
            }
        } else if (c == kFace) {
            const float r = kButtonRadius * s;
            label.font = ui::Font::Heavy;
            label.size = r * 0.95f;
            for (const FaceButton& fb : kFaceButtons) {
                const SDL_FPoint p{m.x + fb.dx * kButtonOffset * s, m.y + fb.dy * kButtonOffset * s};
                const SDL_FRect box = circle_box(p, r);
                ui::fill_rounded(renderer, box, r, (held & fb.key) ? pressed : base);
                ui::outline_rounded(renderer, box, r, line);
                centred_text(text, fb.label, p.x, p.y, label);
            }
        } else if (c == kMenu) {
            const float r = b.w / 2;
            const float k = menu_in_corner() ? kHiddenMenuOpacity : 1.0f;
            const ui::Rgba menu_base{base.r, base.g, base.b, Uint8(float(base.a) * k)};
            const ui::Rgba menu_line{line.r, line.g, line.b, Uint8(float(line.a) * k)};
            ui::fill_rounded(renderer, b, r, menu_base);
            ui::outline_rounded(renderer, b, r, menu_line);
            for (int i = -1; i <= 1; ++i) {
                ui::fill_rounded(renderer, {m.x - 0.45f * r, m.y + float(i) * 0.32f * r - 0.07f * r, 0.9f * r, 0.14f * r},
                                 0.07f * r, menu_line);
            }
        } else {
            ui::fill_rounded(renderer, b, b.h / 2, (held & key_of(c)) ? pressed : base);
            ui::outline_rounded(renderer, b, b.h / 2, line);
            label.font = ui::Font::Semibold;
            label.size = b.h * 0.45f;
            const char* name = c == kL ? "L" : c == kR ? "R" : c == kSelect ? "SELECT" : "START";
            centred_text(text, name, m.x, m.y, label);
        }
    }
}

void TouchControls::draw_menu(SDL_Renderer* renderer, ui::Text& text) {
    ui::fill_rounded(renderer, {0, 0, out_w_, out_h_}, 0, {0, 0, 0, 150});
    ui::TextStyle style;
    style.font = ui::Font::Semibold;
    const std::vector<Item> items = menu_items();
    for (size_t i = 0; i < items.size(); ++i) {
        const SDL_FRect& b = items[i].box;
        const bool leave = i + 1 == items.size();
        ui::fill_rounded(renderer, b, b.h * 0.3f, leave ? ui::Rgba{130, 40, 40, 235} : ui::Rgba{40, 46, 64, 235});
        ui::outline_rounded(renderer, b, b.h * 0.3f, {255, 255, 255, 120});
        style.size = b.h * 0.36f;
        centred_text(text, items[i].label, b.x + b.w / 2, b.y + b.h / 2, style);
    }
}

void TouchControls::draw_editor(SDL_Renderer* renderer, ui::Text& text) {
    ui::fill_rounded(renderer, {0, 0, out_w_, out_h_}, 0, {0, 0, 0, 90});
    draw_controls(renderer, text, std::max(current().opacity, 0.35f));
    // The chosen control, framed twice so the line shows on any background.
    const SDL_FRect b = bounds(selected_);
    ui::outline_rounded(renderer, {b.x - 3, b.y - 3, b.w + 6, b.h + 6}, 6, {255, 220, 40, 255});
    ui::outline_rounded(renderer, {b.x - 4, b.y - 4, b.w + 8, b.h + 8}, 6, {0, 0, 0, 200});
    const std::vector<Item> tools = toolbar_items();
    ui::TextStyle style;
    style.font = ui::Font::Semibold;
    for (const Item& item : tools) {
        ui::fill_rounded(renderer, item.box, item.box.h * 0.3f, {40, 46, 64, 235});
        ui::outline_rounded(renderer, item.box, item.box.h * 0.3f, {255, 255, 255, 120});
        style.size = item.box.h * 0.36f;
        centred_text(text, item.label, item.box.x + item.box.w / 2, item.box.y + item.box.h / 2, style);
    }
    ui::TextStyle hint;
    hint.font = ui::Font::Semibold;
    hint.size = tools.front().box.h * 0.34f;
    hint.outline = {0, 0, 0, 255};
    hint.outline_width = 2;
    const float top = tools.front().box.y;
    centred_text(text, "Ziehen: verschieben, Tippen: ausw\xC3\xA4hlen", safe_.x + safe_.w / 2,
                 top - hint.size * 1.2f, hint);
}
