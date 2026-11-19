#include "file_metadata.h"
#include <cerrno>
#include <sys/stat.h>
#if defined(__linux__)
#include <fcntl.h>
#endif

namespace sacred {
namespace {
uint64_t ticks(int64_t seconds, uint32_t nanoseconds) {
    return uint64_t(seconds) * 10000000 + 116444736000000000ull + nanoseconds / 100;
}
}

int file_metadata(const std::filesystem::path& path, FileMetadata& result) {
    result = {};
#if defined(__linux__)
    struct statx info{};
    if (statx(AT_FDCWD, path.c_str(), 0, STATX_BASIC_STATS | STATX_BTIME, &info)) {
        return errno;
    }

    bool directory = S_ISDIR(info.stx_mode);
    result.attributes = (directory ? 0x10u : 0x20u) | (info.stx_mode & S_IWUSR ? 0u : 1u);
    result.size = directory ? 0 : info.stx_size;
    if (info.stx_mask & STATX_BTIME) {
        result.times[0] = ticks(info.stx_btime.tv_sec, info.stx_btime.tv_nsec);
    }

    result.times[1] = ticks(info.stx_atime.tv_sec, info.stx_atime.tv_nsec);
    result.times[2] = ticks(info.stx_mtime.tv_sec, info.stx_mtime.tv_nsec);
#elif defined(__APPLE__)
    struct stat info{};
    if (stat(path.c_str(), &info)) {
        return errno;
    }

    bool directory = S_ISDIR(info.st_mode);
    result.attributes = (directory ? 0x10u : 0x20u) | (info.st_mode & S_IWUSR ? 0u : 1u);
    result.size = directory ? 0 : info.st_size;
    result.times[0] = ticks(info.st_birthtimespec.tv_sec, info.st_birthtimespec.tv_nsec);
    result.times[1] = ticks(info.st_atimespec.tv_sec, info.st_atimespec.tv_nsec);
    result.times[2] = ticks(info.st_mtimespec.tv_sec, info.st_mtimespec.tv_nsec);
#else
#error Unsupported native file metadata platform
#endif
    return 0;
}
}
