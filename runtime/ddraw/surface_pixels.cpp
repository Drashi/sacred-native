#include "surface_pixels.h"
#include "../process/memory.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace sacred {
PixelDescription describe(PixelFormat format) {
    switch (format) {
    case PixelFormat::Xrgb8888:
        return {32, 0xff0000, 0xff00, 0xff, 0};
    case PixelFormat::Argb8888:
        return {32, 0xff0000, 0xff00, 0xff, 0xff000000};
    case PixelFormat::Rgb565:
        return {16, 0xf800, 0x7e0, 0x1f, 0};
    case PixelFormat::Argb1555:
        return {16, 0x7c00, 0x3e0, 0x1f, 0x8000};
    case PixelFormat::Argb4444:
        return {16, 0xf00, 0xf0, 0xf, 0xf000};
    }

    throw GuestFault("invalid packed pixel format");
}

namespace {
unsigned validate(const PixelImage& image) {
    unsigned bytes = describe(image.format).bits / 8;
    if (!image.width || !image.height || image.width > INT32_MAX || image.height > INT32_MAX ||
        uint64_t(image.width) * bytes > image.pitch || uint64_t(image.pitch) * image.height != image.pixels.size()) {
        throw GuestFault("invalid packed surface storage");
    }

    return bytes;
}

uint32_t load(const uint8_t* pixel, unsigned bytes) {
    uint32_t result = 0;
    for (unsigned i = 0; i < bytes; ++i) {
        result |= uint32_t(pixel[i]) << (i * 8);
    }

    return result;
}

void store(uint8_t* pixel, unsigned bytes, uint32_t value) {
    for (unsigned i = 0; i < bytes; ++i) {
        pixel[i] = uint8_t(value >> (i * 8));
    }
}

unsigned shift(uint32_t mask) {
    unsigned result = 0;
    while (!(mask & 1)) {
        ++result;
        mask >>= 1;
    }

    return result;
}

PixelRect intersect(PixelRect a, PixelRect b) {
    return {std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right), std::min(a.bottom, b.bottom)};
}

void rectangle(PixelRect value) {
    if (value.left >= value.right || value.top >= value.bottom) {
        throw GuestFault("invalid surface rectangle");
    }
}

// Per-channel conversion tables, bit-exact with the scalar formulas (expand: (sample*255 + max/2) / max,
// reduce: (byte*max + 127) / 255). The conversions run on every surface upload and GPU read-back, so the per-pixel
// work is one load, table lookups and one store.
struct ChannelCodec {
    uint32_t mask = 0;
    unsigned offset = 0;
    std::array<uint8_t, 256> expand{};  // sample (at most 8 bits) -> byte
    std::array<uint32_t, 256> reduce{}; // byte -> sample already shifted into place
};
struct FormatCodec {
    std::array<ChannelCodec, 4> channels;
    unsigned bytes = 0;
};
const FormatCodec& codec(PixelFormat format) {
    static const auto build = [](PixelFormat value) {
        FormatCodec result;
        auto description = describe(value);
        result.bytes = description.bits / 8;
        const uint32_t masks[] = {description.red, description.green, description.blue, description.alpha};
        for (unsigned channel = 0; channel < 4; ++channel) {
            ChannelCodec& c = result.channels[channel];
            c.mask = masks[channel];
            if (!c.mask) {
                c.expand.fill(255);
                continue;
            }

            c.offset = shift(c.mask);
            const uint32_t maximum = c.mask >> c.offset;
            if (maximum > 255) {
                throw GuestFault("unqualified pixel channel width");
            }

            for (uint32_t sample = 0; sample <= maximum; ++sample) {
                c.expand[sample] = uint8_t((sample * 255 + maximum / 2) / maximum);
            }

            for (uint32_t byte = 0; byte < 256; ++byte) {
                c.reduce[byte] = ((byte * maximum + 127) / 255) << c.offset;
            }
        }

        return result;
    };
    static const FormatCodec codecs[] = {build(PixelFormat::Xrgb8888), build(PixelFormat::Argb8888),
                                         build(PixelFormat::Rgb565), build(PixelFormat::Argb1555),
                                         build(PixelFormat::Argb4444)};
    switch (format) {
    case PixelFormat::Xrgb8888:
        return codecs[0];
    case PixelFormat::Argb8888:
        return codecs[1];
    case PixelFormat::Rgb565:
        return codecs[2];
    case PixelFormat::Argb1555:
        return codecs[3];
    case PixelFormat::Argb4444:
        return codecs[4];
    }

    throw GuestFault("invalid packed pixel format");
}

