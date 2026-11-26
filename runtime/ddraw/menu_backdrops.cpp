#include "menu_backdrops.h"
#include "texture_pack.h"
#include <set>
#include "texture_pak.h"
#include "../../platform/digest.h"
#include "../../platform/letterbox.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace sacred {
namespace {
std::string upper(std::string text) {
    for (char& c : text) {
        c = char(std::toupper(static_cast<unsigned char>(c)));
    }

    return text;
}

std::string lower(std::string text) {
    for (char& c : text) {
        c = char(std::tolower(static_cast<unsigned char>(c)));
    }

    return text;
}

std::vector<std::string> numbered(const char* format, int first) {
    std::vector<std::string> names;
    char name[64];
    for (int i = 0; i < 12; ++i) {
        std::snprintf(name, sizeof name, format, first + i);
        names.push_back(name);
    }

    return names;
}
}

bool water_floor_point(bool character, double u, double v, double& x, double& y) {
    // Measured at 1836x768 (tools: a floor coded R = u, G = v, three captures to take out the light added on top;
    // per-row fits to about one texel). The horizon is where the plane's depth runs out (u per pixel goes to 0).
    struct View {
        double horizon, slope, centre, a, b;
    };
    static constexpr View main_menu{191.11, 3.97357, 8.3, 2.01616, -650.083};
    static constexpr View character_select{421.19, 9.96099, 9.8, 1.44244, -155.969};
    const View& view = character ? character_select : main_menu;
    if (v >= view.a) {
        return false;
    }

    y = view.horizon + view.b / (v - view.a);
    x = view.centre + (u - 0.5) * view.slope * (y - view.horizon);
    return true;
}

namespace {
// The picture's colour at a canvas point (x from the canvas middle) minus the water's added light, bilinear; false
// where the picture is mostly opaque (the water does not show there) or off the picture.
bool picture_water(const Backdrop& picture, double x, double y, float* out) {
    if (y < 0 || y >= 768 || x + picture.logical_width() / 2 < 0 ||
        x + picture.logical_width() / 2 >= picture.logical_width()) {
        return false;
    }

    const double scale = double(picture.height) / 768.0;
    const double px = std::clamp((x + picture.logical_width() / 2) * scale - 0.5, 0.0, double(picture.width) - 1.001);
    const double py = std::clamp(y * scale - 0.5, 0.0, double(picture.height) - 1.001);
    const uint32_t x0 = uint32_t(px), y0 = uint32_t(py);
    const double fx = px - x0, fy = py - y0;
    double sum[4] = {0, 0, 0, 0};
    for (uint32_t k = 0; k < 4; ++k) {
        const uint8_t* p = &picture.rgba[(size_t(y0 + k / 2) * picture.width + x0 + k % 2) * 4];
        const double weight = (k % 2 ? fx : 1 - fx) * (k / 2 ? fy : 1 - fy);
        for (unsigned c = 0; c < 4; ++c) {
            sum[c] += p[c] * weight;
        }
    }

    if (sum[3] > 160) {
        return false;
    }

    // The water draw adds a light of 39 to the floor (measured on both main menus and the character select).
    for (unsigned c = 0; c < 3; ++c) {
        out[c] = float(std::max(0.0, sum[c] - 39.0));
    }

    out[3] = 255;
    return true;
}

bool usable(const Backdrop* picture) {
    return picture && picture->alpha && picture->height &&
           picture->rgba.size() >= size_t(picture->width) * picture->height * 4;
}
}

bool picture_shows_water(const Backdrop& picture, const std::vector<uint8_t>& floor, uint32_t w, uint32_t h,
                         bool character) {
    if (!usable(&picture) || floor.size() < size_t(w) * h * 4) {
        return false;
    }

    double n = 0, sp = 0, sf = 0, spp = 0, sff = 0, spf = 0;
    for (uint32_t row = 0; row < h; row += 2) {
        for (uint32_t column = 0; column < w; column += 2) {
            double x, y;
            float paint[4];
            if (!water_floor_point(character, (column + w / 2 + 0.5) / (2.0 * w), (row + 0.5) / h, x, y) ||
                !picture_water(picture, x, y, paint)) {
                continue;
            }

            const uint8_t* f = &floor[(size_t(row) * w + column) * 4];
            const double p = (paint[0] + paint[1] + paint[2]) / 3.0, q = (f[0] + f[1] + f[2]) / 3.0;
            n += 1;
            sp += p;
            sf += q;
            spp += p * p;
            sff += q * q;
            spf += p * q;
        }
    }

    if (n < 1000) {
        return false;
    }

    const double vp = spp / n - (sp / n) * (sp / n), vf = sff / n - (sf / n) * (sf / n);
    if (vp <= 0 || vf <= 0) {
        return false;
    }

    const double correlation = (spf / n - (sp / n) * (sf / n)) / std::sqrt(vp * vf);
    return correlation >= 0.3 && std::fabs(sp / n - sf / n) <= 25.0;
}

