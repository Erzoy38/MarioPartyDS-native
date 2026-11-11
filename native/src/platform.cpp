#include "platform.h"

#include <SDL3/SDL.h>

#ifdef __ANDROID__
#include <jni.h>
#endif

namespace platform {

namespace {

[[maybe_unused]] std::string base_path() {
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

std::string keep_rom(const std::string& chosen, std::string& error) {
    if constexpr (!kMobile) {
        return chosen;
    } else {
        const std::filesystem::path kept = user_folder() / "rom.nds";
        const std::string kept_text = kept.string();  // UTF-8 on Android and iOS
        if (chosen == kept_text) return chosen;
        std::size_t size = 0;
        void* data = SDL_LoadFile(chosen.c_str(), &size);
        if (data == nullptr) {
            error = std::string("The file could not be read: ") + SDL_GetError();
            return {};
        }
        const bool saved = SDL_SaveFile(kept_text.c_str(), data, size);
        SDL_free(data);
        if (!saved) {
            error = std::string("The ROM could not be kept in the app's storage: ") + SDL_GetError();
            return {};
        }
        return kept_text;
    }
}

bool restart_app() {
#ifdef __ANDROID__
    JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());  // a local reference
    if (env == nullptr || activity == nullptr) return false;
    bool started = false;
    jclass type = env->GetObjectClass(activity);
    const jmethodID restart = env->GetMethodID(type, "restartApp", "()V");
    if (restart != nullptr) {
        env->CallVoidMethod(activity, restart);
        started = !env->ExceptionCheck();
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(type);
    env->DeleteLocalRef(activity);
    return started;
#else
    return false;
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
