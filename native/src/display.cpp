#include "display.h"

#include "gpu3d.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

const char* layout_name(int layout) {
    switch (layout) {
        case kLayoutSideBySide: return "Side by side";
        case kLayoutHybrid: return "Hybrid (big + small)";
        default: return "Stacked";
    }
}

const char* aspect_name(int aspect) {
    switch (aspect) {
        case kAspect16x9: return "16:9";
        case kAspectFill: return "Widescreen (fill)";
        default: return "4:3 (original)";
    }
}

ScreenRects screen_rects(float ow, float oh, int layout, int aspect) {
    // In units where a screen is 3 high: 4 wide at 4:3, 16/3 wide at 16:9.
    const float w = aspect == kAspect16x9 ? 16.0f / 3.0f : 4.0f, h = 3.0f;
    float aw = w, ah = 2 * h;
    SDL_FRect top{0, 0, w, h}, bottom{0, h, w, h};
    if (layout == kLayoutSideBySide) {
        aw = 2 * w;
        ah = h;
        bottom = {w, 0, w, h};
    } else if (layout == kLayoutHybrid) {
        aw = 3 * w;
        ah = 2 * h;
        top = {0, 0, 2 * w, 2 * h};
        bottom = {2 * w, h / 2, w, h};
    }
    float kx = ow / aw, ky = oh / ah;
    if (aspect != kAspectFill) kx = ky = std::min(kx, ky);
    const float ox = (ow - aw * kx) / 2, oy = (oh - ah * ky) / 2;
    const auto place = [&](const SDL_FRect& r) { return SDL_FRect{ox + r.x * kx, oy + r.y * ky, r.w * kx, r.h * ky}; };
    return {place(top), place(bottom)};
}

bool parse_resolution(const std::string& text, int& w, int& h) {
    return std::sscanf(text.c_str(), "%dx%d", &w, &h) == 2 && w >= 256 && h >= 192;
}

std::vector<std::pair<int, int>> display_resolutions() {
    std::set<std::pair<int, int>> found;
    int count = 0;
    SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(SDL_GetPrimaryDisplay(), &count);
    for (int i = 0; modes != nullptr && i < count; ++i) found.insert({modes[i]->w, modes[i]->h});
    SDL_free(modes);
    if (found.empty()) {
        for (auto r : {std::pair{1280, 720}, std::pair{1366, 768}, std::pair{1600, 900}, std::pair{1920, 1080},
                       std::pair{2560, 1440}, std::pair{3840, 2160}}) {
            found.insert(r);
        }
    }
    return {found.begin(), found.end()};
}

void automatic_window_size(int layout, int aspect, int scale, int& w, int& h) {
    const int sw = (aspect == kAspect16x9 ? 341 : 256) * scale, sh = 192 * scale;
    if (layout == kLayoutSideBySide) {
        w = 2 * sw;
        h = sh;
    } else if (layout == kLayoutHybrid) {
        w = 3 * sw / 2;
        h = sh;
    } else {
        w = sw;
        h = 2 * sh;
    }
}