std::vector<uint8_t> wide_water_texture(const std::vector<uint8_t>& source, uint32_t w, uint32_t h, bool floor,
                                        const Backdrop* picture, bool character, bool painted) {
    const uint32_t wide = 2 * w;
    std::vector<uint8_t> rgba(size_t(wide) * h * 4);
    if (w < 8 || h < 8 || source.size() < size_t(w) * h * 4) {
        return rgba;
    }

    // The original, its two outer columns on each side taken from the third (both water textures have a light
    // column or two at their edges, outside the 4:3 screen in the original but on show on the canvas).
    std::vector<uint8_t> pixels = source;
    for (uint32_t row = 0; row < h; ++row) {
        uint8_t* line = &pixels[size_t(row) * w * 4];
        for (uint32_t c : {0u, 1u}) {
            std::copy(line + 2 * 4, line + 3 * 4, line + c * 4);
        }

        for (uint32_t c : {w - 2, w - 1}) {
            std::copy(line + (w - 3) * 4, line + (w - 2) * 4, line + c * 4);
        }
    }

    // Distance of a column from the original block (0 inside it) and its mirror image.
    auto outside = [&](uint32_t column) {
        const int64_t inner = int64_t(column) - int64_t(w / 2);
        return inner < 0 ? -inner : inner >= int64_t(w) ? inner - int64_t(w) + 1 : 0;
    };
    for (uint32_t row = 0; row < h; ++row) {
        for (uint32_t column = 0; column < wide; ++column) {
            const int64_t inner = int64_t(column) - int64_t(w / 2);
            const int64_t mirrored = inner < 0 ? -inner - 1 : inner >= int64_t(w) ? 2 * int64_t(w) - inner - 1 : inner;
            const uint8_t* own = &pixels[(size_t(row) * w + size_t(std::clamp<int64_t>(mirrored, 0, w - 1))) * 4];
            std::copy(own, own + 4, &rgba[(size_t(row) * wide + column) * 4]);
        }
    }

    // The sides' own content, (r, g, b, a) per texel, faded in after the mirrored band.
    std::vector<float> side;
    const bool paint = painted && usable(picture);
    if (paint) {
        side.assign(size_t(wide) * h * 4, 0.0f);
        std::vector<uint8_t> known(size_t(wide) * h, 0);
        for (uint32_t row = 0; row < h; ++row) {
            for (uint32_t column = 0; column < wide; ++column) {
                double x, y;
                if (outside(column) && water_floor_point(character, (column + 0.5) / wide, (row + 0.5) / h, x, y)) {
                    known[size_t(row) * wide + column] =
                        picture_water(*picture, x, y, &side[(size_t(row) * wide + column) * 4]);
                }
            }
        }

        // Texels the picture does not show (under its opaque parts, past the screen) take the nearest shown one
        // along the row, rows without any the nearest row that has some; each side on its own.
        for (int part = 0; part < 2; ++part) {
            const int64_t first = part ? w / 2 + w : 0, last = part ? wide : w / 2;
            std::vector<int64_t> rows_with;
            for (uint32_t row = 0; row < h; ++row) {
                float* line = &side[size_t(row) * wide * 4];
                const uint8_t* shown = &known[size_t(row) * wide];
                int64_t previous = -1;
                for (int64_t column = first; column < last; ++column) {
                    if (!shown[column]) {
                        continue;
                    }

                    for (int64_t c = previous < 0 ? first : previous + 1; c < column; ++c) {
                        const int64_t near = previous < 0 || column - c < c - previous ? column : previous;
                        std::copy(line + near * 4, line + near * 4 + 4, line + c * 4);
                    }

                    previous = column;
                }

                if (previous < 0) {
                    continue;
                }

                for (int64_t c = previous + 1; c < last; ++c) {
                    std::copy(line + previous * 4, line + previous * 4 + 4, line + c * 4);
                }

                rows_with.push_back(row);
            }

            if (rows_with.empty()) {
                continue;
            }

            size_t next = 0;
            for (uint32_t row = 0; row < h; ++row) {
                while (next + 1 < rows_with.size() && rows_with[next + 1] <= int64_t(row)) {
                    ++next;
                }

                int64_t from = rows_with[next];
                if (next + 1 < rows_with.size() && rows_with[next + 1] - int64_t(row) < int64_t(row) - from) {
                    from = rows_with[next + 1];
                }

                if (from == int64_t(row)) {
                    continue;
                }

                std::copy(&side[(size_t(from) * wide + first) * 4], &side[(size_t(from) * wide + last) * 4],
                          &side[(size_t(row) * wide + first) * 4]);
            }
        }

        if (!floor) { // the painting shows the reflection: it fades out
            std::fill(side.begin(), side.end(), 0.0f);
        }
    } else if (floor) {
        // Patches of the floor's part without objects, 96 texels square every 64 (32 overlapping, feathered), placed
        // by a fixed pseudo-random sequence, then matched row by row to the brightness of the original's outer 96
        // columns on that side.
        side.assign(size_t(wide) * h * 4, 0.0f);
        std::vector<float> weight(size_t(wide) * h, 0.0f);
        const uint32_t block = std::min<uint32_t>(96, std::min(w, h) / 2), step = block * 2 / 3;
        const uint32_t source_rows = character ? h : std::max(block + 1, uint32_t(h * 0.37));
        uint32_t seed = 0x9e3779b9u;
        auto next = [&] {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            return seed;
        };
        for (int part = 0; part < 2; ++part) {
            const int64_t first = part ? w / 2 + w : 0, last = part ? wide : w / 2;
            for (int64_t top = -int64_t(block) / 2; top < int64_t(h); top += step) {
                for (int64_t left = first - int64_t(block) / 2; left < last; left += step) {
                    const uint32_t sx = next() % (w - block), sy = next() % (source_rows - block);
                    for (uint32_t j = 0; j < block; ++j) {
                        for (uint32_t i = 0; i < block; ++i) {
                            const int64_t row = top + j, column = left + i;
                            if (row < 0 || row >= int64_t(h) || column < first || column >= last) {
                                continue;
                            }

                            const float edge = float(std::min(std::min(i, block - 1 - i), std::min(j, block - 1 - j)));
                            const float k = std::min(1.0f, (edge + 1) / float(block - step));
                            const uint8_t* p = &pixels[(size_t(sy + j) * w + sx + i) * 4];
                            float* out = &side[(size_t(row) * wide + column) * 4];
                            for (unsigned c = 0; c < 3; ++c) {
                                out[c] += p[c] * k;
                            }

                            out[3] += p[3] * k;
                            weight[size_t(row) * wide + column] += k;
                        }
                    }
                }
            }

            // Row brightness: the synthesis against the original's outer columns, both averaged over 33 rows.
            const uint32_t band = std::min<uint32_t>(96, w / 2);
            std::vector<double> want(h * 3), have(h * 3);
            for (uint32_t row = 0; row < h; ++row) {
                for (unsigned c = 0; c < 3; ++c) {
                    double a = 0, b = 0;
                    for (uint32_t i = 0; i < band; ++i) {
                        a += pixels[(size_t(row) * w + (part ? w - 1 - i : i)) * 4 + c];
                    }

                    for (int64_t column = first; column < last; ++column) {
                        const float* out = &side[(size_t(row) * wide + column) * 4];
                        b += out[c] / std::max(1e-6f, weight[size_t(row) * wide + column]);
                    }

                    want[row * 3 + c] = a / band;
                    have[row * 3 + c] = b / double(last - first);
                }
            }

            for (uint32_t row = 0; row < h; ++row) {
                for (unsigned c = 0; c < 3; ++c) {
                    double a = 0, b = 0, n = 0;
                    for (int64_t r = int64_t(row) - 16; r <= int64_t(row) + 16; ++r) {
                        if (r < 0 || r >= int64_t(h)) {
                            continue;
                        }

                        a += want[r * 3 + c];
                        b += have[r * 3 + c];
                        n += 1;
                    }

                    const double shift = (a - b) / n;
                    for (int64_t column = first; column < last; ++column) {
                        float* out = &side[(size_t(row) * wide + column) * 4];
                        const float k = std::max(1e-6f, weight[size_t(row) * wide + column]);
                        out[c] = float(std::clamp(out[c] / k + shift, 0.0, 255.0));
                        if (c == 2) {
                            out[3] /= k;
                        }
                    }
                }
            }
        }
    }

    if (!side.empty()) {
        const double band = 24.0;
        for (uint32_t row = 0; row < h; ++row) {
            for (uint32_t column = 0; column < wide; ++column) {
                const int64_t distance = outside(column);
                if (!distance) {
                    continue;
                }

                const double t = std::clamp((distance - 0.5) / band, 0.0, 1.0);
                uint8_t* out = &rgba[(size_t(row) * wide + column) * 4];
                const float* own = &side[(size_t(row) * wide + column) * 4];
                for (unsigned c = 0; c < 4; ++c) {
                    out[c] = uint8_t(std::lround(out[c] * (1 - t) + own[c] * t));
                }
            }
        }

        return rgba;
    }

    // Away from the old edges the mirror image fades into a blur of itself (box blur, three passes per axis), so it
    // continues the texture's colours without repeating its details (a second sword, a mirrored logo).
    std::vector<uint8_t> blurred = rgba;
    const int radius = 24;
    for (int pass = 0; pass < 6; ++pass) {
        const bool horizontal = pass % 2 == 0;
        const uint32_t lines = horizontal ? h : wide, length = horizontal ? wide : h;
        std::vector<uint8_t> next(blurred.size());
        for (uint32_t line = 0; line < lines; ++line) {
            auto at = [&](int64_t i) -> uint8_t* {
                const uint32_t k = uint32_t(std::clamp<int64_t>(i, 0, length - 1));
                return &blurred[(horizontal ? size_t(line) * wide + k : size_t(k) * wide + line) * 4];
            };
            uint32_t sum[4] = {0, 0, 0, 0};
            for (int64_t i = -radius; i <= radius; ++i) {
                for (unsigned c = 0; c < 4; ++c) {
                    sum[c] += at(i)[c];
                }
            }

            for (uint32_t i = 0; i < length; ++i) {
                uint8_t* out = &next[(horizontal ? size_t(line) * wide + i : size_t(i) * wide + line) * 4];
                for (unsigned c = 0; c < 4; ++c) {
                    out[c] = uint8_t(sum[c] / (2 * radius + 1));
                    sum[c] += at(int64_t(i) + radius + 1)[c];
                    sum[c] -= at(int64_t(i) - radius)[c];
                }
            }
        }

        blurred.swap(next);
    }

    for (uint32_t row = 0; row < h; ++row) {
        for (uint32_t column = 0; column < wide; ++column) {
            const int64_t distance = outside(column);
            if (distance <= 0) {
                continue;
            }

            const double k = std::min(1.0, double(distance) / 96.0);
            uint8_t* out = &rgba[(size_t(row) * wide + column) * 4];
            const uint8_t* soft = &blurred[(size_t(row) * wide + column) * 4];
            for (unsigned c = 0; c < 4; ++c) {
                out[c] = uint8_t(std::lround(out[c] * (1 - k) + soft[c] * k));
            }
        }
    }

    return rgba;
}

