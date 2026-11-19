#include "texture_pack.h"
#include "texture_pak.h"
#include "texture_upscale.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP // the loading bitmaps under split menu pictures (menu_backdrops.cpp)
#define STBI_NO_STDIO_WRITE
#define STBI_FAILURE_USERMSG
#include "../../third_party/stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_ONLY_PNG
#include "../../third_party/stb/stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace sacred {
uint64_t texture_hash(const PixelImage& image) {
    uint64_t hash = 0xcbf29ce484222325ull;
    const size_t row = size_t(image.width) * (describe(image.format).bits / 8);
    for (uint32_t y = 0; y < image.height; ++y) {
        const uint8_t* bytes = image.pixels.data() + size_t(y) * image.pitch;
        for (size_t i = 0; i < row; ++i) {
            hash ^= bytes[i];
            hash *= 0x100000001b3ull;
        }
    }

    return hash;
}

const char* texture_format_name(PixelFormat format) {
    switch (format) {
    case PixelFormat::Xrgb8888:
        return "X8R8G8B8";
    case PixelFormat::Argb8888:
        return "A8R8G8B8";
    case PixelFormat::Rgb565:
        return "R5G6B5";
    case PixelFormat::Argb1555:
        return "A1R5G5B5";
    case PixelFormat::Argb4444:
        return "A4R4G4B4";
    }

    return "UNKNOWN";
}

std::string texture_file_name(const PixelImage& image, uint64_t hash) {
    char name[96];
    std::snprintf(name, sizeof name, "%ux%u_%s_%016llx.png", image.width, image.height,
                  texture_format_name(image.format), static_cast<unsigned long long>(hash));
    return name;
}

unsigned texture_scale(uint32_t guest_width, uint32_t guest_height, uint32_t width, uint32_t height) {
    if (!guest_width || !guest_height || width < guest_width || width % guest_width || height % guest_height) {
        return 0;
    }

    const uint32_t k = width / guest_width;
    return height / guest_height == k ? k : 0;
}

unsigned render_scale_fit(uint32_t width, uint32_t height, unsigned k, uint32_t limit) {
    while (k > 1 && (uint64_t(width) * k > limit || uint64_t(height) * k > limit)) {
        --k;
    }

    return k ? k : 1;
}