void apply_window_mode(SDL_Window* window, const AppSettings& settings, int scale, bool fullscreen) {
    int w = 0, h = 0;
    const bool chosen = parse_resolution(settings.resolution, w, h);
    if (fullscreen) {
        SDL_DisplayMode mode{};
        if (chosen && SDL_GetClosestFullscreenDisplayMode(SDL_GetDisplayForWindow(window), w, h, 0, false, &mode)) {
            SDL_SetWindowFullscreenMode(window, &mode);  // the screen switches to this resolution
        } else {
            SDL_SetWindowFullscreenMode(window, nullptr);  // the desktop's resolution
        }
        SDL_SetWindowFullscreen(window, true);
        return;
    }
    SDL_SetWindowFullscreen(window, false);
    if (!chosen) automatic_window_size(settings.layout, settings.aspect, scale, w, h);
    SDL_SetWindowSize(window, w, h);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

// --- Texture packs ---
namespace {

// A path as UTF-8 in a plain string, for SDL.
std::string utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::map<u64, std::filesystem::path> g_pack;  // key -> file
std::filesystem::path g_dump_folder;
u64 g_dumped = 0;

std::string key_text(u64 key) {
    char text[17];
    SDL_snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(key));
    return text;
}

// A file whose name starts with the 16 hex digits of a key belongs to that texture.
bool key_from_name(const std::string& name, u64& key) {
    if (name.size() < 16) return false;
    key = 0;
    for (int i = 0; i < 16; ++i) {
        const char c = char(std::tolower(static_cast<unsigned char>(name[size_t(i)])));
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else return false;
        key = (key << 4) | u64(v);
    }
    return true;
}

bool load_replacement(u64 key, int& w, int& h, std::vector<u32>& rgba) {
    const auto found = g_pack.find(key);
    if (found == g_pack.end()) return false;
    const std::string file = utf8(found->second);
    SDL_Surface* loaded = SDL_LoadPNG(file.c_str());
    if (loaded == nullptr) {
        std::printf("texture pack: %s: %s\n", file.c_str(), SDL_GetError());
        return false;
    }
    SDL_Surface* surface = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(loaded);
    if (surface == nullptr) return false;
    w = surface->w;
    h = surface->h;
    rgba.resize(size_t(w) * size_t(h));
    for (int y = 0; y < h; ++y) {
        std::memcpy(&rgba[size_t(y) * size_t(w)], static_cast<const u8*>(surface->pixels) + size_t(y) * size_t(surface->pitch),
                    size_t(w) * 4);
    }
    SDL_DestroySurface(surface);
    std::printf("texture pack: %s replaced (%dx%d)\n", key_text(key).c_str(), w, h);
    return true;
}

void dump_texture(u64 key, int w, int h, const std::vector<u32>& rgba) {
    const std::filesystem::path file = g_dump_folder / (key_text(key) + ".png");
    std::error_code error;
    if (std::filesystem::exists(file, error)) return;
    SDL_Surface* surface = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, const_cast<u32*>(rgba.data()), w * 4);
    if (surface == nullptr) return;
    if (SDL_SavePNG(surface, utf8(file).c_str())) ++g_dumped;
    SDL_DestroySurface(surface);
}

}  // namespace

std::filesystem::path texture_folder() {
    const char* base = SDL_GetBasePath();
    return std::filesystem::u8path(std::string(base ? base : "")) / "textures";
}

void texture_pack_start(int render_scale, bool replace, bool dump) {
    g_pack.clear();
    const std::filesystem::path folder = texture_folder();
    g_dump_folder = folder / "dump";
    std::error_code error;
    if (replace && std::filesystem::is_directory(folder, error)) {
        for (auto it = std::filesystem::recursive_directory_iterator(folder, error);
             !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
            if (it->is_directory() && it->path().filename() == "dump") {
                it.disable_recursion_pending();  // dumped textures are not a pack
                continue;
            }
            std::string extension = it->path().extension().string();
            for (char& c : extension) c = char(std::tolower(static_cast<unsigned char>(c)));
            u64 key = 0;
            if (it->is_regular_file() && extension == ".png" && key_from_name(it->path().filename().string(), key)) {
                g_pack.emplace(key, it->path());
            }
        }
    }
    if (dump) std::filesystem::create_directories(g_dump_folder, error);
    Gpu3dTextureHooks hooks;
    hooks.load = load_replacement;
    hooks.dump = dump_texture;
    gpu3d_set_display(render_scale, replace && !g_pack.empty(), dump, hooks);
    std::printf("display: 3D at %dx, texture pack %s (%zu files), dump %s\n", render_scale,
                replace ? "on" : "off", g_pack.size(), dump ? utf8(g_dump_folder).c_str() : "off");
}

void open_texture_folder() {
    const std::filesystem::path folder = texture_folder();
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    std::string url = "file:///";
    for (char c : utf8(folder)) {
        if (c == '\\') url += '/';
        else if (c == ' ') url += "%20";
        else url += c;
    }
    SDL_OpenURL(url.c_str());
}