Backdrop network_picture(const std::vector<uint8_t>& screen) {
    Backdrop picture;
    picture.key = "network";
    const uint32_t side = menu_extension_max;
    picture.width = 1024 + 2 * side;
    picture.height = 768;
    picture.rgba.resize(size_t(picture.width) * picture.height * 4);
    for (uint32_t y = 0; y < 768; ++y) {
        for (uint32_t x = 0; x < picture.width; ++x) {
            // Picture column x is screen column x - side, reflected at the screen's edges.
            const int32_t at = int32_t(x) - int32_t(side);
            const uint32_t from = at < 0 ? uint32_t(-1 - at) : at >= 1024 ? uint32_t(2047 - at) : uint32_t(at);
            std::copy_n(&screen[(size_t(y) * 1024 + from) * 4], 4, &picture.rgba[(size_t(y) * picture.width + x) * 4]);
        }
    }

    for (size_t at = 3; at < picture.rgba.size(); at += 4) {
        picture.rgba[at] = 255;
    }

    return picture;
}

const std::vector<BackdropScreen>& backdrop_screens() {
    static const std::vector<BackdropScreen> screens = [] {
        std::vector<BackdropScreen> list;
        list.push_back({"main-sacred", numbered("GUI_OPTION%d.TGA", 1), "UI_MAIN_CENTER.TGA", "UI_MAIN_REF.TGA",
                        "UI_MAIN_SKY.TGA"});
        list.push_back(
            {"main-underworld", numbered("GUI_UW_OPTION%d.TGA", 1), "UI_UW_MAIN_CENTER.TGA", "UI_UW_MAIN_REF.TGA"});
        list.push_back(
            {"charselect-sacred", numbered("GUI_CHAR_%02d.TGA", 1), "UI_CHAR_CENTER.TGA", "UI_CHAR_REF.TGA"});
        list.push_back({"charselect-underworld", numbered("GUI_UW_CHAR_%02d.TGA", 1), "UI_UW_CHAR_CENTER.TGA",
                        "UI_UW_CHAR_REF.TGA"});
        // The classic options screen (LEGACY_UI) names its tiles by row and column.
        std::vector<std::string> classic;
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 4; ++column) {
                classic.push_back("GUI_OPTION_2_" + std::to_string(row) + std::to_string(column) + ".TGA");
            }
        }

        list.push_back({"options-sacred", classic});
        list.push_back({"options-underworld", numbered("UW_OPTION_1_%02d.TGA", 1)});
        // The save game window (load from the main menu, save and load from the game menu) has one background in
        // both looks: the table at 0x9e4754 (FUN_0071efe0) that LEGACY_UI leaves alone.
        list.push_back({"saveload", numbered("UW_OPTION_%02d.TGA", 1)});
        // The stone behind the network windows (local network and internet lobby, account, password): the menu
        // manager's own tiles (FUN_0070be20), row and column in the name like the classic options screen. They are
        // 16-bit in texture.pak, so not recognised by content: the window draw raises a screen flag instead
        // (resolution_sites.json, 0x007277f8), and the picture is made from them at start (network_picture).
        std::vector<std::string> network;
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 4; ++column) {
                network.push_back("GUI_OPTION_BACKGROUND" + std::to_string(row) + std::to_string(column) + ".TGA");
            }
        }

        list.push_back({"network", network});
        return list;
    }();
    return screens;
}

