// NVIDIA DLSS (Super Resolution and DLAA) through NVIDIA's DLSS SDK (NGX), on the Direct3D 11 device
// SDL uses. This part: starting it, finding out whether this PC can use it, the render size for a
// quality level, and stopping it. Without an NVIDIA RTX card, a fitting driver or nvngx_dlss.dll
// next to the program it reports why and the game keeps its own picture.
//
// This software uses NVIDIA DLSS. NVIDIA and DLSS are trademarks of NVIDIA Corporation.
#pragma once

#include <SDL3/SDL.h>

#include <filesystem>
#include <string>

// The choices in the settings, in their order there.
enum DlssMode { kDlssOff = 0, kDlssDlaa, kDlssQuality, kDlssBalanced, kDlssPerformance, kDlssUltraPerformance, kDlssModeCount };
const char* dlss_mode_name(int mode);

// Starts DLSS on the renderer's Direct3D 11 device. Safe to call on any renderer: false (and a
// reason) when it cannot be used. `data_folder` receives NGX's log files.
bool dlss_start(SDL_Renderer* renderer, const std::filesystem::path& data_folder);
void dlss_stop();  // before SDL ends

bool dlss_available();
const std::string& dlss_reason();  // why not, for the settings; empty when available

// The 3D size DLSS wants as input for an output of out_w x out_h at `mode`. False if unknown.
bool dlss_render_size(int mode, unsigned out_w, unsigned out_h, unsigned& w, unsigned& h);

// --- Upscaling ---
// Makes the DLSS feature for in_w x in_h to out_w x out_h and the textures it works on. Returns the
// SDL texture that receives each result (out_w x out_h, RGBA, alpha upscaled too), owned here, or
// null with dlss_reason() saying why.
SDL_Texture* dlss_begin(SDL_Renderer* renderer, int mode, int in_w, int in_h, int out_w, int out_h);
void dlss_end();  // frees the feature and its textures (also done by dlss_stop)

// One picture: RGBA and depth (0 near to 1 far) at the input size, drawn with the sample point
// moved by (jitter_x, jitter_y) input pixels. `reset`: nothing of the last pictures is to be kept.
// Motion vectors are none yet (everything counts as still).
struct DlssFrame {
    const unsigned char* rgba = nullptr;
    const float* depth = nullptr;
    float jitter_x = 0, jitter_y = 0;
    bool reset = false;
};
bool dlss_evaluate(SDL_Renderer* renderer, const DlssFrame& frame);

// The sample point for picture `index` (Halton 2, 3), in input pixels from -0.5 to 0.5, repeating
// after enough pictures for the ratio of output to input.
void dlss_jitter(long long index, int in_w, int out_w, float& x, float& y);
