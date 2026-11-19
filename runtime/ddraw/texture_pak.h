#pragma once
#include "surface_pixels.h"
#include <functional>
#include <cstdint>
#include <string>
#include <vector>

namespace sacred {
// The game's texture paks (PAK/texture.pak, PAK/texture03.pak, "TEX\3"): every entry turned into the exact pixels
// the game unlocks into its texture surface, so that texture_hash() of the image is the hash the running game gets.
// Pak layout: "TEX\3", u32 count, u32 data size; at 0x100 count x {u32 kind, u32 offset, u32 size}; at offset a 0x50
// header {char name[32], u16 width, u16 height, u32 format | stored_size << 8, zero padding} and `size` bytes.
//   format 4: zlib stream of width x height A4R4G4B4 pixels (2 bytes, little endian)
//   format 6: raw width x height B,G,R,A bytes (A8R8G8B8, the same bytes in memory)
// The shipped texture.pak ends inside its last entries (MIX9997..9999); those are counted as truncated.
// A pak's bytes, mapped where the system can (texture.pak is ~860 MB; reading it through istreambuf_iterator held up
// to twice that on the heap whenever the menu pictures or the upscale index were made), read otherwise.
class PakBytes {
public:
    explicit PakBytes(const std::string& path);
    ~PakBytes();
    PakBytes(const PakBytes&) = delete;
    PakBytes& operator=(const PakBytes&) = delete;
    const uint8_t* data() const { return mapped_ ? static_cast<const uint8_t*>(mapped_) : copy_.data(); }
    size_t size() const { return size_; }
    uint8_t operator[](size_t at) const { return data()[at]; }
    explicit operator bool() const { return size_ != 0; }

private:
    void* mapped_ = nullptr;
    size_t size_ = 0;
    std::vector<uint8_t> copy_;
};
struct TexturePakStats {
    unsigned entries = 0, failed = 0, truncated = 0;
};
// Calls `entry` with the index, name and image of every readable entry. False (with *error) when the file is not a
// texture pak; entries that fail are counted in *stats and, with `report`, described on stderr.
bool texture_pak_read(
    const std::string& path,
    const std::function<void(unsigned index, const char* name, const PixelImage& image, unsigned pak_format)>& entry,
    TexturePakStats& stats, std::string* error, bool report = false);
}