uint64_t texture_prefix_hash(const uint8_t* bytes, size_t size) {
    uint64_t hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < std::min<size_t>(size, 1024); ++i) {
        hash ^= bytes[i];
        hash *= 0x100000001b3ull;
    }

    return hash;
}

std::map<std::string, uint64_t> texture_pak_hashes(const std::vector<std::filesystem::path>& paks) {
    return texture_pak_hashes(paks, nullptr);
}

std::map<std::string, uint64_t> texture_pak_hashes(const std::vector<std::filesystem::path>& paks,
                                                   std::map<std::string, uint64_t>* prefixes) {
    std::map<std::string, uint64_t> hashes;
    for (const auto& path : paks) {
        const PakBytes data(path.string());
        if (!data) {
            continue;
        }

        auto u32 = [&](size_t at) {
            return uint32_t(data[at]) | uint32_t(data[at + 1]) << 8 | uint32_t(data[at + 2]) << 16 |
                   uint32_t(data[at + 3]) << 24;
        };
        if (data.size() < 0x100 || data[0] != 'T' || data[1] != 'E' || data[2] != 'X' || data[3] != 3) {
            continue;
        }

        const uint32_t count = u32(4);
        for (uint32_t i = 0; i < count && 0x100 + size_t(i) * 12 + 12 <= data.size(); ++i) {
            const uint32_t offset = u32(0x100 + size_t(i) * 12 + 4), size = u32(0x100 + size_t(i) * 12 + 8);
            if (!size || size_t(offset) + 0x50 + size > data.size()) {
                continue;
            }

            const uint32_t width = uint32_t(data[offset + 32]) | uint32_t(data[offset + 33]) << 8;
            const uint32_t height = uint32_t(data[offset + 34]) | uint32_t(data[offset + 35]) << 8;
            if ((u32(offset + 36) & 0xff) != 6 || size != width * height * 4) {
                continue;
            }

            std::string name(reinterpret_cast<const char*>(data.data() + offset), 32);
            name = upper(name.substr(0, name.find('\0')));
            uint64_t hash = 0xcbf29ce484222325ull;
            for (size_t at = offset + 0x50, end = at + size; at < end; ++at) {
                hash ^= data[at];
                hash *= 0x100000001b3ull;
            }

            hashes[name] = hash;
            if (prefixes) {
                (*prefixes)[name] = texture_prefix_hash(data.data() + offset + 0x50, size);
            }
        }
    }

    return hashes;
}

