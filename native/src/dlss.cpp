#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>

#include "dlss.h"

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#include <cstdio>

namespace {

// This program's own project id (GUID-like, as NGX asks of custom engines).
constexpr const char* kProjectId = "a808abdf-ccda-4171-92fb-1cc2dfd78f3d";
constexpr const char* kEngineVersion = "0.3";

ID3D11Device* g_device = nullptr;
NVSDK_NGX_Parameter* g_params = nullptr;
bool g_started = false, g_available = false;
std::string g_reason = "not started";

NVSDK_NGX_PerfQuality_Value quality_of(int mode) {
    switch (mode) {
        case kDlssDlaa: return NVSDK_NGX_PerfQuality_Value_DLAA;
        case kDlssQuality: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
        case kDlssBalanced: return NVSDK_NGX_PerfQuality_Value_Balanced;
        case kDlssPerformance: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
        default: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    }
}

std::string hex(NVSDK_NGX_Result r) {
    char text[16];
    std::snprintf(text, sizeof text, "%08x", unsigned(r));
    return text;
}

}  // namespace

const char* dlss_mode_name(int mode) {
    static const char* const kNames[kDlssModeCount] = {"Off", "DLAA", "Quality", "Balanced", "Performance",
                                                       "Ultra Performance"};
    return mode >= 0 && mode < kDlssModeCount ? kNames[mode] : "Off";
}

bool dlss_start(SDL_Renderer* renderer, const std::filesystem::path& data_folder) {
    if (g_started) return g_available;
    g_started = true;
    const char* name = SDL_GetRendererName(renderer);
    if (name == nullptr || SDL_strcmp(name, "direct3d11") != 0) {
        g_reason = "The picture does not use Direct3D 11.";
        return false;
    }
    g_device = static_cast<ID3D11Device*>(
        SDL_GetPointerProperty(SDL_GetRendererProperties(renderer), SDL_PROP_RENDERER_D3D11_DEVICE_POINTER, nullptr));
    if (g_device == nullptr) {
        g_reason = "No Direct3D 11 device.";
        return false;
    }
    // nvngx_dlss.dll is looked for next to the program (NGX's default path).
    const std::wstring logs = data_folder.wstring();
    NVSDK_NGX_Result r = NVSDK_NGX_D3D11_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion,
                                                              logs.c_str(), g_device);
    if (NVSDK_NGX_FAILED(r)) {
        g_reason = r == NVSDK_NGX_Result_FAIL_FeatureNotSupported ? "This graphics card has no DLSS (it needs an NVIDIA RTX card)."
                                                                   : "DLSS could not start (" + hex(r) + ").";
        std::printf("dlss: init failed %s\n", hex(r).c_str());
        g_device = nullptr;
        return false;
    }
    r = NVSDK_NGX_D3D11_GetCapabilityParameters(&g_params);
    if (NVSDK_NGX_FAILED(r) || g_params == nullptr) {
        g_reason = "DLSS gave no capabilities (" + hex(r) + ").";
        dlss_stop();
        return false;
    }
    int needs_driver = 0;
    unsigned min_major = 0, min_minor = 0;
    if (NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver))
        && needs_driver) {
        NVSDK_NGX_Parameter_GetUI(g_params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &min_major);
        NVSDK_NGX_Parameter_GetUI(g_params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &min_minor);
        g_reason = "DLSS needs a newer NVIDIA driver (" + std::to_string(min_major) + "." + std::to_string(min_minor) + " or later).";
        dlss_stop();
        return false;
    }
    int available = 0;
    NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    if (!available) {
        int init_result = 0;
        NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &init_result);
        g_reason = init_result == int(NVSDK_NGX_Result_FAIL_FeatureNotFound)
                       ? "nvngx_dlss.dll is missing next to the game."
                       : "DLSS is not available on this PC (" + hex(NVSDK_NGX_Result(init_result)) + ").";
        dlss_stop();
        return false;
    }
    g_available = true;
    g_reason.clear();
    std::printf("dlss: available\n");
    return true;
}

void dlss_stop() {
    if (g_params != nullptr) NVSDK_NGX_D3D11_DestroyParameters(g_params);
    g_params = nullptr;
    if (g_device != nullptr) NVSDK_NGX_D3D11_Shutdown1(g_device);
    g_device = nullptr;
    g_available = false;
}

bool dlss_available() { return g_available; }

const std::string& dlss_reason() { return g_reason; }

bool dlss_render_size(int mode, unsigned out_w, unsigned out_h, unsigned& w, unsigned& h) {
    if (!g_available || mode == kDlssOff) return false;
    unsigned max_w = 0, max_h = 0, min_w = 0, min_h = 0;
    float sharpness = 0;
    const NVSDK_NGX_Result r = NGX_DLSS_GET_OPTIMAL_SETTINGS(g_params, out_w, out_h, quality_of(mode), &w, &h, &max_w, &max_h,
                                                             &min_w, &min_h, &sharpness);
    return NVSDK_NGX_SUCCEED(r) && w > 0 && h > 0;
}
