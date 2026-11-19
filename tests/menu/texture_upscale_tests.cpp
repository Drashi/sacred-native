// The engine's texture upscaler (runtime/ddraw/texture_upscale.cpp) against tools/textures/texture_pack_build.py.
//   texture_upscale_tests                        built-in cases: the script's self-test rules
//   texture_upscale_tests SOURCE EXPECTED [N]    every PNG of SOURCE (or the first N) upscaled and compared byte for
//                                                byte with the file of the same name in EXPECTED (the script's pack)
#include "runtime/ddraw/texture_pack.h"
#include "runtime/ddraw/texture_upscale.h"
#include "runtime/ddraw/texture_pak.h"
#include <cstring>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>

using namespace sacred;
namespace {
unsigned checks = 0;
void require(bool condition, const std::string& what) {
    ++checks;
    if (!condition) {
        throw std::runtime_error("FAIL: " + what);
    }
}

std::vector<uint8_t> image(uint32_t w, uint32_t h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    std::vector<uint8_t> out(size_t(w) * h * 4);
    for (size_t i = 0; i < out.size(); i += 4) {
        out[i] = r;
        out[i + 1] = g;
        out[i + 2] = b;
        out[i + 3] = a;
    }

    return out;
}

void set(std::vector<uint8_t>& im, uint32_t w, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    uint8_t* p = &im[(size_t(y) * w + x) * 4];
    p[0] = r;
    p[1] = g;
    p[2] = b;
    p[3] = a;
}

void built_in() {
    // Glowing disc on transparent black: far from it the output stays transparent black.
    auto icon = image(32, 32, 0, 0, 0, 0);
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            if ((y - 15.5) * (y - 15.5) + (x - 15.5) * (x - 15.5) < 100) {
                set(icon, 32, x, y, 240, 200, 60, 255);
            }
        }
    }

    auto out = texture_upscale(icon, 32, 32, 2);
    require(out.size() == 64 * 64 * 4, "size");
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const int sy = y / 2, sx = x / 2;
            if ((sy - 15.5) * (sy - 15.5) + (sx - 15.5) * (sx - 15.5) <= 150) {
                continue;
            }

            const uint8_t* p = &out[(size_t(y) * 64 + x) * 4];
            require(!p[0] && !p[1] && !p[2] && !p[3], "transparent black stays black");
        }
    }

    // Atlas piece: no dark fringe under visible pixels, gutters stay empty.
    auto piece = image(16, 16, 0, 0, 0, 0);
    for (int y = 4; y < 12; ++y) {
        for (int x = 4; x < 12; ++x) {
            set(piece, 16, x, y, 200, 100, 50, 255);
        }
    }

    out = texture_upscale(piece, 16, 16, 2);
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            const uint8_t* p = &out[(size_t(y) * 32 + x) * 4];
            if (p[3]) {
                require(std::abs(p[0] - 200) <= 2 && std::abs(p[1] - 100) <= 2 && std::abs(p[2] - 50) <= 2, "fringe");
            }

            if (y < 6 || y >= 26 || x < 6 || x >= 26) {
                require(!p[0] && !p[1] && !p[2] && !p[3], "gutter");
            }
        }
    }

    // Opaque: alpha stays 255.
    auto flare = image(16, 16, 0, 0, 0, 255);
    for (int y = 6; y < 10; ++y) {
        for (int x = 6; x < 10; ++x) {
            set(flare, 16, x, y, 255, 255, 255, 255);
        }
    }

    out = texture_upscale(flare, 16, 16, 3);
    for (size_t i = 3; i < out.size(); i += 4) {
        require(out[i] == 255, "opaque alpha");
    }

    std::printf("TEXTURE_UPSCALE_PASS checks=%u\n", checks);
}

int compare(const std::filesystem::path& source, const std::filesystem::path& expected, size_t limit) {
    size_t compared = 0, same = 0, missing = 0, worst = 0;
    double total_ms = 0, max_ms = 0;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(source)) {
        if (entry.path().extension() == ".png") {
            files.push_back(entry.path());
        }
    }

    std::sort(files.begin(), files.end());
    for (const auto& path : files) {
        if (limit && compared >= limit) {
            break;
        }

        const auto target = expected / path.filename();
        TextureReplacement want, have;
        if (!std::filesystem::exists(target) || !texture_png_read(target.string(), want)) {
            ++missing;
            continue;
        }

        if (!texture_png_read(path.string(), have)) {
            throw std::runtime_error("cannot read " + path.string());
        }

        const unsigned scale = texture_scale(have.width, have.height, want.width, want.height);
        const auto start = std::chrono::steady_clock::now();
        const auto got = texture_upscale(have.rgba, have.width, have.height, scale);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        total_ms += ms;
        max_ms = std::max(max_ms, ms);
        ++compared;
        size_t differ = 0, largest = 0;
        for (size_t i = 0; i < got.size(); ++i) {
            if (got[i] != want.rgba[i]) {
                ++differ;
                largest = std::max<size_t>(largest, size_t(std::abs(got[i] - want.rgba[i])));
            }
        }

        if (!differ) {
            ++same;
        } else {
            worst = std::max(worst, largest);
            if (compared - same <= 10) {
                std::printf("DIFF %s: %zu of %zu bytes, largest %zu\n", path.filename().c_str(), differ, got.size(),
                            largest);
            }
        }
    }

    std::printf(
        "TEXTURE_UPSCALE_COMPARE compared=%zu identical=%zu missing=%zu worst_byte_diff=%zu avg_ms=%.2f max_ms=%.2f\n",
        compared, same, missing, worst, compared ? total_ms / compared : 0.0, max_ms);
    return same == compared ? 0 : 1;
}
}

// TexturePack in upscale mode on the installation's paks: the content list, its cache file, and a synchronous upscale
// of a pak texture equal to texture_upscale of its pixels.
int paks(const std::filesystem::path& game) {
    const std::vector<std::string> files = {(game / "PAK/texture.pak").string(), (game / "PAK/texture03.pak").string()};
    const auto cache = std::filesystem::temp_directory_path() / ("texture-upscale-test-" + std::to_string(::getpid()));
    PixelImage sample{};
    uint64_t sample_hash = 0;
    TexturePakStats stats;
    texture_pak_read(
        files[0],
        [&](unsigned, const char* name, const PixelImage& image, unsigned) {
            if (!sample_hash && !std::strncmp(name, "GUI_", 4)) {
                sample = image;
                sample_hash = texture_hash(image);
            }
        },
        stats, nullptr);
    require(sample_hash != 0, "a GUI texture in texture.pak");
    for (int run = 0; run < 2; ++run) {
        TexturePack pack("", false, 64u << 20, 2, files, cache.string());
        require(pack.upscale_ready() && pack.contains(sample_hash), "pak texture listed");
        require(!pack.contains(0x0123456789abcdefull), "other content not listed");
        auto image = pack.find(sample_hash, &sample);
        require(image && image->width == sample.width * 2, "upscaled replacement");
        require(image->rgba == texture_upscale(unpack(sample), sample.width, sample.height, 2),
                "same as texture_upscale");
    }

    require(std::filesystem::file_size(cache) > 23000 * 8, "index cache written");
    std::filesystem::remove(cache);
    std::printf("TEXTURE_UPSCALE_PAKS_PASS checks=%u\n", checks);
    return 0;
}

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "--game") {
            return paks(argv[2]);
        }

        if (argc >= 3) {
            return compare(argv[1], argv[2], argc > 3 ? std::strtoul(argv[3], nullptr, 10) : 0);
        }

        built_in();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
