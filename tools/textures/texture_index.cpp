// Offline texture index for texture packs: every entry of the game's texture paks (PAK/texture.pak,
// PAK/texture03.pak, "TEX\3"), turned into the exact pixel bytes the game unlocks into its texture surface and hashed
// with the shim's texture_hash(), so a pack can be built for textures nobody has seen in play.
//
// The pak format and the conversion are runtime/ddraw/texture_pak.h, shared with the engine (TEXTURE_UPSCALE).
//
// Usage: texture_index [--export DIR] [--skip PREFIX]... PAK...
//   stdout: one TSV line per entry: pak, index, name, width, height, pak format, guest format, hash16
//   --export: writes each distinct content once as DIR/<w>x<h>_<FORMAT>_<hash16>.png (RGBA, as the dump does)
//   --skip: entries whose name starts with PREFIX (case-insensitive) are listed but not exported, e.g. MINIMAP
#include "runtime/ddraw/texture_pack.h"
#include "runtime/ddraw/texture_pak.h"
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <strings.h>
#include <vector>

using namespace sacred;
int main(int argc, char** argv) {
    std::string export_dir;
    std::vector<std::string> paks, skip;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--export") && i + 1 < argc) {
            export_dir = argv[++i];
        } else if (!std::strcmp(argv[i], "--skip") && i + 1 < argc) {
            skip.push_back(argv[++i]);
        } else {
            paks.push_back(argv[i]);
        }
    }

    auto skipped = [&](const char* name) {
        for (const auto& prefix : skip) {
            if (!strncasecmp(name, prefix.c_str(), prefix.size())) {
                return true;
            }
        }

        return false;
    };
    if (paks.empty()) {
        std::fprintf(stderr, "usage: texture_index [--export DIR] [--skip PREFIX]... PAK...\n");
        return 2;
    }

    std::unique_ptr<TextureDump> dump;
    if (!export_dir.empty()) {
        dump = std::make_unique<TextureDump>(export_dir);
    }

    std::set<uint64_t> distinct;
    TexturePakStats stats;
    unsigned exported = 0;
    for (const auto& pak : paks) {
        std::string error;
        const bool ok = texture_pak_read(
            pak,
            [&](unsigned index, const char* name, const PixelImage& image, unsigned format) {
                const uint64_t hash = texture_hash(image);
                distinct.insert(hash);
                std::printf("%s\t%u\t%s\t%u\t%u\t%u\t%s\t%016llx\n", pak.c_str(), index, name, image.width,
                            image.height, format, texture_format_name(image.format),
                            static_cast<unsigned long long>(hash));
                if (dump && !skipped(name) && dump->write(image, hash)) {
                    ++exported;
                }
            },
            stats, &error, true);
        if (!ok) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
    }

    const unsigned entries = stats.entries, failed = stats.failed, truncated = stats.truncated;
    std::fprintf(stderr, "TEXTURE_INDEX entries=%u distinct=%zu failed=%u truncated=%u exported=%u\n", entries,
                 distinct.size(), failed, truncated, exported);
    return failed ? 1 : 0;
}