MenuBackdrops::MenuBackdrops(const std::string& directory, const std::vector<std::filesystem::path>& paks,
                             const std::vector<std::filesystem::path>& originals) {
    namespace fs = std::filesystem;
    std::error_code error;
    std::vector<fs::path> files;
    // The directory as resolved, for the notes: a relative one is read from the working directory.
    std::string shown = directory;
    if (!directory.empty() && fs::path(directory).is_relative()) {
        std::error_code ignored;
        shown = fs::absolute(directory, ignored).string() + " (relative to the working directory " +
                fs::current_path(ignored).string() + ")";
    }

    if (!directory.empty()) {
        for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
            const fs::directory_entry& entry = *it;
            if (lower(entry.path().extension().string()) != ".png") {
                continue;
            }

            std::error_code status;
            if (entry.is_regular_file(status)) {
                files.push_back(entry.path());
                continue;
            }

            // A link to a picture that is not there (made with a relative target, then moved or copied elsewhere).
            if (entry.is_symlink(status) && !fs::exists(entry.path(), status)) {
                std::error_code target_error;
                const fs::path target = fs::read_symlink(entry.path(), target_error);
                notes.push_back(entry.path().filename().string() + ": a link to " + target.string() +
                                ", which does not exist; ignored");
            }
        }

        if (error) {
            notes.push_back("cannot open the directory " + shown + ": " + error.message());
        }
    }

    std::sort(files.begin(), files.end());
    // Split pictures (tools/menu/split_backgrounds.py): <key>.sides.png holds our pixels, <key>.mask.png says which;
    // the rest is the game's own screen, taken from the player's texture.pak tiles or loading bitmap. A full
    // <key>.png wins over a pair.
    std::set<std::string> full;
    for (const auto& path : files) {
        const std::string name = lower(path.filename().string());
        if (name.size() > 4 && name.find('.') == name.size() - 4) {
            full.insert(name.substr(0, name.size() - 4));
        }
    }

    // The game's own screens under the split pictures: the installation's paks, never the profile's replacements.
    const std::vector<fs::path>& base = originals.empty() ? paks : originals;
    std::map<std::string, std::string> composed; // key -> SHA-256 (hex) the split picture must compose to
    {
        std::ifstream sums(fs::path(directory) / "composed.sha256");
        for (std::string digest, name; sums >> digest >> name;) {
            composed[name] = digest;
        }
    }

    std::map<std::string, std::vector<uint8_t>> tile_pixels; // name -> 256x256 RGBA, read once when needed
    auto screen_of = [&](const std::string& key, std::vector<uint8_t>& rgba, std::string& why) {
        if (key.rfind("loading-", 0) == 0) {
            // PAK/<bitmap>.bmp next to texture.pak, any case.
            const fs::path pak_dir = base.empty() ? fs::path() : base.front().parent_path();
            std::error_code status;
            for (fs::directory_iterator it(pak_dir, status), end; !status && it != end; it.increment(status)) {
                if (lower(it->path().filename().string()) == key.substr(8) + ".bmp") {
                    TextureReplacement bitmap;
                    if (!texture_png_read(it->path().string(), bitmap) || bitmap.width != 1024 ||
                        bitmap.height != 768) {
                        why = "cannot read " + it->path().filename().string() + " as a 1024x768 bitmap";
                        return false;
                    }

                    rgba = std::move(bitmap.rgba);
                    return true;
                }
            }

            why = "no " + key.substr(8) + ".bmp in " + pak_dir.string();
            return false;
        }

        const auto screen = std::find_if(backdrop_screens().begin(), backdrop_screens().end(),
                                         [&](const BackdropScreen& s) { return key == s.key; });
        if (screen == backdrop_screens().end()) {
            why = "no original screen known";
            return false;
        }

        if (tile_pixels.empty()) {
            for (const auto& pak : base) {
                TexturePakStats stats;
                texture_pak_read(
                    pak.string(),
                    [&](unsigned, const char* name, const PixelImage& image, unsigned) {
                        std::string upper = name;
                        for (char& c : upper) {
                            c = char(std::toupper(static_cast<unsigned char>(c)));
                        }

                        if (image.width == 256 && image.height == 256 && upper.find(".TGA") != std::string::npos) {
                            tile_pixels[upper] = unpack(image);
                        }
                    },
                    stats, nullptr);
            }
        }

        rgba.assign(size_t(1024) * 768 * 4, 0);
        for (size_t i = 0; i < screen->tiles.size(); ++i) {
            auto tile = tile_pixels.find(screen->tiles[i]);
            if (tile == tile_pixels.end()) {
                why = screen->tiles[i] + " missing from texture.pak";
                return false;
            }

            const size_t left = (i % 4) * 256, top = (i / 4) * 256;
            for (size_t y = 0; y < 256; ++y) {
                std::copy_n(&tile->second[y * 256 * 4], 256 * 4, &rgba[((top + y) * 1024 + left) * 4]);
            }
        }

        return true;
    };
    for (const auto& path : files) {
        std::string key = lower(path.stem().string());
        const bool mask_file = key.size() > 5 && key.compare(key.size() - 5, 5, ".mask") == 0;
        const bool sides_file = key.size() > 6 && key.compare(key.size() - 6, 6, ".sides") == 0;
        if (mask_file) {
            continue;
        }

        if (sides_file) {
            key.resize(key.size() - 6);
            if (full.count(key)) {
                continue;
            }
        }

        const bool tiled = std::any_of(backdrop_screens().begin(), backdrop_screens().end(),
                                       [&](const BackdropScreen& s) { return key == s.key; });
        if (!tiled && key != "worldmap" && key.rfind("loading-", 0) != 0) {
            notes.push_back(path.filename().string() + ": not a known screen name, ignored");
            continue;
        }

        TextureReplacement image;
        if (!texture_png_read(path.string(), image)) {
            notes.push_back(path.filename().string() + ": cannot read the PNG, ignored");
            continue;
        }

        if (sides_file) {
            TextureReplacement mask;
            const fs::path mask_path = path.parent_path() / (path.stem().stem().string() + ".mask.png");
            std::vector<uint8_t> screen;
            std::string why;
            if (!texture_png_read(mask_path.string(), mask) || mask.width != image.width ||
                mask.height != image.height) {
                notes.push_back(path.filename().string() + ": no matching " + mask_path.filename().string() +
                                ", ignored");
                continue;
            }

            if (image.height % 768 || uint64_t(image.width) * 768 < uint64_t(image.height) * 1024) {
                notes.push_back(path.filename().string() + ": not 768 (or k x 768) high and at least 4:3, ignored");
                continue;
            }

            if (!screen_of(key, screen, why)) {
                notes.push_back(path.filename().string() + ": " + why + ", ignored");
                continue;
            }

            // The game's screen at the picture's scale (nearest), in the middle: taken where the mask is 0, our
            // correction added to it (per channel, modulo 256) where the mask is 64, ours where it is 255.
            const uint32_t k = image.height / 768, left = (image.width - 1024 * k) / 2;
            for (uint32_t y = 0; y < image.height; ++y) {
                for (uint32_t x = left; x < left + 1024 * k; ++x) {
                    const size_t at = size_t(y) * image.width + x;
                    const uint8_t kind = mask.rgba[at * 4];
                    if (kind > 127) {
                        continue;
                    }

                    const size_t from = (size_t(y / k) * 1024 + (x - left) / k) * 4;
                    for (int c = 0; c < 4; ++c) {
                        image.rgba[at * 4 + c] = uint8_t(screen[from + c] + (kind >= 32 ? image.rgba[at * 4 + c] : 0));
                    }
                }
            }

            // Another base than the game's own screen (a different release's tiles) composes to noise.
            auto want = composed.find(key);
            if (want != composed.end()) {
                const auto digest = host_sha256(image.rgba.data(), image.rgba.size());
                char hex[65];
                for (int b = 0; b < 32; ++b) {
                    std::snprintf(hex + 2 * b, 3, "%02x", digest[b]);
                }

                if (want->second != hex) {
                    notes.push_back(path.filename().string() + ": the game's screen under it differs from the one it "
                                                               "was made on (composed.sha256), ignored");
                    continue;
                }
            }
        }

        Backdrop backdrop;
        backdrop.key = key;
        backdrop.width = image.width;
        backdrop.height = image.height;
        backdrop.rgba = std::move(image.rgba);
        // At least the 1024x768 screen at the image's height.
        if (uint64_t(backdrop.width) * 768 < uint64_t(backdrop.height) * 1024 || backdrop.height < 768) {
            notes.push_back(path.filename().string() + ": narrower than 4:3 or lower than 768 pixels, ignored");
            continue;
        }

        for (size_t at = 3; at < backdrop.rgba.size() && !backdrop.alpha; at += 4) {
            backdrop.alpha = backdrop.rgba[at] < 255;
        }

        notes.push_back(path.filename().string() + ": " + std::to_string(backdrop.width) + "x" +
                        std::to_string(backdrop.height) + (backdrop.alpha ? " with alpha" : " opaque"));
        backdrops_.push_back(std::move(backdrop));
    }

    if (!directory.empty() && !error && backdrops_.empty()) {
        notes.push_back("no usable picture in " + shown + " (one PNG per screen, e.g. main-sacred.png)");
    }

    // The network windows' picture, unless the directory has one: their stone screen in the middle of the widest
    // canvas, mirrored out to the sides (the stone has no direction, and a mirror has no seam). Made when the split
    // pictures have read the paks' tiles already (decoding texture.pak for it alone would slow every start down).
    if (find("network") < 0 && !tile_pixels.empty()) {
        std::vector<uint8_t> screen;
        std::string why;
        if (screen_of("network", screen, why)) {
            backdrops_.push_back(network_picture(screen));
        } else {
            notes.push_back("network: " + why);
        }
    }

    std::map<std::string, uint64_t> prefix_of;
    const auto hashes = texture_pak_hashes(paks, &prefix_of);
    for (const BackdropScreen& screen : backdrop_screens()) {
        const int index = find(screen.key);
        if (screen.key == std::string("network")) {
            continue; // shown by its screen flag, see backdrop_screens
        }

        // The water and sky of the menu canvas screens are needed with or without a picture.
        auto role = [&](const char* name, BackdropTile::Role kind) {
            if (!name) {
                return;
            }

            auto found = hashes.find(name);
            if (found == hashes.end()) {
                return;
            }

            tiles_[found->second] = BackdropTile{kind, index, 0, 0, screen.key};
            prefixes_.insert(prefix_of[name]);
        };
        role(screen.floor, BackdropTile::Floor);
        role(screen.reflection, BackdropTile::Reflection);
        role(screen.sky, BackdropTile::Sky);
        if (index < 0) {
            continue;
        }

        unsigned missing = 0;
        for (size_t i = 0; i < screen.tiles.size(); ++i) {
            auto found = hashes.find(screen.tiles[i]);
            if (found == hashes.end()) {
                ++missing;
                continue;
            }

            tiles_[found->second] =
                BackdropTile{BackdropTile::Tile, index, unsigned(i % 4), unsigned(i / 4), screen.key};
            prefixes_.insert(prefix_of[screen.tiles[i]]);
        }

        if (missing) {
            notes.push_back(std::string(screen.key) + ": " + std::to_string(missing) +
                            " tiles missing from texture.pak");
        }
    }
}