bool texture_pack_hash(const std::string& file_name, uint64_t& hash) {
    if (file_name.size() < 20) {
        return false;
    }

    std::string tail = file_name.substr(file_name.size() - 4);
    for (auto& c : tail) {
        c = char(std::tolower(static_cast<unsigned char>(c)));
    }

    if (tail != ".png") {
        return false;
    }

    const size_t digits = file_name.size() - 20; // index of the 16 hex digits
    if (digits && file_name[digits - 1] != '_') {
        return false;
    }

    uint64_t value = 0;
    for (size_t i = digits; i < digits + 16; ++i) {
        const char c = char(std::tolower(static_cast<unsigned char>(file_name[i])));
        if (c >= '0' && c <= '9') {
            value = value << 4 | uint64_t(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            value = value << 4 | uint64_t(c - 'a' + 10);
        } else {
            return false;
        }
    }

    hash = value;
    return true;
}

bool texture_inflate(const uint8_t* data, size_t length, size_t size, std::vector<uint8_t>& out) {
    int produced = 0;
    char* bytes = stbi_zlib_decode_malloc_guesssize_headerflag(reinterpret_cast<const char*>(data), int(length),
                                                               int(size), &produced, 1);
    if (!bytes) {
        return false;
    }

    const bool exact = size_t(produced) == size;
    if (exact) {
        out.assign(bytes, bytes + produced);
    }

    STBI_FREE(bytes);
    return exact;
}

bool texture_png_read(const std::string& path, TextureReplacement& image) {
    int width = 0, height = 0, channels = 0;
    // Larger than any GPU takes (and than a sane pack holds): refused before allocating its pixels.
    if (!stbi_info(path.c_str(), &width, &height, &channels) || width <= 0 || height <= 0 ||
        width > int(texture_png_max_side) || height > int(texture_png_max_side)) {
        if (width > int(texture_png_max_side) || height > int(texture_png_max_side)) {
            stbi__err("too large", "larger than 16384 pixels on a side");
        }

        return false;
    }

    stbi_uc* pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (!pixels) {
        return false;
    }

    image.width = uint32_t(width);
    image.height = uint32_t(height);
    image.rgba.assign(pixels, pixels + size_t(width) * height * 4);
    stbi_image_free(pixels);
    return true;
}

bool texture_png_write(const std::string& path, uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba) {
    if (rgba.size() != size_t(width) * height * 4) {
        return false;
    }

    return stbi_write_png(path.c_str(), int(width), int(height), 4, rgba.data(), int(width * 4)) != 0;
}

namespace {
// "<size> <mtime>" of every pak, the key of the cached content list.
std::string pak_signature(const std::vector<std::string>& paks) {
    std::string key = "sacred-native texture index 1\n";
    for (const auto& pak : paks) {
        std::error_code error;
        const auto size = std::filesystem::file_size(pak, error);
        const auto time = std::filesystem::last_write_time(pak, error).time_since_epoch().count();
        key += pak + " " + std::to_string(error ? 0ull : (unsigned long long)size) + " " +
               std::to_string(error ? 0ll : (long long)time) + "\n";
    }

    return key;
}

bool read_index(const std::string& path, const std::string& signature, std::set<uint64_t>& hashes) {
    std::ifstream file(path, std::ios::binary);
    std::string header(signature.size(), '\0');
    if (!file.read(header.data(), std::streamsize(header.size())) || header != signature) {
        return false;
    }

    uint64_t count = 0;
    if (!file.read(reinterpret_cast<char*>(&count), 8) || count > (1u << 24)) {
        return false;
    }

    std::vector<uint64_t> values(count);
    if (!file.read(reinterpret_cast<char*>(values.data()), std::streamsize(count * 8))) {
        return false;
    }

    hashes.insert(values.begin(), values.end());
    return true;
}

void write_index(const std::string& path, const std::string& signature, const std::set<uint64_t>& hashes) {
    const std::string temporary = path + ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        const uint64_t count = hashes.size();
        const std::vector<uint64_t> values(hashes.begin(), hashes.end());
        file.write(signature.data(), std::streamsize(signature.size()));
        file.write(reinterpret_cast<const char*>(&count), 8);
        file.write(reinterpret_cast<const char*>(values.data()), std::streamsize(values.size() * 8));
        if (!file) {
            return;
        }
    }

    std::error_code error;
    std::filesystem::rename(temporary, path, error);
}
}

