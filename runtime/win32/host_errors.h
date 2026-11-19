#pragma once
#include <cerrno>
#include <cstdint>
#include <optional>

namespace sacred {
// Host errno -> Win32 error code for file APIs. Portable (POSIX errno names only, no Linux-specific values).
// nullopt means the errno has no honest Win32 equivalent for a file operation and the caller must trap.
inline std::optional<uint32_t> win32_error_from_errno(int value) {
    switch (value) {
    case ENOENT:
        return 2; // ERROR_FILE_NOT_FOUND
    case ENOTDIR:
        return 3; // ERROR_PATH_NOT_FOUND
    case EMFILE:
    case ENFILE:
        return 4; // ERROR_TOO_MANY_OPEN_FILES
    case EACCES:
    case EPERM:
    case EROFS:
    case EISDIR:
        return 5; // ERROR_ACCESS_DENIED
    case EBADF:
        return 6; // ERROR_INVALID_HANDLE
    case ENOMEM:
        return 8; // ERROR_NOT_ENOUGH_MEMORY
    case EXDEV:
        return 17; // ERROR_NOT_SAME_DEVICE
    case ENODEV:
    case ENXIO:
        return 21; // ERROR_NOT_READY
    case ESPIPE:
        return 25; // ERROR_SEEK
    case EBUSY:
    case ETXTBSY:
        return 32; // ERROR_SHARING_VIOLATION
    case EEXIST:
        return 80; // ERROR_FILE_EXISTS
    case EINVAL:
        return 87; // ERROR_INVALID_PARAMETER
    case EPIPE:
        return 109; // ERROR_BROKEN_PIPE
    case ENOSPC:
    case EDQUOT:
        return 112; // ERROR_DISK_FULL
    case ENOTEMPTY:
        return 145; // ERROR_DIR_NOT_EMPTY
    case ENAMETOOLONG:
        return 206; // ERROR_FILENAME_EXCED_RANGE
    case EFBIG:
        return 223; // ERROR_FILE_TOO_LARGE
    case EIO:
        return 1117; // ERROR_IO_DEVICE
    case ELOOP:
        return 1920; // ERROR_CANT_ACCESS_FILE
    default:
        return std::nullopt;
    }
}
}
