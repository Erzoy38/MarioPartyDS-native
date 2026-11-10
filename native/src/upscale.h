// Pixel-art upscaling of the DS picture (Scale2x / Scale3x, also called EPX): every pixel becomes a
// block whose corners follow the diagonals of its neighbours, so stairs become smooth edges and
// straight lines stay sharp. Works on the CPU, on any PC.
#pragma once

#include <cstdint>
#include <vector>

// `picture` is RGBA, 256 x 384 (two screens one above the other). `factor` is 2, 3 or 4; the result
// is (256 * factor) x (384 * factor). The two screens are scaled apart, so nothing bleeds between them.
void upscale_picture(const std::uint8_t* picture, int factor, std::vector<std::uint8_t>& out);