TexturePack::TexturePack(const std::string& directory, bool background, size_t cache_bytes, unsigned upscale,
                         std::vector<std::string> paks, std::string index_cache)
    : background_(background), cache_bytes_(cache_bytes), upscale_(upscale >= 2 && upscale <= 4 ? upscale : 0) {
    std::error_code error;
    size_t ignored = 0;
    if (!directory.empty()) {
        for (std::filesystem::recursive_directory_iterator it(directory, error), end; !error && it != end;
             it.increment(error)) {
            if (!it->is_regular_file(error)) {
                continue;
            }

            uint64_t hash = 0;
            if (!texture_pack_hash(it->path().filename().string(), hash)) {
                ++ignored;
                continue;
            }

            if (!paths_.emplace(hash, it->path().string()).second) {
                std::fprintf(stderr, "SACRED_TEXTURE_PACK duplicate hash %016llx: %s ignored\n",
                             static_cast<unsigned long long>(hash), it->path().string().c_str());
            }
        }

        if (error) {
            std::fprintf(stderr, "SACRED_TEXTURE_PACK cannot read %s: %s\n", directory.c_str(),
                         error.message().c_str());
        }
    }

    std::fprintf(stderr, "SACRED_TEXTURE_PACK dir=%s textures=%zu ignored=%zu cache_mb=%zu upscale=%u\n",
                 directory.empty() ? "-" : directory.c_str(), paths_.size(), ignored, cache_bytes_ >> 20, upscale_);
    if (upscale_) {
        auto index = [this, paks, index_cache] {
            const auto start = std::chrono::steady_clock::now();
            std::set<uint64_t> allowed;
            const std::string signature = pak_signature(paks);
            const bool cached = !index_cache.empty() && read_index(index_cache, signature, allowed);
            for (const auto& pak : cached ? std::vector<std::string>{} : paks) {
                TexturePakStats stats;
                std::string why;
                if (!texture_pak_read(
                        pak,
                        [&](unsigned, const char*, const PixelImage& image, unsigned) {
                            allowed.insert(texture_hash(image));
                        },
                        stats, &why)) {
                    std::fprintf(stderr, "SACRED_TEXTURE_UPSCALE %s\n", why.c_str());
                }
            }

            if (!cached && !index_cache.empty() && !allowed.empty()) {
                write_index(index_cache, signature, allowed);
            }

            allowed_ = std::move(allowed);
            indexed_.store(true, std::memory_order_release);
            std::fprintf(stderr, "SACRED_TEXTURE_UPSCALE scale=%u textures=%zu index=%s indexed_ms=%lld\n", upscale_,
                         allowed_.size(), cached ? "cached" : "read",
                         static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                    std::chrono::steady_clock::now() - start)
                                                    .count()));
        };
        if (background_) {
            indexer_ = std::thread(index);
        } else {
            index();
        }
    }

    if (background_ && (!paths_.empty() || upscale_)) {
        // Decoding a PNG takes a few ms; an upscale up to tens: give upscaling a few threads, leaving the guest and
        // render threads their cores.
        const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
        const unsigned count = upscale_ ? std::clamp(cores > 3 ? cores - 2 : 1u, 1u, 3u) : 1u;
        for (unsigned i = 0; i < count; ++i) {
            workers_.emplace_back([this] { work(); });
        }
    }
}

void TexturePack::work() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
        if (stop_) {
            return;
        }

        const Job job = std::move(queue_.front());
        queue_.pop_front();
        lock.unlock();
        decode(job);
        lock.lock();
        queued_.erase(job.hash);
    }
}

TexturePack::~TexturePack() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }

    wake_.notify_all();
    for (auto& worker : workers_) {
        worker.join();
    }

    if (indexer_.joinable()) {
        indexer_.join();
    }
}

