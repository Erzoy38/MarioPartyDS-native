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