int MenuBackdrops::find(const std::string& key) const {
    for (size_t i = 0; i < backdrops_.size(); ++i) {
        if (backdrops_[i].key == key) {
            return int(i);
        }
    }

    return -1;
}

const BackdropTile* MenuBackdrops::tile(uint64_t hash) const {
    auto found = tiles_.find(hash);
    return found == tiles_.end() ? nullptr : &found->second;
}

int MenuBackdrops::loading(const std::string& bitmap_path) const {
    std::string name = bitmap_path;
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) {
        name = name.substr(slash + 1);
    }

    const size_t dot = name.rfind('.');
    if (dot != std::string::npos) {
        name = name.substr(0, dot);
    }

    return find("loading-" + lower(name));
}

bool MenuBackdrops::centre_bitmap(size_t index, uint32_t width, uint32_t height, uint32_t pitch,
                                  std::vector<uint8_t>& bgr) const {
    const Backdrop& image = backdrops_[index];
    if (!width || !height || bgr.size() < size_t(pitch) * height) {
        return false;
    }

    // The screen in image pixels: 768 rows map to the image height, its 1024 columns are centred.
    const double scale = double(image.height) / 768.0;
    const double left = (double(image.width) - 1024.0 * scale) / 2.0;
    const double step_x = 1024.0 * scale / width, step_y = 768.0 * scale / height;
    for (uint32_t y = 0; y < height; ++y) {
        const uint32_t y0 = uint32_t(y * step_y),
                       y1 = std::max(y0 + 1, std::min(image.height, uint32_t((y + 1) * step_y)));
        uint8_t* row = &bgr[size_t(height - 1 - y) * pitch];
        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t x0 = uint32_t(left + x * step_x);
            const uint32_t x1 = std::max(x0 + 1, std::min(image.width, uint32_t(left + (x + 1) * step_x)));
            uint32_t sum[3] = {0, 0, 0}, count = 0;
            for (uint32_t sy = y0; sy < y1; ++sy) {
                for (uint32_t sx = x0; sx < x1; ++sx) {
                    const uint8_t* p = &image.rgba[(size_t(sy) * image.width + sx) * 4];
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                    ++count;
                }
            }

            row[x * 3 + 0] = uint8_t(sum[2] / count);
            row[x * 3 + 1] = uint8_t(sum[1] / count);
            row[x * 3 + 2] = uint8_t(sum[0] / count);
        }
    }

    return true;
}
}
