#include "texture_upscale.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sacred {
namespace {
constexpr int pad = 4; // replicated border, in source pixels
constexpr int extend_rounds = 6;
constexpr int precision_bits = 32 - 8 - 2; // Pillow's fixed point for 8-bit resampling

double sinc(double x) {
    if (x == 0.0) {
        return 1.0;
    }

    x *= M_PI;
    return std::sin(x) / x;
}

double lanczos(double x) {
    return -3.0 <= x && x < 3.0 ? sinc(x) * sinc(x / 3.0) : 0.0;
}

// Pillow's precompute_coeffs + normalize_coeffs_8bpc for in_size -> out_size (box = whole image).
struct Coefficients {
    int ksize = 0;
    std::vector<int> bounds; // xmin, count per output pixel
    std::vector<int> k;      // ksize per output pixel
};
Coefficients coefficients(int in_size, int out_size) {
    Coefficients c;
    const double scale = double(in_size) / out_size;
    const double filterscale = scale < 1.0 ? 1.0 : scale;
    const double support = 3.0 * filterscale;
    c.ksize = int(std::ceil(support)) * 2 + 1;
    c.bounds.resize(size_t(out_size) * 2);
    c.k.assign(size_t(out_size) * c.ksize, 0);
    std::vector<double> weights(c.ksize);
    for (int xx = 0; xx < out_size; ++xx) {
        const double center = (xx + 0.5) * scale;
        const double ss = 1.0 / filterscale;
        int xmin = int(center - support + 0.5);
        if (xmin < 0) {
            xmin = 0;
        }

        int xmax = int(center + support + 0.5);
        if (xmax > in_size) {
            xmax = in_size;
        }

        xmax -= xmin;
        double ww = 0.0;
        for (int x = 0; x < xmax; ++x) {
            const double w = lanczos((x + xmin - center + 0.5) * ss);
            weights[x] = w;
            ww += w;
        }

        for (int x = 0; x < c.ksize; ++x) {
            double value = x < xmax ? (ww != 0.0 ? weights[x] / ww : weights[x]) : 0.0;
            c.k[size_t(xx) * c.ksize + x] =
                value < 0 ? int(-0.5 + value * (1 << precision_bits)) : int(0.5 + value * (1 << precision_bits));
        }

        c.bounds[size_t(xx) * 2] = xmin;
        c.bounds[size_t(xx) * 2 + 1] = xmax;
    }

    return c;
}

inline uint8_t clip8(int value) {
    const int shifted = value >> precision_bits;
    return uint8_t(shifted < 0 ? 0 : shifted > 255 ? 255 : shifted);
}

// One 8-bit plane (w x h) -> (w*s x h*s), as Image.fromarray(plane, "L").resize(..., LANCZOS).
std::vector<uint8_t> resize_plane(const std::vector<uint8_t>& in, int w, int h, int s, const Coefficients& horizontal,
                                  const Coefficients& vertical) {
    const int ow = w * s, oh = h * s;
    std::vector<uint8_t> temp(size_t(ow) * h), out(size_t(ow) * oh);
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = &in[size_t(y) * w];
        for (int xx = 0; xx < ow; ++xx) {
            const int xmin = horizontal.bounds[size_t(xx) * 2], count = horizontal.bounds[size_t(xx) * 2 + 1];
            const int* k = &horizontal.k[size_t(xx) * horizontal.ksize];
            int sum = 1 << (precision_bits - 1);
            for (int x = 0; x < count; ++x) {
                sum += row[xmin + x] * k[x];
            }

            temp[size_t(y) * ow + xx] = clip8(sum);
        }
    }

    for (int yy = 0; yy < oh; ++yy) {
        const int ymin = vertical.bounds[size_t(yy) * 2], count = vertical.bounds[size_t(yy) * 2 + 1];
        const int* k = &vertical.k[size_t(yy) * vertical.ksize];
        uint8_t* row = &out[size_t(yy) * ow];
        for (int xx = 0; xx < ow; ++xx) {
            int sum = 1 << (precision_bits - 1);
            for (int y = 0; y < count; ++y) {
                sum += temp[size_t(ymin + y) * ow + xx] * k[y];
            }

            row[xx] = clip8(sum);
        }
    }

    return out;
}

// texture_pack_build.resample: `channels` planes of an interleaved w x h image (stride `stride` bytes per pixel,
// starting at channel `first`), padded by replicating the border, resampled and cropped; interleaved result.
std::vector<uint8_t> resample(const std::vector<uint8_t>& image, int w, int h, int stride, int channels, int s) {
    const int pw = w + 2 * pad, ph = h + 2 * pad;
    const Coefficients horizontal = coefficients(pw, pw * s), vertical = coefficients(ph, ph * s);
    std::vector<uint8_t> out(size_t(w) * s * h * s * channels);
    std::vector<uint8_t> plane(size_t(pw) * ph);
    for (int c = 0; c < channels; ++c) {
        for (int y = 0; y < ph; ++y) {
            const int sy = std::clamp(y - pad, 0, h - 1);
            for (int x = 0; x < pw; ++x) {
                plane[size_t(y) * pw + x] = image[(size_t(sy) * w + std::clamp(x - pad, 0, w - 1)) * stride + c];
            }
        }

        const auto big = resize_plane(plane, pw, ph, s, horizontal, vertical);
        const int bw = pw * s;
        for (int y = 0; y < h * s; ++y) {
            for (int x = 0; x < w * s; ++x) {
                out[(size_t(y) * w * s + x) * channels + c] = big[size_t(y + pad * s) * bw + x + pad * s];
            }
        }
    }

    return out;
}