// 8-bit channels: expand and reduce are the identity, so (A)RGB8888 <-> RGBA is a byte swizzle.
void unpack_rows_8888(const PixelImage& image, PixelRect area, bool alpha, uint8_t* out) {
    for (uint32_t y = area.top; y < uint32_t(area.bottom); ++y) {
        const uint8_t* row = image.pixels.data() + size_t(y) * image.pitch + size_t(area.left) * 4;
        for (uint32_t x = area.left; x < uint32_t(area.right); ++x, row += 4, out += 4) {
            out[0] = row[2];
            out[1] = row[1];
            out[2] = row[0];
            out[3] = alpha ? row[3] : 255;
        }
    }
}

void pack_rows_8888(PixelImage& image, const uint8_t* in, bool alpha) {
    for (uint32_t y = 0; y < image.height; ++y) {
        uint8_t* row = image.pixels.data() + size_t(y) * image.pitch;
        for (uint32_t x = 0; x < image.width; ++x, row += 4, in += 4) {
            row[0] = in[2];
            row[1] = in[1];
            row[2] = in[0];
            row[3] = alpha ? in[3] : 0;
        }
    }
}

template <unsigned Bytes>
void unpack_rows(const PixelImage& image, PixelRect area, const FormatCodec& c, uint8_t* out) {
    for (uint32_t y = area.top; y < uint32_t(area.bottom); ++y) {
        const uint8_t* row = image.pixels.data() + size_t(y) * image.pitch + size_t(area.left) * Bytes;
        for (uint32_t x = area.left; x < uint32_t(area.right); ++x, row += Bytes, out += 4) {
            const uint32_t value = load(row, Bytes);
            for (unsigned channel = 0; channel < 4; ++channel) {
                const ChannelCodec& ch = c.channels[channel];
                out[channel] = ch.expand[(value & ch.mask) >> ch.offset];
            }
        }
    }
}

template <unsigned Bytes> void pack_rows(PixelImage& image, const uint8_t* in, const FormatCodec& c) {
    for (uint32_t y = 0; y < image.height; ++y) {
        uint8_t* row = image.pixels.data() + size_t(y) * image.pitch;
        for (uint32_t x = 0; x < image.width; ++x, row += Bytes, in += 4) {
            uint32_t value = 0;
            for (unsigned channel = 0; channel < 4; ++channel) {
                value |= c.channels[channel].reduce[in[channel]];
            }

            store(row, Bytes, value);
        }
    }
}
}

std::vector<uint8_t> unpack(const PixelImage& image) {
    return unpack(image, {0, 0, int32_t(image.width), int32_t(image.height)});
}

std::vector<uint8_t> unpack(const PixelImage& image, PixelRect area) {
    unsigned bytes = validate(image);
    rectangle(area);
    if (area.left < 0 || area.top < 0 || uint32_t(area.right) > image.width || uint32_t(area.bottom) > image.height) {
        throw GuestFault("invalid packed surface read rectangle");
    }

    const FormatCodec& c = codec(image.format);
    uint32_t width = area.right - area.left, height = area.bottom - area.top;
    std::vector<uint8_t> result(size_t(width) * height * 4);
    if (image.format == PixelFormat::Xrgb8888 || image.format == PixelFormat::Argb8888) {
        unpack_rows_8888(image, area, image.format == PixelFormat::Argb8888, result.data());
    } else if (bytes == 2) {
        unpack_rows<2>(image, area, c, result.data());
    } else {
        unpack_rows<4>(image, area, c, result.data());
    }

    return result;
}

