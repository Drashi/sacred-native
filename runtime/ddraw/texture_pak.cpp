#include "texture_pak.h"
#include "texture_pack.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sacred {
PakBytes::PakBytes(const std::string& path) {
#ifndef _WIN32
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd >= 0) {
        struct stat info{};
        if (::fstat(fd, &info) == 0 && info.st_size > 0) {
            void* mapped = ::mmap(nullptr, size_t(info.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
            if (mapped != MAP_FAILED) {
                mapped_ = mapped;
                size_ = size_t(info.st_size);
            }
        }

        ::close(fd);
        if (mapped_) {
            return;
        }
    }
#endif
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return;
    }

    const std::streamoff length = file.tellg();
    if (length <= 0) {
        return;
    }

    copy_.resize(size_t(length));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(copy_.data()), length)) {
        copy_.clear();
    }

    size_ = copy_.size();
}

PakBytes::~PakBytes() {
#ifndef _WIN32
    if (mapped_) {
        ::munmap(mapped_, size_);
    }
#endif
}

namespace {
uint32_t u32(const PakBytes& data, size_t at) {
    return uint32_t(data[at]) | uint32_t(data[at + 1]) << 8 | uint32_t(data[at + 2]) << 16 |
           uint32_t(data[at + 3]) << 24;
}

uint16_t u16(const PakBytes& data, size_t at) {
    return uint16_t(data[at] | data[at + 1] << 8);
}
}

bool texture_pak_read(const std::string& path,
                      const std::function<void(unsigned, const char*, const PixelImage&, unsigned)>& entry,
                      TexturePakStats& stats, std::string* error, bool report) {
    const PakBytes data(path);
    if (data.size() < 0x100 || std::memcmp(data.data(), "TEX\3", 4)) {
        if (error) {
            *error = path + ": not a texture pak";
        }

        return false;
    }

    const uint32_t count = u32(data, 4);
    if (0x100 + uint64_t(count) * 12 > data.size()) {
        if (error) {
            *error = path + ": truncated index";
        }

        return false;
    }

    for (uint32_t index = 0; index < count; ++index) {
        const uint32_t offset = u32(data, 0x100 + index * 12 + 4), size = u32(data, 0x100 + index * 12 + 8);
        if (!size) {
            continue;
        }

        if (uint64_t(offset) + 0x50 + size > data.size()) {
            ++stats.truncated;
            if (report) {
                std::fprintf(stderr, "%s #%u: entry runs past the end of the file; skipped\n", path.c_str(), index);
            }

            continue;
        }

        char name[33]{};
        std::memcpy(name, data.data() + offset, 32);
        const uint32_t width = u16(data, offset + 32), height = u16(data, offset + 34);
        const uint32_t format = u32(data, offset + 36) & 0xff;
        const uint8_t* body = data.data() + offset + 0x50;
        PixelImage image{width, height, 0, PixelFormat::Argb4444, {}};
        bool ok = width && height;
        if (ok && format == 4) {
            image.pitch = width * 2;
            ok = texture_inflate(body, size, size_t(image.pitch) * height, image.pixels);
        } else if (ok && format == 6) {
            image.format = PixelFormat::Argb8888;
            image.pitch = width * 4;
            ok = size == size_t(image.pitch) * height;
            if (ok) {
                image.pixels.assign(body, body + size);
            }
        } else {
            ok = false;
        }

        ++stats.entries;
        if (!ok) {
            ++stats.failed;
            if (report) {
                std::fprintf(stderr, "%s #%u %s: unsupported or corrupt (format %u, %ux%u, %u bytes)\n", path.c_str(),
                             index, name, format, width, height, size);
            }

            continue;
        }

        entry(index, name, image, format);
    }

    return true;
}
}
