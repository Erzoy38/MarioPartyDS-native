// Upscaling of the DS picture (256 x 384, two screens one above the other) for the 2D parts:
//   kUpscaleEdges: Scale2x / Scale3x (EPX), pixel art with smooth diagonals, straight lines stay sharp
//   kUpscaleFsr:   AMD FidelityFX Super Resolution 1 (EASU edge-adaptive upscaling + RCAS sharpening),
//                  a CPU port of the open shaders (MIT licence, (c) Advanced Micro Devices, Inc.)
// Both run on the CPU (FSR on all cores), on any PC.
#pragma once

#include <cstdint>
#include <vector>

enum UpscaleMode { kUpscaleOff = 0, kUpscaleEdges = 1, kUpscaleFsr = 2 };

// `picture` is RGBA 256 x 384. The result is (256 * factor) x (384 * factor), factor 2 to 12. The two
// screens are scaled apart, so nothing bleeds between them.
void upscale_picture(const std::uint8_t* picture, int factor, int mode, std::vector<std::uint8_t>& out);