void pack(PixelImage& image, const std::vector<uint8_t>& rgba) {
    unsigned bytes = validate(image);
    if (rgba.size() != uint64_t(image.width) * image.height * 4) {
        throw GuestFault("invalid RGBA surface storage");
    }

    const FormatCodec& c = codec(image.format);
    if (image.format == PixelFormat::Xrgb8888 || image.format == PixelFormat::Argb8888) {
        pack_rows_8888(image, rgba.data(), image.format == PixelFormat::Argb8888);
    } else if (bytes == 2) {
        pack_rows<2>(image, rgba.data(), c);
    } else {
        pack_rows<4>(image, rgba.data(), c);
    }
}

void fill(PixelImage& image, PixelRect area, uint32_t color) {
    unsigned bytes = validate(image);
    rectangle(area);
    area = intersect(area, {0, 0, int32_t(image.width), int32_t(image.height)});
    for (int32_t y = area.top; y < area.bottom; ++y) {
        for (int32_t x = area.left; x < area.right; ++x) {
            store(image.pixels.data() + size_t(y) * image.pitch + uint32_t(x) * bytes, bytes, color);
        }
    }
}

void blit(PixelImage& destination, PixelRect dst, const PixelImage& source, PixelRect src, PixelRect clip) {
    unsigned bytes = validate(destination);
    validate(source);
    rectangle(dst);
    rectangle(src);
    if (destination.format != source.format || src.left < 0 || src.top < 0 || uint32_t(src.right) > source.width ||
        uint32_t(src.bottom) > source.height) {
        throw GuestFault("unqualified surface conversion or source rectangle");
    }

    auto area = intersect(intersect(dst, clip), {0, 0, int32_t(destination.width), int32_t(destination.height)});
    std::vector<uint8_t> snapshot;
    const uint8_t* input = source.pixels.data();
    if (&source == &destination) {
        snapshot = source.pixels;
        input = snapshot.data();
    }

    uint64_t sw = int64_t(src.right) - src.left, sh = int64_t(src.bottom) - src.top;
    uint64_t dw = int64_t(dst.right) - dst.left, dh = int64_t(dst.bottom) - dst.top;
    if (area.left >= area.right || area.top >= area.bottom) {
        return;
    }

    // Nearest-sample source column for each destination column, once per blit instead of per pixel.
    std::vector<uint32_t> columns(size_t(area.right - area.left));
    for (int32_t x = area.left; x < area.right; ++x) {
        columns[size_t(x - area.left)] = src.left + uint32_t(((2 * (int64_t(x) - dst.left) + 1) * sw) / (2 * dw));
    }

    // Unscaled: (2k+1)*w / 2w == k, so each row is one contiguous copy.
    const bool unscaled = sw == dw;
    const size_t row_bytes = size_t(area.right - area.left) * bytes;
    for (int32_t y = area.top; y < area.bottom; ++y) {
        uint32_t sy = src.top + ((2 * (int64_t(y) - dst.top) + 1) * sh) / (2 * dh);
        const uint8_t* from = input + size_t(sy) * source.pitch;
        uint8_t* to = destination.pixels.data() + size_t(y) * destination.pitch + uint32_t(area.left) * bytes;
        if (unscaled) {
            std::memcpy(to, from + size_t(columns[0]) * bytes, row_bytes);
            continue;
        }

        for (size_t i = 0; i < columns.size(); ++i, to += bytes) {
            std::memcpy(to, from + size_t(columns[i]) * bytes, bytes);
        }
    }
}
}