std::shared_ptr<const TextureReplacement> TexturePack::decode(const Job& job) {
    auto path = paths_.find(job.hash);
    const bool from_file = path != paths_.end();
    if (!from_file && !job.rgba) {
        return nullptr;
    }

    const auto start = std::chrono::steady_clock::now();
    auto image = std::make_shared<TextureReplacement>();
    bool valid = false;
    try {
        if (from_file) {
            valid = texture_png_read(path->second, *image);
        } else {
            image->width = job.width * upscale_;
            image->height = job.height * upscale_;
            image->rgba = texture_upscale(*job.rgba, job.width, job.height, upscale_);
            valid = true;
        }
    } catch (const std::exception&) {
        valid = false;
    } // out of memory for one image: treated as undecodable

    const uint64_t us = uint64_t(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
    std::lock_guard<std::mutex> lock(mutex_);
    if (!valid) {
        if (failed_.insert(job.hash).second && from_file) {
            std::fprintf(stderr, "SACRED_TEXTURE_PACK cannot decode %s: %s\n", path->second.c_str(),
                         stbi_failure_reason() ? stbi_failure_reason() : "unknown");
        }

        ++stats_.failed;
        return nullptr;
    }

    if (from_file) {
        ++stats_.decoded;
        stats_.decode_us += us;
        stats_.max_decode_us = std::max(stats_.max_decode_us, us);
    } else {
        ++stats_.upscaled;
        stats_.upscale_us += us;
        stats_.max_upscale_us = std::max(stats_.max_upscale_us, us);
    }

    insert(job.hash, image);
    return image;
}

// Called with mutex_ held.
void TexturePack::insert(uint64_t hash, std::shared_ptr<const TextureReplacement> image) {
    if (cache_.count(hash)) {
        return;
    }

    recent_.push_front(hash);
    cached_bytes_ += image->rgba.size();
    cache_.emplace(hash, Cached{std::move(image), recent_.begin()});
    while (cached_bytes_ > cache_bytes_ && recent_.size() > 1) {
        auto oldest = cache_.find(recent_.back());
        cached_bytes_ -= oldest->second.image->rgba.size();
        cache_.erase(oldest);
        recent_.pop_back();
        ++stats_.evicted;
    }
}

namespace {
// The guest's pixels as the pack builder sees them (the dump's PNG content), for an upscale job.
std::shared_ptr<std::vector<uint8_t>> source_of(const PixelImage* guest) {
    return guest ? std::make_shared<std::vector<uint8_t>>(unpack(*guest)) : nullptr;
}
}

std::shared_ptr<const TextureReplacement> TexturePack::ready(uint64_t hash, const PixelImage* guest) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = cache_.find(hash);
        if (found != cache_.end()) {
            recent_.splice(recent_.begin(), recent_, found->second.place);
            return found->second.image;
        }

        if (background_ || failed_.count(hash)) {
            return nullptr;
        }
    }

    if (!paths_.count(hash) && !upscalable(hash)) {
        return nullptr;
    }

    return decode(Job{hash, paths_.count(hash) ? nullptr : source_of(guest), guest ? guest->width : 0,
                      guest ? guest->height : 0});
}

void TexturePack::request(uint64_t hash, const PixelImage* guest) {
    const bool file = paths_.count(hash) != 0;
    if (!background_ || (!file && (!upscalable(hash) || !guest))) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cache_.count(hash) || failed_.count(hash) || !queued_.insert(hash).second) {
            return;
        }
    }

    Job job{hash, file ? nullptr : source_of(guest), guest ? guest->width : 0, guest ? guest->height : 0};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(job));
    }

    wake_.notify_one();
}

std::shared_ptr<const TextureReplacement> TexturePack::find(uint64_t hash, const PixelImage* guest) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = cache_.find(hash);
        if (found != cache_.end()) {
            return found->second.image;
        }

        if (failed_.count(hash)) {
            return nullptr;
        }
    }

    if (!paths_.count(hash) && !upscalable(hash)) {
        return nullptr;
    }

    return decode(Job{hash, paths_.count(hash) ? nullptr : source_of(guest), guest ? guest->width : 0,
                      guest ? guest->height : 0});
}

TexturePack::Stats TexturePack::stats() {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

TextureDump::TextureDump(std::string directory) : directory_(std::move(directory)) {
    std::error_code error;
    std::filesystem::create_directories(directory_, error);
    if (error) {
        std::fprintf(stderr, "SACRED_TEXTURE_DUMP cannot create %s: %s\n", directory_.c_str(), error.message().c_str());
    }
}

bool TextureDump::write(const PixelImage& image, uint64_t hash) {
    if (!written_.insert(hash).second) {
        return false;
    }

    const std::string path = directory_ + "/" + texture_file_name(image, hash);
    std::error_code error;
    if (std::filesystem::exists(path, error)) {
        return false;
    }

    if (!texture_png_write(path, image.width, image.height, unpack(image))) {
        std::fprintf(stderr, "SACRED_TEXTURE_DUMP cannot write %s\n", path.c_str());
        return false;
    }

    return true;
}
}
