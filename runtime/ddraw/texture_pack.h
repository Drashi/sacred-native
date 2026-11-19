#pragma once
#include "surface_pixels.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <list>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace sacred {
// Opt-in texture replacement (off unless SACRED_TEXTURE_PACK / TEXTURE_PACK or SACRED_TEXTURE_DUMP names a directory).
// A texture is identified by its content: FNV-1a 64 over the guest-format pixel rows (width * bytes per pixel of every
// row, pitch padding skipped), taken when the guest unlocks the whole texture surface.
uint64_t texture_hash(const PixelImage& image);
// "A4R4G4B4" and friends: the D3D names of the guest pixel formats.
const char* texture_format_name(PixelFormat format);
// "<w>x<h>_<FORMAT>_<hash16>.png", the dump's file name for one content.
std::string texture_file_name(const PixelImage& image, uint64_t hash);
// The integer factor k with replacement = k x guest in both axes, 0 when there is none (a different factor per
// axis, a smaller image, or a remainder).
unsigned texture_scale(uint32_t guest_width, uint32_t guest_height, uint32_t width, uint32_t height);
// The render scale a k x surface of width x height can get within the GPU's texture size limit: k lowered until it
// fits, never below 1 (a 1920x1080 target at k=4 is 7680x4320, which an 8192 limit takes and a 4096 one takes at 2).
unsigned render_scale_fit(uint32_t width, uint32_t height, unsigned k, uint32_t limit);
// Pack PNGs larger than this on either side are refused undecoded.
constexpr uint32_t texture_png_max_side = 16384;
// The hash of a pack file name ending in "_<hash16>.png" or equal to "<hash16>.png" (16 hex digits, any case).
bool texture_pack_hash(const std::string& file_name, uint64_t& hash);
struct TextureReplacement {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba; // top row first, 4 bytes per pixel
};
// The pack directory is indexed when constructed (recursively, file names only: a full pack holds tens of thousands
// of files and many GB of pixels, so nothing is decoded up front). A replacement is decoded when its content shows
// up: ready() returns it from a byte-bounded LRU cache of decoded images, request() queues it for the background
// decoder, which never blocks the guest thread; the caller applies it once ready() has it (a frame or two later).
// A synchronous pack (tests) decodes inside ready() instead. Files that fail to decode are logged once and never
// match.
//
// With `upscale` 2..4 (TEXTURE_UPSCALE) every texture of the game's texture paks that the pack has no file for is made
// from the game's own pixels instead (texture_upscale, the offline pack builder's algorithm), on worker threads, into
// the same cache. Which contents qualify is learnt by reading `paks` on a background thread at start (until then
// nothing is upscaled): textures the game makes at run time (glyph caches) keep their pixels.
class TexturePack {
public:
    // cache_bytes: decoded images kept in memory (least recently used ones are dropped beyond it).
    // index_cache: a file that keeps the paks' content list between runs (reading the paks takes seconds; the list
    // is reused while every pak has the same size and modification time).
    explicit TexturePack(const std::string& directory, bool background = true, size_t cache_bytes = 512u << 20,
                         unsigned upscale = 0, std::vector<std::string> paks = {}, std::string index_cache = {});
    ~TexturePack();
    TexturePack(const TexturePack&) = delete;
    TexturePack& operator=(const TexturePack&) = delete;
    size_t size() const { return paths_.size(); }
    bool contains(uint64_t hash) const { return paths_.count(hash) != 0 || upscalable(hash); }
    // The replacement, or null: not in the pack, undecodable, or (background pack) not ready yet. `guest` (the
    // texture's own pixels) lets a synchronous pack upscale.
    std::shared_ptr<const TextureReplacement> ready(uint64_t hash, const PixelImage* guest = nullptr);
    // Queues a decode or an upscale of `guest` for the background workers (no-op when cached, queued, failed or not
    // in the pack).
    void request(uint64_t hash, const PixelImage* guest = nullptr);
    // Decodes now on the calling thread (tests, tools); null when not in the pack or undecodable.
    std::shared_ptr<const TextureReplacement> find(uint64_t hash, const PixelImage* guest = nullptr);
    // Upscale mode: true once the paks are read (before that, contains() knows only the pack's files).
    bool upscale_ready() const { return indexed_.load(std::memory_order_acquire); }
    struct Stats {
        uint64_t decoded = 0, decode_us = 0, max_decode_us = 0, evicted = 0, failed = 0;
        uint64_t upscaled = 0, upscale_us = 0, max_upscale_us = 0;
    };
    Stats stats();

private:
    struct Job {
        uint64_t hash;
        std::shared_ptr<std::vector<uint8_t>> rgba;
        uint32_t width = 0, height = 0;
    };
    bool upscalable(uint64_t hash) const {
        return upscale_ && indexed_.load(std::memory_order_acquire) && allowed_.count(hash) && !paths_.count(hash);
    }

    std::shared_ptr<const TextureReplacement> decode(const Job& job);
    void insert(uint64_t hash, std::shared_ptr<const TextureReplacement> image);
    void work();
    std::map<uint64_t, std::string> paths_; // fixed after the constructor
    const bool background_;
    const size_t cache_bytes_;
    const unsigned upscale_;
    std::set<uint64_t> allowed_; // pak contents; fixed once indexed_ is set
    std::atomic<bool> indexed_{false};
    std::mutex mutex_;
    std::condition_variable wake_;
    std::list<uint64_t> recent_; // most recently used first
    struct Cached {
        std::shared_ptr<const TextureReplacement> image;
        std::list<uint64_t>::iterator place;
    };
    std::map<uint64_t, Cached> cache_;
    size_t cached_bytes_ = 0;
    std::set<uint64_t> queued_, failed_;
    std::deque<Job> queue_;
    Stats stats_;
    bool stop_ = false;
    std::thread indexer_;
    std::vector<std::thread> workers_;
};
// Writes each distinct texture content once as an RGBA PNG named texture_file_name() (dump mode).
class TextureDump {
public:
    explicit TextureDump(std::string directory);
    // Returns true when this content was new and has been written.
    bool write(const PixelImage& image, uint64_t hash);

private:
    std::string directory_;
    std::set<uint64_t> written_;
};
// A zlib stream (texture.pak entries) inflated into exactly `size` bytes; false on a corrupt stream or another size.
bool texture_inflate(const uint8_t* data, size_t length, size_t size, std::vector<uint8_t>& out);
// PNG helpers for the dump and the tests (RGBA, top row first).
bool texture_png_write(const std::string& path, uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba);
bool texture_png_read(const std::string& path, TextureReplacement& image);
}
