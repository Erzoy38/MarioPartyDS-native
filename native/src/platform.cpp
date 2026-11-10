#include "platform.h"

#include <SDL3/SDL.h>

namespace platform {

namespace {

std::string base_path() {
    const char* base = SDL_GetBasePath();
    return std::string(base ? base : "");
}

}  // namespace

std::filesystem::path user_folder() {
    if constexpr (kMobile) {
        char* pref = SDL_GetPrefPath("MarioPartyDS", "MarioPartyDS");
        std::filesystem::path path = pref ? std::filesystem::u8path(std::string(pref)) : std::filesystem::path(".");
        SDL_free(pref);
        return path;
    } else {
        return std::filesystem::u8path(base_path());
    }
}

std::string bundled_file(const std::string& name) {
#ifdef __ANDROID__
    return name;  // SDL reads relative names from the APK's assets
#else
    return base_path() + name;
#endif
}

const char* preferred_render_driver() {
#if defined(_WIN32)
    return "direct3d11";
#elif defined(__ANDROID__)
    return "vulkan";
#elif defined(__APPLE__)
    return "metal";
#else
    return nullptr;
#endif
}

}  // namespace platform
