// DLSS where there is none (Android, iOS): it reports why, and the game keeps its own picture.
// Same interface as dlss.cpp, which is built on Windows only.
#include "dlss.h"

namespace {
const std::string kReason = "only on Windows with an NVIDIA RTX card";
}

const char* dlss_mode_name(int mode) {
    static const char* const kNames[kDlssModeCount] = {"Off", "DLAA", "Quality", "Balanced", "Performance",
                                                       "Ultra Performance"};
    return mode >= 0 && mode < kDlssModeCount ? kNames[mode] : "Off";
}

bool dlss_start(SDL_Renderer*, const std::filesystem::path&) { return false; }
void dlss_stop() {}
bool dlss_available() { return false; }
const std::string& dlss_reason() { return kReason; }
bool dlss_render_size(int, unsigned, unsigned, unsigned&, unsigned&) { return false; }
SDL_Texture* dlss_begin(SDL_Renderer*, int, int, int, int, int) { return nullptr; }
void dlss_end() {}
bool dlss_evaluate(SDL_Renderer*, const DlssFrame&) { return false; }

void dlss_jitter(long long, int, int, float& x, float& y) {
    x = 0;
    y = 0;
}
