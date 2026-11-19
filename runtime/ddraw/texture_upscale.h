#pragma once
#include <cstdint>
#include <vector>

namespace sacred {
// The texture builder of tools/textures/texture_pack_build.py, in the engine (TEXTURE_UPSCALE): an RGBA texture
// (top row first) magnified by an integer factor 2..4 with Lanczos, atlas-safe. The steps and the arithmetic are the
// script's: transparent pixels take the colour of the nearest visible ones (six rounds of 8-neighbour averaging in
// float, then rounded half to even), the image is padded by 4 replicated pixels, each channel is resampled like
// Pillow's Image.resize(LANCZOS) (22-bit fixed-point coefficients, horizontal pass then vertical, 8-bit between),
// alpha is cut back to the original coverage grown by one output pixel, and fully transparent output pixels show the
// original colour upscaled and clamped to each source pixel's 3x3 range. tests/menu/texture_upscale_tests.cpp checks
// it against the script's output.
std::vector<uint8_t> texture_upscale(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height, unsigned scale);
}
