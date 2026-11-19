// Split menu pictures (data/menu-backgrounds, tools/menu/split_backgrounds.py): MenuBackdrops puts <key>.sides.png,
// <key>.mask.png and the game's own screen back together.
//   menu_split_tests GAME SPLIT_DIR [FULL_DIR]
// Every pair in SPLIT_DIR loads without a note of failure, and each composed picture has the SHA-256 that
// SPLIT_DIR/composed.sha256 records for it (written by tools/menu/split_backgrounds.py from its own composition): a
// change of the format, of the pairs or of the engine's composition that alters a picture fails here. With FULL_DIR
// (pictures kept privately) each result must also equal <key>.png there byte for byte.
#include "runtime/ddraw/menu_backdrops.h"
#include "runtime/ddraw/texture_pack.h"
#include "platform/digest.h"
#include <fstream>
#include <map>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <filesystem>
#include <stdexcept>
#include <string>

using namespace sacred;
namespace fs = std::filesystem;

int main(int argc, char** argv) {
    try {
        if (argc < 3) {
            std::fprintf(stderr, "usage: menu_split_tests GAME SPLIT_DIR [FULL_DIR]\n");
            return 2;
        }

        fs::path pak_dir;
        for (const auto& entry : fs::directory_iterator(argv[1])) {
            if (entry.path().filename().string() == "PAK" || entry.path().filename().string() == "pak") {
                pak_dir = entry.path();
            }
        }

        std::vector<fs::path> paks;
        for (const char* name : {"texture.pak", "texture03.pak"}) {
            for (const auto& entry : fs::directory_iterator(pak_dir)) {
                std::string file = entry.path().filename().string();
                for (char& c : file) {
                    c = char(std::tolower(static_cast<unsigned char>(c)));
                }

                if (file == name) {
                    paks.push_back(entry.path());
                }
            }
        }

        MenuBackdrops backdrops(argv[2], paks);
        unsigned pairs = 0, checked = 0;
        for (const auto& entry : fs::directory_iterator(argv[2])) {
            if (entry.path().filename().string().find(".sides.png") != std::string::npos) {
                ++pairs;
            }
        }

        for (const auto& note : backdrops.notes) {
            std::printf("note: %s\n", note.c_str());
            if (note.find("ignored") != std::string::npos) {
                throw std::runtime_error("FAIL: " + note);
            }
        }

        if (backdrops.size() != pairs) {
            throw std::runtime_error("FAIL: not every pair loaded");
        }

        std::map<std::string, std::string> expected;
        std::ifstream sums(fs::path(argv[2]) / "composed.sha256");
        for (std::string digest, key; sums >> digest >> key;) {
            expected[key] = digest;
        }

        unsigned hashed = 0;
        for (size_t i = 0; i < backdrops.size(); ++i) {
            const Backdrop& picture = backdrops.at(i);
            const auto digest = host_sha256(picture.rgba.data(), picture.rgba.size());
            char hex[65];
            for (int b = 0; b < 32; ++b) {
                std::snprintf(hex + 2 * b, 3, "%02x", digest[b]);
            }

            auto want = expected.find(picture.key);
            if (want == expected.end()) {
                throw std::runtime_error("FAIL: no recorded hash for " + picture.key);
            }

            if (want->second != hex) {
                throw std::runtime_error("FAIL: " + picture.key + " composes to another picture");
            }

            ++hashed;
        }

        if (argc > 3) {
            for (size_t i = 0; i < backdrops.size(); ++i) {
                const Backdrop& picture = backdrops.at(i);
                TextureReplacement full;
                if (!texture_png_read((fs::path(argv[3]) / (picture.key + ".png")).string(), full)) {
                    throw std::runtime_error("FAIL: no " + picture.key + ".png in " + argv[3]);
                }

                if (full.width != picture.width || full.height != picture.height || full.rgba != picture.rgba) {
                    throw std::runtime_error("FAIL: " + picture.key + " differs from the unsplit picture");
                }

                ++checked;
            }
        }

        // A profile overlay texture03.pak (tools/legacy_char_pak.py) with a repainted character select: the game
        // draws its tiles, but the picture is put together on the installation's own; without the installation's
        // paks the changes land on other pixels, which composed.sha256 catches.
        const fs::path overlay =
            fs::temp_directory_path() / ("menu_split_overlay_" + std::to_string(::getpid()) + ".pak");
        {
            std::vector<uint8_t> data(0x100, 0);
            data[0] = 'T';
            data[1] = 'E';
            data[2] = 'X';
            data[3] = 3;
            data[4] = 12;
            data.resize(0x100 + 12 * 12, 0);
            auto put32 = [&](size_t at, uint32_t v) {
                for (int i = 0; i < 4; ++i) {
                    data[at + i] = uint8_t(v >> (8 * i));
                }
            };
            for (uint32_t i = 0; i < 12; ++i) {
                put32(0x100 + 12 * i + 4, uint32_t(data.size()));
                put32(0x100 + 12 * i + 8, 256 * 256 * 4);
                std::vector<uint8_t> header(0x50, 0);
                char name[32];
                std::snprintf(name, sizeof name, "GUI_CHAR_%02u.TGA", i + 1);
                std::copy(name, name + std::strlen(name), header.begin());
                header[33] = 1;
                header[35] = 1;
                header[36] = 6; // 256 x 256, BGRA
                data.insert(data.end(), header.begin(), header.end());
                data.insert(data.end(), size_t(256) * 256 * 4, uint8_t(0x40 + i));
            }

            std::ofstream(overlay, std::ios::binary)
                .write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        }

        const std::vector<fs::path> drawn{paks.front(), overlay};
        MenuBackdrops with_originals(argv[2], drawn, paks), overlay_only(argv[2], drawn);
        fs::remove(overlay);
        const int composed = with_originals.find("charselect-sacred");
        if (composed < 0 || with_originals.size() != pairs) {
            throw std::runtime_error("FAIL: overlay: not every pair loaded");
        }

        if (with_originals.at(size_t(composed)).rgba !=
            backdrops.at(size_t(backdrops.find("charselect-sacred"))).rgba) {
            throw std::runtime_error("FAIL: overlay: charselect-sacred composed on the overlay's tiles");
        }

        if (overlay_only.find("charselect-sacred") >= 0) {
            throw std::runtime_error("FAIL: overlay: a picture on other tiles than the game's was not refused");
        }

        std::printf("MENU_SPLIT_PASS pictures=%zu hashes=%u compared_with_unsplit=%u overlay=ok\n", backdrops.size(),
                    hashed, checked);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
