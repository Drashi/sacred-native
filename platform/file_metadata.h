#pragma once
#include <array>
#include <cstdint>
#include <filesystem>

namespace sacred {
struct FileMetadata {
    uint32_t attributes = 0;
    uint64_t size = 0;
    // Creation, access and modification times in 100 ns ticks since 1601.
    // An unavailable filesystem creation time remains zero.
    std::array<uint64_t, 3> times{};
};
int file_metadata(const std::filesystem::path& path, FileMetadata& result);
}
