#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>

#include "dlss.h"

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

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
    dlss_end();
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

// --- Upscaling ---
namespace {

struct Upscaler {
    NVSDK_NGX_Handle* feature = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11Texture2D* color = nullptr;   // RGBA8, input size
    ID3D11Texture2D* depth = nullptr;   // R32_FLOAT, input size
    ID3D11Texture2D* motion = nullptr;  // R16G16_FLOAT, input size, all zero
    ID3D11Texture2D* output = nullptr;  // RGBA8, output size, written by DLSS
    SDL_Texture* shown = nullptr;       // `output` for SDL
    int in_w = 0, in_h = 0, out_w = 0, out_h = 0;
    bool first = true;
};
Upscaler g_up;

template <typename T>
void release(T*& p) {
    if (p != nullptr) p->Release();
    p = nullptr;
}

ID3D11Texture2D* make_texture(int w, int h, DXGI_FORMAT format, UINT bind, const void* initial, UINT pitch) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = UINT(w);
    desc.Height = UINT(h);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA data{initial, pitch, 0};
    ID3D11Texture2D* texture = nullptr;
    if (FAILED(g_device->CreateTexture2D(&desc, initial != nullptr ? &data : nullptr, &texture))) return nullptr;
    return texture;
}

// For finding the right direction of the sample point: MPDS_DLSS_JITTER=-1 turns it round.
float jitter_sign() {
    static const float sign = [] {
        const char* text = std::getenv("MPDS_DLSS_JITTER");
        return text != nullptr && std::atoi(text) < 0 ? -1.0f : 1.0f;
    }();
    return sign;
}

float halton(long long index, int base) {
    float f = 1, r = 0;
    for (long long i = index; i > 0; i /= base) {
        f /= float(base);
        r += f * float(i % base);
    }
    return r;
}

}  // namespace

SDL_Texture* dlss_begin(SDL_Renderer* renderer, int mode, int in_w, int in_h, int out_w, int out_h) {
    dlss_end();
    if (!g_available || mode == kDlssOff || in_w <= 0 || in_h <= 0 || out_w <= 0 || out_h <= 0) return nullptr;
    Upscaler& u = g_up;
    g_device->GetImmediateContext(&u.context);
    u.in_w = in_w;
    u.in_h = in_h;
    u.out_w = out_w;
    u.out_h = out_h;
    const std::vector<std::uint32_t> zero(size_t(in_w) * size_t(in_h), 0);
    u.color = make_texture(in_w, in_h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, nullptr, 0);
    u.depth = make_texture(in_w, in_h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, nullptr, 0);
    u.motion = make_texture(in_w, in_h, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE, zero.data(), UINT(in_w) * 4);
    u.output = make_texture(out_w, out_h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                            nullptr, 0);
    if (u.color == nullptr || u.depth == nullptr || u.motion == nullptr || u.output == nullptr) {
        g_reason = "DLSS textures could not be made.";
        dlss_end();
        return nullptr;
    }
    SDL_FlushRenderer(renderer);  // SDL's own work first; its state on the context is set again after
    NVSDK_NGX_DLSS_Create_Params create{};
    create.Feature.InWidth = unsigned(in_w);
    create.Feature.InHeight = unsigned(in_h);
    create.Feature.InTargetWidth = unsigned(out_w);
    create.Feature.InTargetHeight = unsigned(out_h);
    create.Feature.InPerfQualityValue = quality_of(mode);
    // Motion vectors at the input size, depth 0 near, no HDR; alpha is kept for the layers over 2D.
    create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure
                                  | NVSDK_NGX_DLSS_Feature_Flags_AlphaUpscaling;
    const NVSDK_NGX_Result r = NGX_D3D11_CREATE_DLSS_EXT(u.context, &u.feature, g_params, &create);
    if (NVSDK_NGX_FAILED(r) || u.feature == nullptr) {
        g_reason = "DLSS could not be set up for this size (" + hex(r) + ").";
        std::printf("dlss: create failed %s\n", hex(r).c_str());
        dlss_end();
        return nullptr;
    }
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_RGBA32);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STATIC);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, out_w);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, out_h);
    SDL_SetPointerProperty(props, SDL_PROP_TEXTURE_CREATE_D3D11_TEXTURE_POINTER, u.output);
    u.shown = SDL_CreateTextureWithProperties(renderer, props);
    SDL_DestroyProperties(props);
    if (u.shown == nullptr) {
        g_reason = std::string("DLSS output could not be shown: ") + SDL_GetError();
        dlss_end();
        return nullptr;
    }
    u.first = true;
    std::printf("dlss: %dx%d -> %dx%d, %s\n", in_w, in_h, out_w, out_h, dlss_mode_name(mode));
    return u.shown;
}

void dlss_end() {
    Upscaler& u = g_up;
    if (u.shown != nullptr) SDL_DestroyTexture(u.shown);
    u.shown = nullptr;
    if (u.feature != nullptr) NVSDK_NGX_D3D11_ReleaseFeature(u.feature);
    u.feature = nullptr;
    release(u.color);
    release(u.depth);
    release(u.motion);
    release(u.output);
    release(u.context);
}

bool dlss_evaluate(SDL_Renderer* renderer, const DlssFrame& frame) {
    Upscaler& u = g_up;
    if (u.feature == nullptr || frame.rgba == nullptr || frame.depth == nullptr) return false;
    SDL_FlushRenderer(renderer);
    u.context->UpdateSubresource(u.color, 0, nullptr, frame.rgba, UINT(u.in_w) * 4, 0);
    u.context->UpdateSubresource(u.depth, 0, nullptr, frame.depth, UINT(u.in_w) * 4, 0);
    NVSDK_NGX_D3D11_DLSS_Eval_Params eval{};
    eval.Feature.pInColor = u.color;
    eval.Feature.pInOutput = u.output;
    eval.pInDepth = u.depth;
    eval.pInMotionVectors = u.motion;
    // DLSS takes how far the picture moved, the opposite of where the sample point went (measured:
    // the other sign is further from the full-size picture in DLAA, Quality and Performance).
    eval.InJitterOffsetX = -frame.jitter_x * jitter_sign();
    eval.InJitterOffsetY = -frame.jitter_y * jitter_sign();
    eval.InRenderSubrectDimensions = {unsigned(u.in_w), unsigned(u.in_h)};
    eval.InReset = frame.reset || u.first ? 1 : 0;
    const NVSDK_NGX_Result r = NGX_D3D11_EVALUATE_DLSS_EXT(u.context, u.feature, g_params, &eval);
    if (NVSDK_NGX_FAILED(r)) {
        static int reported = 0;
        if (reported++ < 3) std::printf("dlss: evaluate failed %s\n", hex(r).c_str());
        return false;
    }
    u.first = false;
    return true;
}

void dlss_jitter(long long index, int in_w, int out_w, float& x, float& y) {
    // NVIDIA's advice: at least 8 samples per input pixel's share of the output.
    const float ratio = in_w > 0 ? float(out_w) / float(in_w) : 1.0f;
    const long long phases = std::max(8LL, (long long)std::ceil(8.0f * ratio * ratio));
    const long long i = index % phases + 1;  // Halton starts at 1: index 0 would be 0, 0
    x = halton(i, 2) - 0.5f;
    y = halton(i, 3) - 0.5f;
}
