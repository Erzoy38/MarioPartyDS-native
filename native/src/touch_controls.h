// On-screen controls for phones and tablets (MOBILE_PORT.md, phase 4): the D-pad, A B X Y, L R,
// Start, Select and a menu button, worked with several fingers at once; a finger on the DS's
// bottom screen is the stylus. The player moves, sizes and fades the controls in an editor; each
// orientation keeps its own arrangement (touch_controls.ini in the app's folder).
//
// Everything is in the output's pixels (SDL_GetCurrentRenderOutputSize), drawn with the logical
// presentation switched off.
#pragma once

#include <SDL3/SDL.h>

#include "ui.h"

#include <array>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

class TouchControls {
public:
    enum class Action { None, SwapScreens, LeaveGame };

    void load(const std::filesystem::path& file);

    // Each picture: the output's size and the part of it that no notch or rounded corner covers.
    void set_output(float w, float h, const SDL_FRect& safe);
    bool portrait() const { return portrait_; }
    // Where the DS screens go: in portrait the upper part, the controls below; in landscape all of
    // the safe area, the controls at the sides over it. With the buttons hidden, all of it.
    SDL_FRect screen_area() const;
    bool buttons_hidden() const { return buttons_hidden_; }
    // Where the bottom DS screen is shown, for the stylus.
    void set_touch_screen(const SDL_FRect& rect) { touch_rect_ = rect; }

    // Finger events (SDL_EVENT_FINGER_*); anything else is ignored.
    Action handle(const SDL_Event& event);
    // The Android back button: opens the menu, or closes the menu or editor.
    void back();
    // A gamepad in use hides the controls until the screen is touched again.
    void set_gamepad_in_use(bool in_use);

    // While the menu or the editor is open the game gets no input from here.
    bool busy() const { return mode_ != Mode::Play; }
    Uint32 keys() const;  // DS key bits (ds_keys.h) the fingers hold
    bool stylus(Uint32& x, Uint32& y) const;  // the stylus on the DS screen, in DS pixels

    void draw(SDL_Renderer* renderer, ui::Text& text);

private:
    enum Control { kDpad, kFace, kL, kR, kSelect, kStart, kMenu, kControlCount };
    struct Place {
        float x = 0.5f, y = 0.5f;  // the middle, as parts of the safe area
        float size = 1.0f;
    };
    struct Arrangement {
        std::array<Place, kControlCount> places{};
        float opacity = 0.6f;
    };
    enum class Mode { Play, Menu, Edit };
    enum class Hold { None, Buttons, Dpad, Stylus, Drag, Tap };
    struct Finger {
        Hold hold = Hold::None;
        float x = 0, y = 0;
        Uint32 keys = 0;
        int target = -1;          // the control dragged, or the menu or toolbar item tapped
        float grab_x = 0, grab_y = 0;  // finger minus the control's middle, while dragging
    };
    struct Item {
        std::string label;
        SDL_FRect box;
    };

    static Arrangement default_arrangement(bool portrait);
    Arrangement& current() { return arrangements_[portrait_ ? 1 : 0]; }
    const Arrangement& current() const { return arrangements_[portrait_ ? 1 : 0]; }
    void save() const;

    float unit() const;  // the measure of all sizes: the safe area's shorter side
    SDL_FPoint centre(int control) const;
    SDL_FRect bounds(int control) const;
    bool shown(int control) const;
    // Control kL, kR, kSelect or kStart to its DS key: the names are the same, the values not.
    static Uint32 key_of(int control);
    bool menu_in_corner() const;  // buttons hidden while playing: the menu button small, top left
    int control_at(float x, float y) const;
    Uint32 button_keys_at(float x, float y) const;
    Uint32 dpad_keys_at(float x, float y) const;
    std::vector<Item> menu_items() const;
    std::vector<Item> toolbar_items() const;
    static int item_at(const std::vector<Item>& items, float x, float y);
    Action finger_down(SDL_FingerID id, float x, float y);
    Action finger_up(SDL_FingerID id, float x, float y);
    void finger_moved(SDL_FingerID id, float x, float y);
    void editor_tool(int item);

    void draw_controls(SDL_Renderer* renderer, ui::Text& text, float opacity);
    void draw_menu(SDL_Renderer* renderer, ui::Text& text);
    void draw_editor(SDL_Renderer* renderer, ui::Text& text);

    std::filesystem::path file_;
    std::array<Arrangement, 2> arrangements_{default_arrangement(false), default_arrangement(true)};
    float out_w_ = 0, out_h_ = 0;
    SDL_FRect safe_{};
    bool portrait_ = false;
    SDL_FRect touch_rect_{};
    Mode mode_ = Mode::Play;
    std::map<SDL_FingerID, Finger> fingers_;
    bool gamepad_in_use_ = false;
    bool buttons_hidden_ = false;   // the player hid them in the menu; the menu button stays
    bool leave_asked_ = false;      // "back to the main menu" was tapped once
    int selected_ = kFace;          // the control the editor's tools change
};
