#include "runtime/ddraw/surface_pixels.h"
#include "runtime/ddraw/texture_pack.h"
#include "runtime/ddraw/menu_backdrops.h"
#include "runtime/ddraw/inset_schedule.h"
#include <cmath>
#include <fstream>
#include "runtime/process/memory.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

using namespace sacred;
static unsigned checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x))                                                                                                      \
            throw GuestFault(#x);                                                                                      \
    } while (0)
static PixelImage image(uint32_t width, uint32_t height, PixelFormat format) {
    uint32_t pitch = width * describe(format).bits / 8;
    return {width, height, pitch, format, std::vector<uint8_t>(pitch * height)};
}

static uint32_t word(const PixelImage& value, uint32_t x, uint32_t y) {
    unsigned bytes = describe(value.format).bits / 8;
    uint32_t result = 0;
    for (unsigned i = 0; i < bytes; ++i) {
        result |= uint32_t(value.pixels[y * value.pitch + x * bytes + i]) << (i * 8);
    }

    return result;
}

int main() {
    try {
        auto argb = image(1, 1, PixelFormat::Argb8888);
        argb.pixels = {0x33, 0x22, 0x11, 0x44};
        const std::vector<uint8_t> rgba = {0x11, 0x22, 0x33, 0x44};
        CHECK(unpack(argb) == rgba);
        pack(argb, rgba);
        CHECK(word(argb, 0, 0) == 0x44112233);
        auto xrgb = image(1, 1, PixelFormat::Xrgb8888);
        xrgb.pixels = argb.pixels;
        auto opaque = rgba;
        opaque[3] = 255;
        CHECK(unpack(xrgb) == opaque);
        auto nibble = image(1, 1, PixelFormat::Argb4444);
        nibble.pixels = {0x23, 0x81};
        const std::vector<uint8_t> expanded = {17, 34, 51, 136};
        CHECK(unpack(nibble) == expanded);
        auto green = image(1, 1, PixelFormat::Rgb565);
        green.pixels = {0xe0, 7};
        const std::vector<uint8_t> green_rgba = {0, 255, 0, 255};
        CHECK(unpack(green) == green_rgba);
        auto red = image(1, 1, PixelFormat::Argb1555);
        red.pixels = {0, 0xfc};
        const std::vector<uint8_t> red_rgba = {255, 0, 0, 255};
        CHECK(unpack(red) == red_rgba);
        for (auto format : {PixelFormat::Rgb565, PixelFormat::Argb1555, PixelFormat::Argb4444}) {
            auto packed = image(256, 256, format);
            for (uint32_t i = 0; i < 65536; ++i) {
                packed.pixels[i * 2] = uint8_t(i);
                packed.pixels[i * 2 + 1] = uint8_t(i >> 8);
            }

            auto original = packed.pixels;
            pack(packed, unpack(packed));
            CHECK(packed.pixels == original);
        }

        PixelImage padded{3, 2, 8, PixelFormat::Argb4444, std::vector<uint8_t>(16, 0xa5)};
        fill(padded, {-1, -1, 2, 1}, 0x8123);
        CHECK(word(padded, 0, 0) == 0x8123 && word(padded, 1, 0) == 0x8123);
        CHECK(word(padded, 2, 0) == 0xa5a5 && padded.pixels[6] == 0xa5 && padded.pixels[7] == 0xa5);
        CHECK(word(padded, 0, 1) == 0xa5a5);
        CHECK(unpack(padded, {1, 0, 2, 1}) == expanded);
        auto source = image(4, 4, PixelFormat::Xrgb8888);
        for (unsigned i = 0; i < 16; ++i) {
            source.pixels[i * 4] = uint8_t(i);
        }

        auto destination = image(6, 6, PixelFormat::Xrgb8888);
        blit(destination, {-1, -1, 3, 3}, source, {0, 0, 4, 4}, {0, 0, 6, 6});
        CHECK(word(destination, 0, 0) == 5 && word(destination, 2, 2) == 15 && word(destination, 3, 3) == 0);
        destination = image(6, 6, PixelFormat::Xrgb8888);
        blit(destination, {0, 0, 4, 4}, source, {0, 0, 4, 4}, {1, 1, 5, 5});
        CHECK(word(destination, 0, 0) == 0 && word(destination, 1, 1) == 5 && word(destination, 3, 3) == 15);
        auto reduced = image(2, 2, PixelFormat::Xrgb8888);
        blit(reduced, {0, 0, 2, 2}, source, {0, 0, 4, 4}, {0, 0, 2, 2});
        CHECK(word(reduced, 0, 0) == 5 && word(reduced, 1, 0) == 7 && word(reduced, 0, 1) == 13 &&
              word(reduced, 1, 1) == 15);
        blit(source, {1, 0, 4, 4}, source, {0, 0, 3, 4}, {0, 0, 4, 4});
        CHECK(word(source, 0, 0) == 0 && word(source, 1, 0) == 0 && word(source, 2, 0) == 1 && word(source, 3, 0) == 2);
        auto unchanged = destination.pixels;
        bool rejected = false;
        try {
            blit(destination, {0, 0, 4, 4}, source, {-1, 0, 3, 4}, {0, 0, 6, 6});
        } catch (const GuestFault&) {
            rejected = true;
        }

        CHECK(rejected && destination.pixels == unchanged);
        { // Texture identity: FNV-1a 64 over the pixel rows only (pitch padding skipped), stable across runs.
            PixelImage padded{
                2,
                2,
                8,
                PixelFormat::Argb4444,
                {0x23, 0x81, 0x45, 0x67, 0xaa, 0xbb, 0xcc, 0xdd, 0x01, 0x02, 0x03, 0x04, 0xee, 0xff, 0x11, 0x22}};
            CHECK(texture_hash(padded) == 0x389184dd8b343299ull);
            auto other = padded;
            for (unsigned i : {4u, 5u, 6u, 7u, 12u, 13u, 14u, 15u}) {
                other.pixels[i] = 0;
            }

            CHECK(texture_hash(other) == texture_hash(padded));
            other.pixels[8] ^= 1;
            CHECK(texture_hash(other) != texture_hash(padded));
            CHECK(texture_file_name(padded, 0x389184dd8b343299ull) == "2x2_A4R4G4B4_389184dd8b343299.png");
            uint64_t hash = 0;
            CHECK(texture_pack_hash("256x256_A4R4G4B4_389184DD8B343299.png", hash) && hash == 0x389184dd8b343299ull);
            CHECK(texture_pack_hash("0000000000000abc.PNG", hash) && hash == 0xabc);
            CHECK(texture_pack_hash("upscaled_0000000000000abc.png", hash) && hash == 0xabc);
            for (const char* name : {"0000000000000abc.bmp", "x0000000000000abc.png", "000000000000abc.png",
                                     "000000000000xabc.png", "256x256_A4R4G4B4_389184dd8b343299.png.txt"}) {
                CHECK(!texture_pack_hash(name, hash));
            }

            CHECK(texture_scale(256, 256, 256, 256) == 1 && texture_scale(256, 256, 512, 512) == 2 &&
                  texture_scale(128, 64, 512, 256) == 4);
            CHECK(!texture_scale(256, 256, 512, 256) && !texture_scale(256, 256, 384, 384) &&
                  !texture_scale(256, 256, 128, 128) && !texture_scale(3, 3, 8, 8) && !texture_scale(0, 4, 4, 4));
        }

        { // Texture pack loading: index only, decode on request in the background, LRU-bounded cache.
            const std::string directory = "build/tests/menu/surface-pack";
            std::filesystem::remove_all(directory);
            std::filesystem::create_directories(directory);
            std::vector<uint8_t> red(8 * 8 * 4, 0), blue(8 * 8 * 4, 0);
            for (size_t i = 0; i < red.size(); i += 4) {
                red[i] = 255;
                red[i + 3] = 255;
                blue[i + 2] = 255;
                blue[i + 3] = 255;
            }

            CHECK(texture_png_write(directory + "/0000000000000001.png", 8, 8, red));
            CHECK(texture_png_write(directory + "/0000000000000002.png", 8, 8, blue));
            {
                TexturePack pack(directory, true, 300); // room for one 8x8 image (256 bytes)
                CHECK(pack.size() == 2 && pack.contains(1) && !pack.contains(3));
                CHECK(!pack.ready(1)); // nothing decoded up front
                pack.request(1);
                std::shared_ptr<const TextureReplacement> image;
                for (int tries = 0; tries < 2000 && !image; ++tries) {
                    image = pack.ready(1);
                    if (!image) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }

                CHECK(image && image->width == 8 && image->rgba == red);
                pack.request(2);
                std::shared_ptr<const TextureReplacement> second;
                for (int tries = 0; tries < 2000 && !second; ++tries) {
                    second = pack.ready(2);
                    if (!second) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }

                CHECK(second && second->rgba == blue);
                CHECK(!pack.ready(1) && pack.stats().evicted == 1); // evicted; the caller's copy stays valid
                CHECK(image->rgba == red);
            }

            TexturePack sync(directory, false);
            CHECK(sync.ready(2) && sync.ready(2)->rgba == blue && !sync.ready(7));
        }

        { // Render scale within the GPU limit: lowered until it fits, never below 1.
            CHECK(render_scale_fit(1920, 1080, 4, 16384) == 4 && render_scale_fit(1920, 1080, 4, 8192) == 4);
            CHECK(render_scale_fit(1920, 1080, 4, 7679) == 3 && render_scale_fit(1920, 1080, 4, 4096) == 2);
            CHECK(render_scale_fit(1920, 1080, 4, 2048) == 1 && render_scale_fit(1920, 1080, 4, 1024) == 1);
            CHECK(render_scale_fit(1024, 768, 0, 16384) == 1 && render_scale_fit(4096, 4096, 2, 16384) == 2);
            CHECK(render_scale_fit(0xffffffffu, 1, 4, 16384) == 1);
        }

        { // Bad packs are ignored, never fatal: a missing directory, a file instead of one, an empty one, and
            // corrupt / non-PNG / oversized files named like replacements (logged once, never matched).
            CHECK(TexturePack("build/tests/menu/no-such-pack", true).size() == 0);
            CHECK(TexturePack("", true).size() == 0);
            const std::string directory = "build/tests/menu/surface-bad-pack";
            std::filesystem::remove_all(directory);
            std::filesystem::create_directories(directory + "/empty");
            CHECK(TexturePack(directory + "/empty", true).size() == 0);
            auto put = [&](const std::string& name, const std::string& bytes) {
                std::ofstream(directory + "/" + name, std::ios::binary) << bytes;
            };
            put("plain.txt", "not a texture");
            CHECK(TexturePack(directory + "/plain.txt", true).size() == 0);
            put("0000000000000001.png", "");                                        // empty
            put("0000000000000002.png", "\x89PNG\r\n\x1a\nthis is not a png body"); // truncated after the signature
            put("0000000000000003.png", "BM this is a bitmap");                     // another format
            // A valid header claiming 17000 x 1 pixels (over texture_png_max_side) and an empty IDAT.
            std::string huge("\x89PNG\r\n\x1a\n\0\0\0\x0dIHDR\0\0\x42\x68\0\0\0\x01\x08\x06\0\0\0", 29);
            put("0000000000000004.png", huge + std::string(4, '\0') + std::string("\0\0\0\0IDAT\0\0\0\0", 12));
            std::vector<uint8_t> green(4 * 4 * 4, 255);
            CHECK(texture_png_write(directory + "/0000000000000005.png", 4, 4, green));
            for (bool background : {false, true}) {
                TexturePack pack(directory, background);
                CHECK(pack.size() == 5);
                for (uint64_t hash = 1; hash <= 4; ++hash) {
                    pack.request(hash);
                    CHECK(!pack.find(hash));
                    CHECK(!pack.ready(hash));
                }

                CHECK(pack.find(5) && pack.find(5)->rgba == green);
            }
        }

        { // Wide screen images (menu_backdrops.h): the tiles of a screen are recognised by the content of the
            // texture.pak entries they come from (later paks win), images by their key, loading bitmaps by stem.
            const std::string directory = "build/tests/menu/backdrops";
            std::filesystem::remove_all(directory);
            std::filesystem::create_directories(directory + "/images");
            auto pak = [&](const std::string& path, const std::vector<std::pair<std::string, uint8_t>>& entries,
                           bool compressed_extra) {
                std::vector<uint8_t> data(0x100, 0);
                data[0] = 'T';
                data[1] = 'E';
                data[2] = 'X';
                data[3] = 3;
                const uint32_t count = uint32_t(entries.size()) + (compressed_extra ? 1 : 0);
                data[4] = uint8_t(count);
                data.resize(0x100 + 12 * count, 0);
                auto put32 = [&](size_t at, uint32_t v) {
                    for (int i = 0; i < 4; ++i) {
                        data[at + i] = uint8_t(v >> (8 * i));
                    }
                };
                uint32_t index = 0;
                auto entry = [&](const std::string& name, uint32_t format, const std::vector<uint8_t>& pixels) {
                    const uint32_t offset = uint32_t(data.size());
                    put32(0x100 + 12 * index + 4, offset);
                    put32(0x100 + 12 * index + 8, uint32_t(pixels.size()));
                    ++index;
                    std::vector<uint8_t> header(0x50, 0);
                    std::copy(name.begin(), name.end(), header.begin());
                    header[32] = 0;
                    header[33] = 1;
                    header[34] = 0;
                    header[35] = 1; // 256 x 256
                    header[36] = uint8_t(format);
                    data.insert(data.end(), header.begin(), header.end());
                    data.insert(data.end(), pixels.begin(), pixels.end());
                };
                for (const auto& [name, fill] : entries) {
                    entry(name, 6, std::vector<uint8_t>(256 * 256 * 4, fill));
                }

                if (compressed_extra) {
                    entry("GUI_UW_OPTION2.TGA", 4, std::vector<uint8_t>(100, 1));
                }

                std::ofstream(path, std::ios::binary)
                    .write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
            };
            pak(directory + "/texture.pak",
                {{"GUI_UW_OPTION1.TGA", 7},
                 {"UW_option_1_05.tga", 9},
                 {"UI_MAIN_SKY.TGA", 13},
                 {"UI_UW_MAIN_REF.TGA", 15},
                 {"UW_option_07.tga", 17}},
                true);
            pak(directory + "/texture03.pak", {{"UW_OPTION_1_05.TGA", 11}}, false);
            auto tile_hash = [&](uint8_t fill) {
                auto texture = image(256, 256, PixelFormat::Argb8888);
                std::fill(texture.pixels.begin(), texture.pixels.end(), fill);
                return texture_hash(texture);
            };
            const auto hashes = texture_pak_hashes(
                {directory + "/texture.pak", directory + "/texture03.pak", directory + "/missing.pak"});
            CHECK(hashes.size() == 5 && hashes.at("GUI_UW_OPTION1.TGA") == tile_hash(7));
            CHECK(hashes.at("UW_OPTION_1_05.TGA") == tile_hash(11));
            // Images: an opaque one, one with alpha, a 2x loading image, and three that are ignored.
            std::vector<uint8_t> opaque(1536 * 768 * 4, 255);
            CHECK(texture_png_write(directory + "/images/options-underworld.png", 1536, 768, opaque));
            std::vector<uint8_t> holes = opaque;
            holes[3] = 0;
            CHECK(texture_png_write(directory + "/images/Main-Underworld.PNG", 1536, 768, holes));
            std::vector<uint8_t> loading(2560 * 1536 * 4);
            for (uint32_t y = 0; y < 1536; ++y) {
                for (uint32_t x = 0; x < 2560; ++x) {
                    uint8_t* p = &loading[(size_t(y) * 2560 + x) * 4];
                    p[0] = uint8_t(x & 1 ? 200 : 100);
                    p[1] = uint8_t(y < 768 ? 10 : 20);
                    p[2] = uint8_t(x / 8);
                    p[3] = 255;
                }
            }

            CHECK(texture_png_write(directory + "/images/loading-loadinguw00.png", 2560, 1536, loading));
            CHECK(texture_png_write(directory + "/images/options-sacred.png", 1000, 768,
                                    std::vector<uint8_t>(1000 * 768 * 4, 255))); // narrower than 4:3
            CHECK(texture_png_write(directory + "/images/unknown-screen.png", 1536, 768, opaque));
            std::ofstream(directory + "/images/worldmap.png") << "not a png";
            CHECK(texture_png_write(directory + "/images/saveload.png", 1836, 768,
                                    std::vector<uint8_t>(1836 * 768 * 4, 255)));
            MenuBackdrops backdrops(directory + "/images", {directory + "/texture.pak", directory + "/texture03.pak"});
            // Notes: four images read, three ignored, and 11 of the 12 tiles missing for each tiled screen.
            CHECK(backdrops.size() == 4 && backdrops.notes.size() == 10);
            const int options = backdrops.find("options-underworld"), main = backdrops.find("main-underworld");
            CHECK(options >= 0 && main >= 0 && backdrops.find("options-sacred") < 0 && backdrops.find("worldmap") < 0);
            CHECK(!backdrops.at(size_t(options)).alpha && backdrops.at(size_t(main)).alpha);
            CHECK(backdrops.at(size_t(options)).logical_width() == 1536 &&
                  backdrops.at(size_t(options)).centre_left() == 256);
            // UW_OPTION_1_05 is the fifth tile: column 0 of row 1; the replaced texture.pak content is not a tile.
            const BackdropTile* tile = backdrops.tile(tile_hash(11));
            CHECK(tile && tile->backdrop == options && tile->column == 0 && tile->row == 1);
            CHECK(!backdrops.tile(tile_hash(9)));
            // The save game window: UW_option_07 is column 2 of row 1 of its picture, 406 pixels in on each side.
            const int saveload = backdrops.find("saveload");
            tile = backdrops.tile(tile_hash(17));
            CHECK(saveload >= 0 && tile && tile->backdrop == saveload && tile->column == 2 && tile->row == 1 &&
                  tile->role == BackdropTile::Tile && tile->screen == "saveload");
            CHECK(backdrops.at(size_t(saveload)).centre_left() == 406 && !backdrops.at(size_t(saveload)).alpha);
            { // The first-bytes test lets the screens' textures through and stops others.
                std::vector<uint8_t> seven(256 * 256 * 4, 7), eight(256 * 256 * 4, 8);
                CHECK(backdrops.candidate(texture_prefix_hash(seven.data(), seven.size())));
                CHECK(!backdrops.candidate(texture_prefix_hash(eight.data(), eight.size())));
            }

            tile = backdrops.tile(tile_hash(7));
            CHECK(tile && tile->backdrop == main && tile->column == 0 && tile->row == 0 &&
                  tile->role == BackdropTile::Tile);
            // The menu canvas textures are known with or without a picture of their screen.
            tile = backdrops.tile(tile_hash(13));
            CHECK(tile && tile->role == BackdropTile::Sky && tile->screen == "main-sacred" && tile->backdrop == -1);
            tile = backdrops.tile(tile_hash(15));
            CHECK(tile && tile->role == BackdropTile::Reflection && tile->screen == "main-underworld" &&
                  tile->backdrop == main);
            CHECK(MenuBackdrops({directory + "/texture.pak"}).tile(tile_hash(13)) != nullptr);
            // A loading bitmap by its stem, any case and path; its centre box-filtered into a bottom-up BGR bitmap.
            const int load = backdrops.loading("Z:\\pak/LoadingUW00.BMP");
            CHECK(load >= 0 && backdrops.loading("pak/loading0.bmp") < 0);
            const uint32_t pitch = 1024 * 3;
            std::vector<uint8_t> bgr(size_t(pitch) * 768);
            CHECK(backdrops.centre_bitmap(size_t(load), 1024, 768, pitch, bgr));
            // A 2x image 1280 screen pixels wide: screen pixel (0, 0) is image pixels (256..257, 0..1): red 150 (two
            // columns averaged), green 10, blue 32.
            const uint8_t* top_left = &bgr[size_t(767) * pitch];
            CHECK(top_left[2] == 150 && top_left[1] == 10 && top_left[0] == 32);
            const uint8_t* bottom_right = &bgr[1023 * 3];
            CHECK(bottom_right[1] == 20 && bottom_right[0] == uint8_t((256 + 2046) / 8));
            std::vector<uint8_t> small(10);
            CHECK(!backdrops.centre_bitmap(size_t(load), 1024, 768, pitch, small));
            // A directory of links whose relative targets are not there (copied away from the pictures): each link
            // and the empty result are noted.
            namespace fs = std::filesystem;
            fs::remove_all(directory + "/copied");
            fs::create_directories(directory + "/copied");
            fs::create_symlink("../pictures/01.png", directory + "/copied/main-sacred.png");
            fs::create_symlink("../pictures/05.png", directory + "/copied/options-underworld.png");
            const MenuBackdrops dangling(directory + "/copied", {});
            auto noted = [](const MenuBackdrops& b, const std::string& text) {
                return std::any_of(b.notes.begin(), b.notes.end(),
                                   [&](const std::string& note) { return note.find(text) != std::string::npos; });
            };
            CHECK(dangling.size() == 0 && dangling.notes.size() == 3 &&
                  noted(dangling, "main-sacred.png: a link to ../pictures/01.png, which does not exist; ignored") &&
                  noted(dangling, "no usable picture in " + fs::absolute(directory + "/copied").string() +
                                      " (relative to the working directory " + fs::current_path().string() + ")"));
            // A link whose target is there is read like a file.
            fs::create_symlink("../images/options-underworld.png", directory + "/copied/charselect-sacred.png");
            CHECK(MenuBackdrops(directory + "/copied", {}).size() == 1);
            // A directory that cannot be opened, and a relative one, are noted with the path as resolved.
            const MenuBackdrops missing(directory + "/nowhere", {});
            CHECK(missing.size() == 0 && missing.notes.size() == 1 &&
                  noted(missing, "cannot open the directory " + fs::absolute(directory + "/nowhere").string()));
            const std::string absolute = fs::absolute(directory + "/copied").string();
            CHECK(noted(MenuBackdrops(absolute + "/../nowhere", {}),
                        "cannot open the directory " + absolute + "/../nowhere: "));
            const MenuBackdrops relative("no-such-dir-for-menu-backdrops", {});
            CHECK(relative.notes.size() == 1 &&
                  noted(relative, "(relative to the working directory " + fs::current_path().string() + ")"));
            CHECK(MenuBackdrops("", {}).notes.empty());
        }

        { // The network windows' picture: their screen in the middle, mirrored at its edges out to the sides.
            std::vector<uint8_t> screen(size_t(1024) * 768 * 4);
            for (size_t i = 0; i < screen.size(); ++i) {
                screen[i] = uint8_t(i / 4 % 1024 * 7 + i % 4 + i / 4096); // column, channel and row in each byte
            }

            const Backdrop picture = network_picture(screen);
            CHECK(picture.key == "network" && picture.width == 1836 && picture.height == 768 && !picture.alpha);
            CHECK(picture.centre_left() == 406);
            auto pixel = [](const std::vector<uint8_t>& rgba, uint32_t width, uint32_t x, uint32_t y) {
                const size_t at = (size_t(y) * width + x) * 4;
                return std::array<uint8_t, 4>{rgba[at], rgba[at + 1], rgba[at + 2], rgba[at + 3]};
            };
            auto opaque = [&](uint32_t x, uint32_t y) {
                auto p = pixel(screen, 1024, x, y);
                p[3] = 255;
                return p;
            };
            CHECK(pixel(picture.rgba, 1836, 406, 5) == opaque(0, 5) &&
                  pixel(picture.rgba, 1836, 1429, 700) == opaque(1023, 700));
            CHECK(pixel(picture.rgba, 1836, 405, 5) == opaque(0, 5) &&
                  pixel(picture.rgba, 1836, 0, 9) == opaque(405, 9));
            CHECK(pixel(picture.rgba, 1836, 1430, 700) == opaque(1023, 700) &&
                  pixel(picture.rgba, 1836, 1835, 1) == opaque(1023 - 405, 1));
        }

        { // The wide water textures of the menu canvas (wide_water_texture).
            double x = 0, y = 0;
            // The main menu's floor: the middle of screen row 765 is about u 0.5 (8 pixels right of the canvas
            // middle), v 0.882; texels nearer the camera land lower, a texel past the camera nowhere.
            CHECK(water_floor_point(false, 0.5, 0.8819, x, y) && std::fabs(y - 765) < 2 && std::fabs(x - 8.3) < 0.01);
            double x2 = 0, y2 = 0;
            CHECK(water_floor_point(false, 0.1, 0.8819, x2, y2) && std::fabs(y2 - y) < 1e-9 && x2 < -880 && x2 > -940);
            CHECK(water_floor_point(true, 0.5, 0.8, x, y) && y > 600 && y < 768 &&
                  !water_floor_point(true, 0.5, 1.5, x, y));
            const uint32_t w = 64, h = 64;
            std::vector<uint8_t> original(size_t(w) * h * 4);
            for (uint32_t row = 0; row < h; ++row) {
                for (uint32_t column = 0; column < w; ++column) {
                    uint8_t* p = &original[(size_t(row) * w + column) * 4];
                    p[0] = uint8_t(column);
                    p[1] = uint8_t(row);
                    p[2] = 80;
                    p[3] = 200;
                }
            }

            for (uint32_t row = 0; row < h; ++row) {
                original[size_t(row) * w * 4] = 255; // a light edge column
            }

            auto at = [&](const std::vector<uint8_t>& image, uint32_t column, uint32_t row) {
                return &image[(size_t(row) * 2 * w + column) * 4];
            };
            // Without a picture: the original in the middle half, its two outer columns repaired, then the mirror
            // image at the old edges.
            const std::vector<uint8_t> plain = wide_water_texture(original, w, h, false, nullptr, false, false);
            CHECK(plain.size() == size_t(2 * w) * h * 4);
            CHECK(at(plain, w / 2 + 5, 7)[0] == 5 && at(plain, w / 2 + 5, 7)[1] == 7 &&
                  at(plain, w / 2 + 5, 7)[3] == 200);
            CHECK(at(plain, w / 2, 3)[0] == 2 && at(plain, w / 2 + 1, 3)[0] == 2 &&
                  at(plain, w / 2 + w - 1, 3)[0] == w - 3);
            CHECK(at(plain, w / 2 - 1, 3)[0] == 2);
            // A picture whose holes show water painted in one colour: the floor's sides carry it less the water's
            // light (39) beyond the mirrored band of 24 texels, the reflection's sides fade out.
            Backdrop picture;
            picture.key = "main-underworld";
            picture.width = 1836;
            picture.height = 768;
            picture.alpha = true;
            picture.rgba.assign(size_t(1836) * 768 * 4, 255);
            for (size_t i = 0; i < size_t(1836) * 768; ++i) {
                picture.rgba[i * 4] = 100;
                picture.rgba[i * 4 + 1] = 120;
                picture.rgba[i * 4 + 2] = 140;
                if (i / 1836 >= 600) {
                    picture.rgba[i * 4 + 3] = 0;
                }
            }

            const std::vector<uint8_t> floor = wide_water_texture(original, w, h, true, &picture, false, true);
            const uint8_t* far = at(floor, 0, 56); // v 0.88: the bottom of the screen, far left
            CHECK(far[0] == 61 && far[1] == 81 && far[2] == 101 && far[3] == 255);
            CHECK(at(floor, 2 * w - 1, 56)[0] == 61 && at(floor, w / 2 + 5, 56)[0] == 5);
            const std::vector<uint8_t> reflection = wide_water_texture(original, w, h, false, &picture, false, true);
            CHECK(at(reflection, 0, 56)[3] == 0 && at(reflection, w / 2 - 1, 56)[3] > 150);
            // That picture is flat: it does not show the floor's light and dark. One that does (the floor plus 39
            // under its holes) passes.
            // (a 256-texel floor, light and dark by rows: value row / 2; the picture's rows the floor rows they
            // show plus 39)
            const uint32_t big = 256;
            std::vector<uint8_t> rows(size_t(big) * big * 4, 255);
            for (uint32_t row = 0; row < big; ++row) {
                for (uint32_t column = 0; column < big; ++column) {
                    uint8_t* p = &rows[(size_t(row) * big + column) * 4];
                    p[0] = p[1] = p[2] = uint8_t(row / 2);
                }
            }

            Backdrop showing = picture;
            for (uint32_t row = 600; row < 768; ++row) {
                double v = 0.5; // the floor row seen on this screen row: water_floor_point inverted by bisection
                for (double lo = 0.0, hi = 1.0, k = 0; k < 40; ++k) {
                    v = (lo + hi) / 2;
                    double px, py;
                    water_floor_point(false, 0.5, v, px, py);
                    (py < row + 0.5 ? lo : hi) = v;
                }

                const uint8_t value = uint8_t(std::floor(v * big) / 2 + 39);
                for (uint32_t column = 0; column < 1836; ++column) {
                    uint8_t* p = &showing.rgba[(size_t(row) * 1836 + column) * 4];
                    p[0] = p[1] = p[2] = value;
                }
            }

            CHECK(!picture_shows_water(picture, rows, big, big, false) &&
                  picture_shows_water(showing, rows, big, big, false));
            // Without a usable picture the floor's sides are patches of itself matched to its edge row by row: a
            // floor of one colour stays that colour.
            std::vector<uint8_t> uniform(size_t(w) * h * 4, 90);
            const std::vector<uint8_t> made = wide_water_texture(uniform, w, h, true, nullptr, false, false);
            const std::vector<uint8_t> synthesised = wide_water_texture(uniform, w, h, true, &picture, true, false);
            CHECK(at(made, 0, 10)[0] == 90 && at(synthesised, 2 * w - 1, 63)[1] == 90 &&
                  at(synthesised, 0, 0)[3] == 90);
        }

        {
            // Which presents show the raised 1024x768 inset, in the orders recorded from the game
            // (SACRED_INSET_TRACE): a 2D screen draws and raises every frame and presents it once.
            InsetSchedule schedule;
            auto frame = [&](bool raised) {
                schedule.drew();
                return schedule.present(raised);
            };
            for (int i = 0; i < 3; ++i) {
                const auto shown = frame(true);
                CHECK(shown.on && shown.fresh);
            }

            // The first world frame after a loading screen (or after the world map, the options or the save game
            // window) draws its scene and raises nothing: shown whole, not inside the screen's 4:3 box.
            CHECK(schedule.pending(false)); // nothing drawn yet: the next present could repeat the last frame
            schedule.drew();                // BeginScene of the world frame
            CHECK(!schedule.pending(false));
            CHECK(!schedule.present(false).on);
            CHECK(!frame(false).on);
            // A frame presented twice (a Blt, then an Unlock of the primary surface) with nothing drawn between the
            // presents keeps its inset on the repeat, which is not fresh (it reuses the first present's picture).
            CHECK(frame(true).on);
            CHECK(schedule.pending(false));
            const auto repeat = schedule.present(false);
            CHECK(repeat.on && !repeat.fresh);
            CHECK(frame(true).on);
            // ...but a frame that raised nothing is not carried by its own repeat present, nor by later ones.
            CHECK(!frame(false).on && !schedule.present(false).on && !schedule.pending(false));
            // A raise always shows, whatever came before (a screen opening over the world).
            CHECK(schedule.pending(true));
            const auto opened = frame(true);
            CHECK(opened.on && opened.fresh);
        }

        std::printf("SURFACE_PIXELS_PASS checks=%u packed16_values=196608 overlap=1 clipping=1\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "SURFACE_PIXELS_FAILURE %s\n", error.what());
        return 1;
    }
}