// texture_pack_build.extend, in the script's float32 arithmetic and order.
std::vector<uint8_t> extend(const std::vector<uint8_t>& rgba, int w, int h) {
    const size_t n = size_t(w) * h;
    std::vector<uint8_t> known(n);
    size_t visible = 0;
    for (size_t i = 0; i < n; ++i) {
        visible += (known[i] = rgba[i * 4 + 3] > 0);
    }

    if (visible == n || visible == 0) {
        return rgba;
    }

    std::vector<float> out(n * 4), total(n * 4);
    std::vector<float> count(n);
    for (size_t i = 0; i < n * 4; ++i) {
        out[i] = float(rgba[i]);
    }

    static const int directions[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {-1, 1}, {1, -1}, {1, 1}};
    std::vector<uint8_t> grow(n);
    for (int round = 0; round < extend_rounds; ++round) {
        std::fill(total.begin(), total.end(), 0.0f);
        std::fill(count.begin(), count.end(), 0.0f);
        for (const auto& d : directions) {
            const int dy = d[0], dx = d[1];
            for (int y = 0; y < h; ++y) {
                const int sy = y + dy;
                for (int x = 0; x < w; ++x) {
                    const int sx = x + dx;
                    const size_t i = size_t(y) * w + x;
                    float weight = 0.0f;
                    const float* value = nullptr;
                    if (sy >= 0 && sy < h && sx >= 0 && sx < w) {
                        const size_t j = size_t(sy) * w + sx;
                        weight = known[j] ? 1.0f : 0.0f;
                        value = &out[j * 4];
                    }

                    for (int c = 0; c < 4; ++c) {
                        total[i * 4 + c] += (value ? value[c] : 0.0f) * weight;
                    }

                    count[i] += weight;
                }
            }
        }

        bool any = false;
        for (size_t i = 0; i < n; ++i) {
            any |= (grow[i] = !known[i] && count[i] > 0.0f);
        }

        if (!any) {
            break;
        }

        for (size_t i = 0; i < n; ++i) {
            if (grow[i]) {
                for (int c = 0; c < 4; ++c) {
                    out[i * 4 + c] = total[i * 4 + c] / count[i];
                }

                known[i] = 1;
            }
        }
    }

    double mean[4] = {0, 0, 0, 0};
    size_t known_count = 0;
    for (size_t i = 0; i < n; ++i) {
        if (known[i]) {
            ++known_count;
            for (int c = 0; c < 4; ++c) {
                mean[c] += out[i * 4 + c];
            }
        }
    }

    std::vector<uint8_t> result(n * 4);
    for (size_t i = 0; i < n; ++i) {
        for (int c = 0; c < 4; ++c) {
            const float value = known[i] ? out[i * 4 + c] : float(mean[c] / double(known_count));
            result[i * 4 + c] = uint8_t(std::clamp(std::nearbyint(value), 0.0f, 255.0f));
        }
    }

    return result;
}
}

std::vector<uint8_t> texture_upscale(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height,
                                     unsigned scale) {
    const int w = int(width), h = int(height), s = int(scale);
    if (s < 2 || s > 4 || !w || !h || rgba.size() != size_t(w) * h * 4) {
        throw std::invalid_argument("texture_upscale: bad size or scale");
    }

    const size_t n = size_t(w) * h, on = n * s * s;
    const int ow = w * s;
    bool opaque = true;
    for (size_t i = 0; i < n && opaque; ++i) {
        opaque = rgba[i * 4 + 3] == 255;
    }

    std::vector<uint8_t> out(on * 4);
    if (opaque) {
        const auto rgb = resample(rgba, w, h, 4, 3, s);
        for (size_t i = 0; i < on; ++i) {
            out[i * 4] = rgb[i * 3];
            out[i * 4 + 1] = rgb[i * 3 + 1];
            out[i * 4 + 2] = rgb[i * 3 + 2];
            out[i * 4 + 3] = 255;
        }

        return out;
    }

    out = resample(extend(rgba, w, h), w, h, 4, 4, s);
    // Coverage magnified by nearest neighbour, grown by one output pixel (4-neighbourhood).
    auto covered = [&](int x, int y) {
        return rgba[(size_t(y / s) * w + x / s) * 4 + 3] > 0;
    };
    const int oh = h * s;
    std::vector<uint8_t> clear(on);
    bool any_clear = false;
    for (int y = 0; y < oh; ++y) {
        for (int x = 0; x < ow; ++x) {
            const size_t i = size_t(y) * ow + x;
            uint8_t& alpha = out[i * 4 + 3];
            if (covered(x, y)) {
                alpha = std::max<uint8_t>(alpha, 1);
            } else if (!((y > 0 && covered(x, y - 1)) || (y + 1 < oh && covered(x, y + 1)) ||
                         (x > 0 && covered(x - 1, y)) || (x + 1 < ow && covered(x + 1, y)))) {
                alpha = 0;
            }

            any_clear |= (clear[i] = alpha == 0);
        }
    }

    if (!any_clear) {
        return out;
    }

    // deringed(): the original colour upscaled, clamped to each source pixel's 3x3 range.
    const auto rgb = resample(rgba, w, h, 4, 3, s);
    for (int y = 0; y < oh; ++y) {
        for (int x = 0; x < ow; ++x) {
            const size_t i = size_t(y) * ow + x;
            if (!clear[i]) {
                continue;
            }

            const int sx = x / s, sy = y / s;
            for (int c = 0; c < 3; ++c) {
                uint8_t low = 255, high = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int px = std::clamp(sx + dx, 0, w - 1), py = std::clamp(sy + dy, 0, h - 1);
                        const uint8_t v = rgba[(size_t(py) * w + px) * 4 + c];
                        low = std::min(low, v);
                        high = std::max(high, v);
                    }
                }

                out[i * 4 + c] = std::clamp(rgb[i * 3 + c], low, high);
            }
        }
    }

    return out;
}
}
