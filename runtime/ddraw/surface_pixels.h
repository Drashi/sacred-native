#pragma once
#include <cstdint>
#include <vector>

namespace sacred {
enum class PixelFormat { Xrgb8888, Argb8888, Rgb565, Argb1555, Argb4444 };
struct PixelDescription {
    uint32_t bits, red, green, blue, alpha;
};
PixelDescription describe(PixelFormat format);
struct PixelRect {
    int32_t left, top, right, bottom;
};
struct PixelImage {
    uint32_t width, height, pitch;
    PixelFormat format;
    std::vector<uint8_t> pixels;
};
std::vector<uint8_t> unpack(const PixelImage& image);
std::vector<uint8_t> unpack(const PixelImage& image, PixelRect rectangle);
void pack(PixelImage& image, const std::vector<uint8_t>& rgba);
void fill(PixelImage& image, PixelRect rectangle, uint32_t color);
void blit(PixelImage& destination, PixelRect dst, const PixelImage& source, PixelRect src, PixelRect clip);
}
