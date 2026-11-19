#include "../../platform/qa.h"
#include "startup.h"
#include "../../platform/calendar.h"
#include "../../platform/system_info.h"
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <thread>
#include <sys/stat.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

namespace sacred {
namespace {
bool file_match(std::string pattern, std::string name) {
    for (auto* text : {&pattern, &name}) {
        for (char& ch : *text) {
            if (ch >= 'A' && ch <= 'Z') {
                ch += 'a' - 'A';
            }
        }
    }

    if (pattern == "*.*") {
        pattern = "*";
    }

    if (pattern.size() >= 2 && pattern.substr(pattern.size() - 2) == ".*" && name.find('.') == std::string::npos) {
        pattern.resize(pattern.size() - 2);
    }

    size_t p = 0, n = 0, star = std::string::npos, matched = 0;
    while (n < name.size()) {
        if (p < pattern.size() && pattern[p] == name[n]) {
            ++p;
            ++n;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            matched = n;
        } else if (star != std::string::npos) {
            p = star + 1;
            n = ++matched;
        } else {
            return false;
        }
    }

    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }

    return p == pattern.size();
}

struct Descriptor {
    StartupApi api;
    const char* dll;
    const char* symbol;
    unsigned count;
    ReturnChannel result;
    unsigned in, out;
};
const Descriptor descriptors[] = {
#define API(id, dll, symbol, count, result, in, out)                                                                   \
    {StartupApi::id, dll, symbol, count, ReturnChannel::result, in, out},
#include "startup.def"
#undef API
};
static_assert(sizeof descriptors / sizeof descriptors[0] == 56);
const Descriptor& descriptor(StartupApi api) {
    return descriptors[static_cast<unsigned>(api)];
}

const uint16_t cp1252[] = {0x20ac, 0x81,   0x201a, 0x192,  0x201e, 0x2026, 0x2020, 0x2021, 0x2c6,  0x2030, 0x160,
                           0x2039, 0x152,  0x8d,   0x17d,  0x8f,   0x90,   0x2018, 0x2019, 0x201c, 0x201d, 0x2022,
                           0x2013, 0x2014, 0x2dc,  0x2122, 0x161,  0x203a, 0x153,  0x9d,   0x17e,  0x178};
uint16_t decode(uint8_t byte) {
    return byte >= 0x80 && byte < 0xa0 ? cp1252[byte - 0x80] : byte;
}

uint8_t encode(uint16_t value, bool& used, uint8_t fallback) {
    if (value < 0x80 || (value >= 0xa0 && value <= 0xff)) {
        return uint8_t(value);
    }

    for (unsigned i = 0; i < 32; ++i) {
        if (cp1252[i] == value) {
            return uint8_t(0x80 + i);
        }
    }

    used = true;
    return fallback;
}

std::string basename(std::string value) {
    auto p = value.find_last_of("\\/");
    if (p != std::string::npos) {
        value.erase(0, p + 1);
    }

    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') {
            c += 'a' - 'A';
        }
    }

    return value;
}

uint32_t host_error(int value) {
    if (auto code = win32_error_from_errno(value)) {
        return *code;
    }

    throw GuestFault("unmapped startup host error " + std::to_string(value));
}
}

Signature Startup::signature(StartupApi api) {
    const auto& d = descriptor(api);
    Signature s{std::string(d.dll) == "msvcrt.dll" ? Convention::Cdecl : Convention::Stdcall, d.result,
                std::vector<Argument>(d.count), ""};
    for (unsigned i = 0; i < d.count; ++i) {
        unsigned flags = ((d.in >> i) & 1) | (((d.out >> i) & 1) << 1);
        s.arguments[i].direction = flags == 3   ? PointerDirection::InOut
                                   : flags == 2 ? PointerDirection::Out
                                   : flags == 1 ? PointerDirection::In
                                                : PointerDirection::Value;
    }

    if (api == StartupApi::InitTerm) {
        s.callback_signature = "cdecl void(void)";
    }

    if (api == StartupApi::FlsAlloc) {
        s.callback_signature = "stdcall void(guest_pointer)";
    }

    if (api == StartupApi::SetUnhandledExceptionFilter) {
        s.callback_signature = "stdcall LONG(EXCEPTION_POINTERS*)";
    }

    return s;
}

const char* Startup::symbol(StartupApi api) {
    return descriptor(api).symbol;
}

Startup::Startup(Control& c, const Loader& loader, StartupEnvironment environment)
    : control_(c), loader_(loader), environment_(std::move(environment)) {
    if (loader.native_modules().empty()) {
        throw GuestFault("Startup requires native module metadata");
    }

    for (int fd = 0; fd < 3; ++fd) {
        struct stat info{};
        if (fstat(fd, &info)) {
            if (errno == EBADF) {
                continue;
            }

            throw GuestFault("cannot inspect startup standard handle");
        }

        uint32_t type = environment_.standard_types[fd];
        if (!type) {
            type = S_ISCHR(info.st_mode) ? 2 : (S_ISFIFO(info.st_mode) || S_ISSOCK(info.st_mode)) ? 3 : 1;
        }

        handles_.emplace(0xe0000000 + fd, Handle{fd, type, {}});
    }

    next_handle_ += 3;
    if (environment_.command_line.empty()) {
        std::string executable = (std::filesystem::canonical(environment_.assets) / "Sacred.exe").string();
        std::replace(executable.begin(), executable.end(), '/', '\\');
        environment_.command_line = "\"Z:" + executable + "\"";
    }

    command_line_ = c.heap.allocate(uint32_t(environment_.command_line.size()) + 1);
    put_ansi(command_line_, environment_.command_line);
    if (!environment_.profile.empty()) {
        auto assets = std::filesystem::canonical(environment_.assets);
        auto profile = std::filesystem::weakly_canonical(std::filesystem::absolute(environment_.profile));
        auto relative = profile.lexically_relative(assets);
        if (relative.empty() || *relative.begin() != "..") {
            throw GuestFault("private profile must be outside the installation");
        }

        std::filesystem::create_directories(environment_.profile);
        auto settings = std::filesystem::path(environment_.profile) / "Settings.cfg";
        if (!std::filesystem::exists(settings)) {
            std::filesystem::copy_file(std::filesystem::path(environment_.assets) / "Settings.cfg", settings);
        }
    }
}

uint64_t Startup::counter() const {
    if (environment_.counter_ns) {
        return environment_.counter_ns();
    }

    if (environment_.tick_count) {
        return uint64_t(environment_.tick_count()) * 1000000;
    }

    static const auto epoch = std::chrono::steady_clock::now();
    const uint64_t now = uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
    const uint64_t origin =
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(epoch.time_since_epoch()).count());
    return origin + (now - origin) * std::max<uint32_t>(environment_.time_scale, 1);
}

Startup::~Startup() {
    shutdown_threads();
    for (const auto& h : handles_) {
        if (h.first >= 0xe0000003 && h.second.fd >= 0) {
            close(h.second.fd);
        }
    }
}

void Startup::bind() {
    control_.host(
        loader_.resolve("kernel32.dll", "FindClose"), {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            if (!finds_.erase(uint32_t(a[0]))) {
                return Result{error(6), {}};
            }

            return Result{1, {}};
        },
        "kernel32.dll!FindClose");
    control_.host(
        loader_.resolve("kernel32.dll", "FindNextFileA"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4, PointerDirection::Out}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            auto search = finds_.find(uint32_t(a[0]));
            if (search == finds_.end()) {
                return Result{error(6), {}};
            }

            auto& state = search->second;
            if (state.next == state.entries.size()) {
                return Result{error(18), {}};
            }

            find_data(state.entries[state.next], uint32_t(a[1]));
            ++state.next;
            return Result{1, {}};
        },
        "kernel32.dll!FindNextFileA");
    control_.host(
        loader_.resolve("kernel32.dll", "FileTimeToSystemTime"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4, PointerDirection::In}, {4, PointerDirection::Out}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            c.memory.check(uint32_t(a[0]), 8, Read);
            uint64_t value = c.memory.load32(uint32_t(a[0])) | uint64_t(c.memory.load32(uint32_t(a[0]) + 4)) << 32;
            if (value > INT64_MAX) {
                return Result{error(87), {}};
            }

            int64_t delta = int64_t(value) - 116444736000000000ll;
            time_t seconds = delta / 10000000;
            if (delta % 10000000 < 0) {
                --seconds;
            }

            tm calendar{};
            if (!calendar_utc(&seconds, &calendar)) {
                throw GuestFault("cannot convert native FILETIME calendar");
            }

            const uint16_t fields[] = {uint16_t(calendar.tm_year + 1900), uint16_t(calendar.tm_mon + 1),
                                       uint16_t(calendar.tm_wday),        uint16_t(calendar.tm_mday),
                                       uint16_t(calendar.tm_hour),        uint16_t(calendar.tm_min),
                                       uint16_t(calendar.tm_sec),         uint16_t(value % 10000000 / 10000)};
            c.memory.write(uint32_t(a[1]), fields, sizeof fields);
            return Result{1, {}};
        },
        "kernel32.dll!FileTimeToSystemTime");
    control_.host(
        loader_.resolve("kernel32.dll", "FileTimeToLocalFileTime"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4, PointerDirection::In}, {4, PointerDirection::Out}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            c.memory.check(uint32_t(a[0]), 8, Read);
            c.memory.check(uint32_t(a[1]), 8, Write);
            uint64_t utc = c.memory.load32(uint32_t(a[0])) | uint64_t(c.memory.load32(uint32_t(a[0]) + 4)) << 32;
            time_t now = (int64_t(file_time()) - 116444736000000000ll) / 10000000;
            tm current{};
            if (!calendar_local(&now, &current)) {
                throw GuestFault("cannot obtain current timezone bias");
            }

            uint64_t local = utc + uint64_t(int64_t(calendar_offset(current)) * 10000000);
            c.memory.store32(uint32_t(a[1]), uint32_t(local));
            c.memory.store32(uint32_t(a[1]) + 4, uint32_t(local >> 32));
            return Result{1, {}};
        },
        "kernel32.dll!FileTimeToLocalFileTime");
    control_.host(
        loader_.resolve("kernel32.dll", "FindFirstFileA"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4, PointerDirection::In}, {4, PointerDirection::Out}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) -> Result {
            std::string input = ansi(uint32_t(a[0]));
            std::replace(input.begin(), input.end(), '\\', '/');
            std::filesystem::path query(input), directory = query.parent_path();
            if (directory.empty()) {
                directory = ".";
            }

            std::string pattern = query.filename().string();
            if (pattern.find('?') != std::string::npos) {
                throw GuestFault("unqualified DOS question-mark wildcard");
            }

            c.memory.check(uint32_t(a[1]), 320, Write);
            auto lower = file_path(directory.string(), false, 3, false);
            auto view = file_path(directory.string(), false, 3);
            std::map<std::string, std::string> names;
            bool exists = false;
            auto collect = [&](const std::filesystem::path& path) {
                if (!std::filesystem::is_directory(path)) {
                    return;
                }

                exists = true;
                std::set<std::string> local;
                for (const auto& entry : std::filesystem::directory_iterator(path)) {
                    std::string name = entry.path().filename().string(), key = name;
                    for (char& ch : key) {
                        if (ch >= 'A' && ch <= 'Z') {
                            ch += 'a' - 'A';
                        }
                    }

                    if (!local.insert(key).second) {
                        throw GuestFault("case-colliding file enumeration");
                    }

                    names[key] = name;
                }
            };
            collect(lower);
            if (view != lower) {
                collect(view);
            }

            if (!exists) {
                return Result{error(3, UINT32_MAX), {}};
            }

            for (const char* dot : {".", ".."}) {
                if (file_match(pattern, dot)) {
                    names[dot] = dot;
                }
            }

            Find result;
            for (const auto& name : names) {
                if (file_match(pattern, name.second)) {
                    auto path = file_path((directory / name.second).string(), false, 3);
                    FileMetadata metadata;
                    int failure = file_metadata(path, metadata);
                    if (failure) {
                        return Result{error(host_error(failure), UINT32_MAX), {}};
                    }

                    if (name.second.size() >= 260) {
                        return Result{error(206, UINT32_MAX), {}};
                    }

                    result.entries.push_back({name.second, metadata});
                }
            }

            if (result.entries.empty()) {
                return Result{error(2, UINT32_MAX), {}};
            }

            find_data(result.entries.front(), uint32_t(a[1]));
            uint32_t handle = next_handle_++;
            finds_.emplace(handle, std::move(result));
            return Result{handle, {}};
        },
        "kernel32.dll!FindFirstFileA");
    control_.host(
        loader_.resolve("kernel32.dll", "ReleaseMutex"), {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            auto found = mutexes_.find(uint32_t(a[0]));
            if (found == mutexes_.end()) {
                return Result{error(6), {}};
            }

            auto& mutex = *found->second;
            if (mutex.owner != thread_id() || !mutex.recursion) {
                return Result{error(288), {}};
            }

            if (!--mutex.recursion) {
                mutex.owner = 0;
                control_.scheduler().changed();
            }

            return Result{1, {}};
        },
        "kernel32.dll!ReleaseMutex");
    control_.host(
        loader_.resolve("kernel32.dll", "WaitForSingleObject"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            uint32_t waited;
            if (wait_thread(uint32_t(a[0]), uint32_t(a[1]), waited)) {
                return Result{waited, {}};
            }

            if (auto found = semaphores_.find(uint32_t(a[0])); found != semaphores_.end()) {
                auto held = found->second;
                if (!held->count && a[1] == UINT32_MAX && threads_.empty()) {
                    throw GuestFault("unsignaled semaphore requires guest scheduling");
                }

                if (!wait_for([&] { return held->count > 0; }, uint32_t(a[1]))) {
                    return Result{258, {}};
                }

                --held->count;
                return Result{0, {}};
            }

            if (auto found = events_.find(uint32_t(a[0])); found != events_.end()) {
                auto held = found->second;
                if (!held->signaled && a[1] == UINT32_MAX && threads_.empty()) {
                    throw GuestFault("unsignaled event requires guest scheduling");
                }

                if (!wait_for([&] { return held->signaled; }, uint32_t(a[1]))) {
                    return Result{258, {}};
                }

                if (!held->manual) {
                    held->signaled = false;
                }

                return Result{0, {}};
            }

            auto found = mutexes_.find(uint32_t(a[0]));
            if (found == mutexes_.end()) {
                if (handles_.count(uint32_t(a[0]))) {
                    throw GuestFault("unqualified wait object type");
                }

                return Result{error(6, UINT32_MAX), {}};
            }

            auto held = found->second;
            auto& mutex = *held;
            uint32_t self = thread_id();
            if (mutex.owner && mutex.owner != self && a[1] && threads_.empty()) {
                throw GuestFault("contended mutex requires guest scheduling");
            }

            if (!wait_for([&] { return !mutex.owner || mutex.owner == self; }, uint32_t(a[1]))) {
                return Result{258, {}};
            }

            mutex.owner = self;
            ++mutex.recursion;
            return Result{0, {}};
        },
        "kernel32.dll!WaitForSingleObject");
    control_.host(
        loader_.resolve("kernel32.dll", "GlobalMemoryStatus"),
        {Convention::Stdcall, ReturnChannel::Void, {{4, PointerDirection::Out}}, ""},
        [](Control& c, const std::vector<uint64_t>& a) {
            const MemoryTotals host = host_memory();
            const uint64_t cap = UINT32_MAX, user = 0x7ffe0000;
            auto clamp = [cap](uint64_t value) {
                return uint32_t(std::min(value, cap));
            };
            const uint64_t used = host.physical_total - host.physical_free;
            const uint32_t fields[8] = {32,
                                        uint32_t(host.physical_total ? used * 100 / host.physical_total : 0),
                                        clamp(host.physical_total),
                                        clamp(host.physical_free),
                                        clamp(host.physical_total + host.swap_total),
                                        clamp(host.physical_free + host.swap_free),
                                        uint32_t(user),
                                        uint32_t(user - std::min(user, c.memory.reserved_below(uint32_t(user))))};
            c.memory.write(uint32_t(a[0]), fields, sizeof fields);
            return Result{};
        },
        "kernel32.dll!GlobalMemoryStatus");
    control_.host(
        loader_.resolve("kernel32.dll", "WriteFile"),
        {Convention::Stdcall,
         ReturnChannel::Eax,
         {{4}, {4, PointerDirection::In}, {4}, {4, PointerDirection::Out}, {4}},
         ""},
        [this](Control& current, const std::vector<uint64_t>& a) {
            if (a[4]) {
                throw GuestFault("unobserved overlapped file write");
            }

            if (a[3]) {
                current.memory.store32(uint32_t(a[3]), 0);
            }

            auto handle = handles_.find(uint32_t(a[0]));
            if (handle == handles_.end() || handle->second.fd < 0) {
                return Result{error(6), {}};
            }

            current.memory.check(uint32_t(a[1]), uint32_t(a[2]), Read);
            std::vector<uint8_t> bytes(a[2]);
            current.memory.read(uint32_t(a[1]), bytes.data(), bytes.size());
            ssize_t count = write(handle->second.fd, bytes.data(), bytes.size());
            // A GENERIC_READ-only handle is O_RDONLY; Windows reports that write as ERROR_ACCESS_DENIED, not an invalid
            // handle.
            if (count < 0) {
                return Result{error(errno == EBADF ? 5 : host_error(errno)), {}};
            }

            if (a[3]) {
                current.memory.store32(uint32_t(a[3]), uint32_t(count));
            }

            return Result{1, {}};
        },
        "kernel32.dll!WriteFile");
    control_.host(
        loader_.resolve("kernel32.dll", "GetFileSize"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4, PointerDirection::Out}}, ""},
        [this](Control& current, const std::vector<uint64_t>& a) {
            auto handle = handles_.find(uint32_t(a[0]));
            struct stat info{};
            if (handle == handles_.end() || handle->second.fd < 0) {
                error(6);
                return Result{UINT32_MAX, {}};
            }

            if (fstat(handle->second.fd, &info)) {
                error(host_error(errno));
                return Result{UINT32_MAX, {}};
            }

            if (a[1]) {
                current.memory.store32(uint32_t(a[1]), uint32_t(uint64_t(info.st_size) >> 32));
            }

            return Result{uint32_t(info.st_size), {}};
        },
        "kernel32.dll!GetFileSize");
    control_.host(
        loader_.resolve("kernel32.dll", "CreateFileMappingA"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}, {4}, {4}, {4}, {4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            if (a[1] || a[5] || a[2] != 2) {
                throw GuestFault("unqualified file-mapping request");
            }

            auto handle = handles_.find(uint32_t(a[0]));
            struct stat info{};
            if (handle == handles_.end() || handle->second.fd < 0) {
                return Result{error(6), {}};
            }

            if (fstat(handle->second.fd, &info)) {
                return Result{error(host_error(errno)), {}};
            }

            const uint64_t requested = a[3] << 32 | a[4], size = requested ? requested : uint64_t(info.st_size);
            if (!size || size > uint64_t(info.st_size)) {
                return Result{error(8), {}};
            }

            const int fd = dup(handle->second.fd);
            if (fd < 0) {
                return Result{error(host_error(errno)), {}};
            }

            const uint32_t mapping = next_handle_++;
            handles_.emplace(mapping, Handle{fd, 0, {}});
            mappings_.emplace(mapping, size);
            return Result{mapping, {}};
        },
        "kernel32.dll!CreateFileMappingA");
    control_.host(
        loader_.resolve("kernel32.dll", "MapViewOfFile"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}, {4}, {4}, {4}}, ""},
        [this](Control& current, const std::vector<uint64_t>& a) {
            if (a[1] != 4) {
                throw GuestFault("unqualified file-view access");
            }

            auto mapping = mappings_.find(uint32_t(a[0]));
            if (mapping == mappings_.end()) {
                return Result{error(6), {}};
            }

            const uint64_t offset = a[2] << 32 | a[3];
            if (offset % 0x10000 || offset >= mapping->second) {
                return Result{error(87), {}};
            }

            const uint64_t length = a[4] ? uint64_t(a[4]) : mapping->second - offset;
            if (length > mapping->second - offset) {
                return Result{error(87), {}};
            }

            std::vector<uint8_t> bytes(length);
            const int fd = handles_.at(uint32_t(a[0])).fd;
            for (size_t done = 0; done < bytes.size();) {
                const ssize_t count = pread(fd, bytes.data() + done, bytes.size() - done, off_t(offset + done));
                if (count <= 0) {
                    return Result{error(count < 0 ? host_error(errno) : 38), {}};
                }

                done += size_t(count);
            }

            const uint32_t view = current.heap.allocate(uint32_t(length));
            current.memory.write(view, bytes.data(), bytes.size());
            views_.emplace(view, uint32_t(length));
            if (environment_.qa_marker) {
                uint64_t live = 0;
                for (const auto& entry : views_) {
                    live += entry.second;
                }

                environment_.qa_marker("QA_MAP view=" + std::to_string(view) + " offset=" + std::to_string(offset) +
                                       " length=" + std::to_string(length) + " mapping=" +
                                       std::to_string(mapping->second) + " live_view_bytes=" + std::to_string(live));
            }

            return Result{view, {}};
        },
        "kernel32.dll!MapViewOfFile");
    control_.host(
        loader_.resolve("kernel32.dll", "UnmapViewOfFile"), {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
        [this](Control& current, const std::vector<uint64_t>& a) {
            if (!views_.erase(uint32_t(a[0]))) {
                return Result{error(487), {}};
            }

            if (environment_.qa_marker) {
                environment_.qa_marker("QA_UNMAP view=" + std::to_string(a[0]));
            }

            current.heap.free(uint32_t(a[0]));
            return Result{1, {}};
        },
        "kernel32.dll!UnmapViewOfFile");
    control_.host(
        loader_.resolve("winmm.dll", "timeEndPeriod"), {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            auto period = timer_periods_.find(uint32_t(a[0]));
            if (period == timer_periods_.end() || !period->second) {
                return Result{97, {}};
            }

            if (!--period->second) {
                timer_periods_.erase(period);
            }

            return Result{0, {}};
        },
        "winmm.dll!timeEndPeriod");
    control_.host(
        loader_.resolve("winmm.dll", "timeGetTime"), {Convention::Stdcall, ReturnChannel::Eax, {}, ""},
        [this](Control&, const std::vector<uint64_t>&) { return Result{uint32_t(counter() / 1000000), {}}; },
        "winmm.dll!timeGetTime");
    control_.host(
        loader_.resolve("kernel32.dll", "MulDiv"), {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}, {4}}, ""},
        [](Control&, const std::vector<uint64_t>& a) {
            const int64_t divisor = int32_t(a[2]);
            if (!divisor) {
                return Result{uint32_t(-1), {}};
            }

            const int64_t product = int64_t(int32_t(a[0])) * int32_t(a[1]);
            const bool negative = (product < 0) != (divisor < 0);
            const uint64_t magnitude = product < 0 ? uint64_t(-product) : uint64_t(product);
            const uint64_t denominator = divisor < 0 ? uint64_t(-divisor) : uint64_t(divisor);
            const uint64_t quotient = (magnitude + denominator / 2) / denominator;
            if (quotient > (negative ? uint64_t(INT32_MAX) + 1 : uint64_t(INT32_MAX))) {
                return Result{uint32_t(-1), {}};
            }

            return Result{uint32_t(negative ? -int64_t(quotient) : int64_t(quotient)), {}};
        },
        "kernel32.dll!MulDiv");
    control_.host(
        loader_.resolve("kernel32.dll", "Sleep"), {Convention::Stdcall, ReturnChannel::Void, {{4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            if (a[0] == UINT32_MAX) {
                throw GuestFault("unqualified infinite Sleep");
            }

            if (environment_.on_sleep) {
                environment_.on_sleep(uint32_t(a[0]));
            }

            if (a[0]) {
                wait_for([] { return false; }, uint32_t(a[0]));
            } else {
                control_.scheduler().yield();
            }

            return Result{};
        },
        "kernel32.dll!Sleep");
    for (const auto& module : loader_.native_modules()) {
        control_.module(module.name, module.base, 0x10000);
    }

    control_.bind_exception_imports(loader_);
    control_.set_clock([this] { return counter(); });
    bind_threads();
    bind_strings();
    bind_virtual();
    bind_locale();
    bind_system();
    for (const auto& d : descriptors) {
        if (d.api == StartupApi::SetUnhandledExceptionFilter) {
            continue; // Has its own body.
        }

        control_.host(
            loader_.resolve(d.dll, d.symbol), signature(d.api),
            [this, api = d.api](Control&, const std::vector<uint64_t>& args) { return call(api, args); },
            std::string(d.dll) + "!" + d.symbol);
    }

    control_.host(
        loader_.resolve("kernel32.dll", "GetFileAttributesA"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4, PointerDirection::In}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            auto path = file_path(ansi(uint32_t(a[0])), false, 3);
            struct stat status{};
            if (stat(path.c_str(), &status)) {
                return Result{error(host_error(errno), UINT32_MAX), {}};
            }

            uint32_t attributes = S_ISDIR(status.st_mode) ? 0x10 : S_ISREG(status.st_mode) ? 0x20 : 0;
            if (!attributes) {
                throw GuestFault("unqualified asset file type");
            }

            if (!(status.st_mode & 0222)) {
                attributes |= 1;
            }

            return Result{attributes, {}};
        },
        "kernel32.dll!GetFileAttributesA");
    control_.host(
        loader_.resolve("kernel32.dll", "DeleteFileA"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4, PointerDirection::In}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            auto name = ansi(uint32_t(a[0]));
            auto path = file_path(name, false, 3);
            struct stat status{};
            if (stat(path.c_str(), &status)) {
                return Result{error(host_error(errno)), {}};
            }

            if (S_ISDIR(status.st_mode) || !(status.st_mode & 0222)) {
                return Result{error(5), {}};
            }

            if (!S_ISREG(status.st_mode)) {
                throw GuestFault("unqualified deletion file type");
            }

            for (const auto& handle : handles_) {
                if (handle.second.fd >= 0) {
                    struct stat opened{};
                    if (fstat(handle.second.fd, &opened)) {
                        throw GuestFault("invalid live file descriptor");
                    }

                    // Qualified CreateFileA share mode is READ|WRITE, not DELETE.
                    if (opened.st_dev == status.st_dev && opened.st_ino == status.st_ino) {
                        return Result{error(32), {}};
                    }
                }
            }

            if (std::filesystem::exists(file_path(name, false, 3, false))) {
                throw GuestFault("deleting an immutable lower asset requires unqualified overlay tombstones");
            }

            if (unlink(path.c_str())) {
                return Result{error(host_error(errno)), {}};
            }

            return Result{1, {}};
        },
        "kernel32.dll!DeleteFileA");
    control_.host(
        loader_.resolve("kernel32.dll", "SetFilePointer"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}, {4, PointerDirection::InOut}, {4}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            auto handle = handles_.find(uint32_t(a[0]));
            if (handle == handles_.end() || handle->second.fd < 0 || handle->second.type != 1) {
                return Result{error(6, UINT32_MAX), {}};
            }

            if (a[3] > 2) {
                return Result{error(87, UINT32_MAX), {}};
            }

            int64_t distance = int32_t(a[1]), base = 0;
            if (a[2]) {
                c.memory.check(uint32_t(a[2]), 4, Read | Write);
                uint64_t bits = uint64_t(c.memory.load32(uint32_t(a[2]))) << 32 | uint32_t(a[1]);
                std::memcpy(&distance, &bits, sizeof distance);
            }

            auto failed = [&] {
                return Result{error(errno == EINVAL ? 87 : errno == ESPIPE ? 1 : host_error(errno), UINT32_MAX), {}};
            };
            if (a[3] == 1) {
                base = lseek(handle->second.fd, 0, SEEK_CUR);
                if (base < 0) {
                    return failed();
                }
            } else if (a[3] == 2) {
                struct stat status{};
                if (fstat(handle->second.fd, &status)) {
                    return failed();
                }

                base = status.st_size;
            }

            if (distance < -base) {
                return Result{error(131, UINT32_MAX), {}};
            }

            if (distance > std::numeric_limits<int64_t>::max() - base) {
                return Result{error(87, UINT32_MAX), {}};
            }

            int64_t position = base + distance;
            if (!a[2] && uint64_t(position) > UINT32_MAX) {
                return Result{error(87, UINT32_MAX), {}};
            }

            if (lseek(handle->second.fd, position, SEEK_SET) < 0) {
                return failed();
            }

            if (a[2]) {
                c.memory.store32(uint32_t(a[2]), uint32_t(uint64_t(position) >> 32));
            }

            uint32_t low = uint32_t(position);
            if (low == UINT32_MAX) {
                error(0);
            }

            return Result{low, {}};
        },
        "kernel32.dll!SetFilePointer");
    // Real TinCat security-cookie attach closure (original RVA 0x389b5).
    control_.host(
        loader_.resolve("kernel32.dll", "GetCurrentProcessId"), {Convention::Stdcall, ReturnChannel::Eax, {}, ""},
        [](Control&, const std::vector<uint64_t>&) { return Result{uint32_t(getpid()), {}}; },
        "kernel32.dll!GetCurrentProcessId");
    control_.host(
        loader_.resolve("kernel32.dll", "QueryPerformanceCounter"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4, PointerDirection::Out}}, ""},
        [this](Control& control, const std::vector<uint64_t>& args) {
            uint64_t counter = this->counter();
            control.memory.check(args[0], 8, Write);
            control.memory.store32(args[0], uint32_t(counter));
            control.memory.store32(args[0] + 4, uint32_t(counter >> 32));
            return Result{1, {}};
        },
        "kernel32.dll!QueryPerformanceCounter");
    control_.host(
        loader_.resolve("kernel32.dll", "QueryPerformanceFrequency"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4, PointerDirection::Out}}, ""},
        [](Control& c, const std::vector<uint64_t>& a) {
            const uint32_t frequency[] = {1000000000, 0};
            c.memory.write(uint32_t(a[0]), frequency, sizeof frequency);
            return Result{1, {}};
        },
        "kernel32.dll!QueryPerformanceFrequency");
    control_.host(
        loader_.resolve("kernel32.dll", "TlsGetValue"), {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            if (a[0] >= tls_count_) {
                return Result{error(87), {}};
            }

            error(0);
            return Result{c.memory.load32(c.cpu.fs_base + 0xe10 + uint32_t(a[0]) * 4), {}};
        },
        "kernel32.dll!TlsGetValue");
    for (const char* symbol : {"GetLocalTime", "GetSystemTime"}) {
        bool local_time = std::string(symbol) == "GetLocalTime";
        control_.host(
            loader_.resolve("kernel32.dll", symbol),
            {Convention::Stdcall, ReturnChannel::Void, {{4, PointerDirection::Out}}, ""},
            [this, local_time](Control& c, const std::vector<uint64_t>& a) {
                uint64_t filetime = file_time();
                int64_t ticks = int64_t(filetime) - 116444736000000000ll;
                time_t seconds = ticks / 10000000;
                tm local{};
                if (!(local_time ? calendar_local(&seconds, &local) : calendar_utc(&seconds, &local))) {
                    throw GuestFault("cannot convert native system time");
                }

                const uint16_t fields[] = {uint16_t(local.tm_year + 1900), uint16_t(local.tm_mon + 1),
                                           uint16_t(local.tm_wday),        uint16_t(local.tm_mday),
                                           uint16_t(local.tm_hour),        uint16_t(local.tm_min),
                                           uint16_t(local.tm_sec),         uint16_t((ticks / 10000) % 1000)};
                uint8_t bytes[16];
                for (unsigned i = 0; i < 8; ++i) {
                    bytes[i * 2] = uint8_t(fields[i]);
                    bytes[i * 2 + 1] = uint8_t(fields[i] >> 8);
                }

                c.memory.write(a[0], bytes, sizeof bytes);
                return Result{};
            },
            std::string("kernel32.dll!") + symbol);
    }

    control_.host(
        loader_.resolve("kernel32.dll", "GetTimeZoneInformation"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4, PointerDirection::Out}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            time_t now = (int64_t(file_time()) - 116444736000000000ll) / 10000000;
            tm current{}, year{};
            if (!calendar_local(&now, &current)) {
                throw GuestFault("cannot query current timezone");
            }

            year.tm_year = current.tm_year;
            year.tm_mday = 1;
            time_t begin = calendar_epoch(&year);
            ++year.tm_year;
            time_t end = calendar_epoch(&year);
            tm previous{};
            if (!calendar_local(&begin, &previous)) {
                throw GuestFault("cannot query timezone year");
            }

            struct Transition {
                time_t utc;
                long before;
                bool daylight;
            };
            std::vector<Transition> transitions;
            long standard = 0, daylight = 0;
            bool have_standard = false, have_daylight = false;
            auto offset = [&](const tm& value) {
                bool dst = value.tm_isdst > 0;
                auto& found = dst ? have_daylight : have_standard;
                auto& result = dst ? daylight : standard;
                if (found && result != calendar_offset(value)) {
                    throw GuestFault("timezone has multiple annual offsets for one state");
                }

                found = true;
                result = calendar_offset(value);
            };
            offset(previous);
            time_t last = begin;
            for (time_t sample = begin + 86400; sample <= end; sample += 86400) {
                tm next{};
                if (!calendar_local(&sample, &next)) {
                    throw GuestFault("cannot query timezone transition");
                }

                offset(next);
                if (calendar_offset(next) != calendar_offset(previous) || next.tm_isdst != previous.tm_isdst) {
                    time_t low = last, high = sample;
                    while (high - low > 1) {
                        time_t middle = low + (high - low) / 2;
                        tm value{};
                        if (!calendar_local(&middle, &value)) {
                            throw GuestFault("cannot locate timezone transition");
                        }

                        if (calendar_offset(value) == calendar_offset(previous) &&
                            value.tm_isdst == previous.tm_isdst) {
                            low = middle;
                        } else {
                            high = middle;
                        }
                    }

                    transitions.push_back({high, calendar_offset(previous), next.tm_isdst > 0});
                }

                previous = next;
                last = sample;
            }

            if (!have_standard || (have_daylight && transitions.size() != 2) ||
                (!have_daylight && !transitions.empty()) || standard % 60 || daylight % 60) {
                throw GuestFault("timezone cannot be represented by the admitted Win32 annual rules");
            }

            std::vector<uint8_t> data(172);
            c.memory.write(a[0], data.data(), data.size());
            c.memory.store32(a[0], uint32_t(-standard / 60));
            if (have_daylight) {
                if (transitions[0].daylight == transitions[1].daylight) {
                    throw GuestFault("timezone lacks paired standard/daylight transitions");
                }

                c.memory.store32(a[0] + 168, uint32_t((standard - daylight) / 60));
                for (const auto& transition : transitions) {
                    // Win32 transition wall time is expressed before the change.
                    time_t wall = transition.utc + transition.before;
                    tm date{};
                    if (!calendar_utc(&wall, &date)) {
                        throw GuestFault("cannot marshal timezone transition");
                    }

                    uint16_t fields[] = {uint16_t(date.tm_year + 1900), uint16_t(date.tm_mon + 1),
                                         uint16_t(date.tm_wday),        uint16_t(date.tm_mday),
                                         uint16_t(date.tm_hour),        uint16_t(date.tm_min),
                                         uint16_t(date.tm_sec),         0};
                    uint8_t bytes[16];
                    for (unsigned i = 0; i < 8; ++i) {
                        bytes[i * 2] = uint8_t(fields[i]);
                        bytes[i * 2 + 1] = uint8_t(fields[i] >> 8);
                    }

                    c.memory.write(a[0] + (transition.daylight ? 152 : 68), bytes, sizeof bytes);
                }
            }

            // Descriptions are empty in the original Wine/IANA oracle profile.
            return Result{have_daylight ? (current.tm_isdst > 0 ? 2u : 1u) : 0u, {}};
        },
        "kernel32.dll!GetTimeZoneInformation");
}

uint64_t Startup::file_time() const {
    return environment_.file_time ? environment_.file_time()
                                  : uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                 std::chrono::system_clock::now().time_since_epoch())
                                                 .count() /
                                             100) +
                                        116444736000000000ull;
}

std::string Startup::ansi(uint32_t pointer, int32_t length) const {
    std::string result;
    if (!pointer || !length || length < -1) {
        throw GuestFault("invalid startup ANSI input");
    }

    for (uint32_t i = 0; i < (length < 0 ? 32768u : uint32_t(length)); ++i) {
        uint8_t c;
        control_.memory.read(pointer + i, &c, 1);
        if (!c && length < 0) {
            return result;
        }

        result += char(c);
    }

    if (length < 0) {
        throw GuestFault("unterminated startup ANSI string");
    }

    return result;
}

std::vector<uint16_t> Startup::wide(uint32_t pointer, int32_t length) const {
    if (!pointer || !length || length < -1) {
        throw GuestFault("invalid startup UTF-16 input");
    }

    std::vector<uint16_t> result;
    for (uint32_t i = 0; i < (length < 0 ? 32768u : uint32_t(length)); ++i) {
        uint8_t b[2];
        control_.memory.read(pointer + i * 2, b, 2);
        uint16_t c = uint16_t(b[0]) | uint16_t(b[1]) << 8;
        result.push_back(c);
        if (!c && length < 0) {
            return result;
        }
    }

    if (length < 0) {
        throw GuestFault("unterminated startup UTF-16 string");
    }

    return result;
}

void Startup::put_ansi(uint32_t pointer, const std::string& value) {
    control_.memory.write(pointer, value.c_str(), value.size() + 1);
}

uint32_t Startup::error(uint32_t code, uint32_t result) {
    control_.memory.store32(active_control().cpu.fs_base + 0x34, code);
    return result;
}

uint32_t Startup::thread_id() const {
    return control_.memory.load32(active_control().cpu.fs_base + 0x24);
}

void Startup::find_data(const FindEntry& entry, uint32_t destination) {
    std::array<uint8_t, 320> data{};
    control_.memory.write(destination, data.data(), data.size());
    control_.memory.store32(destination, entry.metadata.attributes);
    for (unsigned i = 0; i < 3; ++i) {
        control_.memory.store32(destination + 4 + i * 8, uint32_t(entry.metadata.times[i]));
        control_.memory.store32(destination + 8 + i * 8, uint32_t(entry.metadata.times[i] >> 32));
    }

    control_.memory.store32(destination + 28, uint32_t(entry.metadata.size >> 32));
    control_.memory.store32(destination + 32, uint32_t(entry.metadata.size));
    control_.memory.write(destination + 44, entry.name.c_str(), entry.name.size() + 1);
}

uint32_t Startup::allocate(uint32_t heap, uint32_t flags, uint32_t size) {
    if (flags & ~8u) {
        throw GuestFault("unobserved heap allocation flags");
    }

    if (heap && !heaps_.count(heap)) {
        return error(6);
    }

    if (heap && heaps_.at(heap) && size > heaps_.at(heap)) {
        return error(8);
    }

    uint32_t pointer = control_.heap.allocate_small(std::max(size, 1u));
    blocks_.emplace(pointer, Block{heap, size});
    return pointer;
}

uint32_t Startup::release(uint32_t heap, uint32_t pointer) {
    if (!pointer) {
        return 1;
    }

    auto found = blocks_.find(pointer);
    if (found == blocks_.end() || found->second.heap != heap) {
        return error(6);
    }

    control_.heap.free(pointer);
    blocks_.erase(found);
    return 1;
}

void Startup::publish_critical(uint32_t address, const Critical& s) {
    uint32_t values[] = {0, s.recursion ? s.recursion - 1 : UINT32_MAX, s.recursion, s.owner, 0, s.spin};
    control_.memory.write(address, values, sizeof values);
}

std::filesystem::path Startup::file_path(const std::string& input, bool writing, uint32_t disposition, bool overlay,
                                         bool want_file) {
    std::string name = input;
    std::replace(name.begin(), name.end(), '\\', '/');
    if (name.size() >= 2 && name[1] == ':') {
        if (name[0] != 'Z' && name[0] != 'z') {
            throw GuestFault("unqualified file drive: " + name);
        }

        name.erase(0, 2);
    }

    auto root = std::filesystem::canonical(environment_.assets);
    auto relative = std::filesystem::path(name).lexically_normal();
    auto lower = [](std::string text) {
        for (char& c : text) {
            if (c >= 'A' && c <= 'Z') {
                c += 'a' - 'A';
            }
        }

        return text;
    };
    if (relative.is_absolute()) {
        auto part = relative.begin();
        for (const auto& component : root) {
            if (part == relative.end() || lower(part->string()) != lower(component.string())) {
                throw GuestFault("file path escapes installation/profile");
            }

            ++part;
        }

        std::filesystem::path tail;
        for (; part != relative.end(); ++part) {
            tail /= *part;
        }

        relative = tail;
    }

    if (relative.empty() || *relative.begin() == "..") {
        throw GuestFault("file path escapes installation/profile");
    }

    auto resolve = [&](const std::filesystem::path& base) {
        auto path = base;
        for (const auto& part : relative) {
            if (std::filesystem::is_directory(path)) {
                std::filesystem::path match;
                for (const auto& entry : std::filesystem::directory_iterator(path)) {
                    if (lower(entry.path().filename().string()) == lower(part.string())) {
                        if (!match.empty()) {
                            throw GuestFault("case-colliding file path");
                        }

                        match = entry.path();
                    }
                }

                path = match.empty() ? path / part : match;
            } else {
                path /= part;
            }
        }

        auto canonical = std::filesystem::weakly_canonical(path);
        auto inside = canonical.lexically_relative(std::filesystem::canonical(base));
        if (inside.empty() || *inside.begin() == "..") {
            throw GuestFault("file symlink escapes root");
        }

        return path;
    };
    auto asset = resolve(root);
    if (environment_.profile.empty() || !overlay) {
        if (writing) {
            throw GuestFault("file writes require private profile");
        }

        return asset;
    }

    auto profile = resolve(std::filesystem::canonical(environment_.profile));
    if (!writing) {
        if (!std::filesystem::exists(profile)) {
            return asset;
        }

        if (want_file && std::filesystem::is_directory(profile) && std::filesystem::is_regular_file(asset)) {
            return asset;
        }

        return profile;
    }

    std::filesystem::create_directories(profile.parent_path());
    if (!std::filesystem::exists(profile) && std::filesystem::is_regular_file(asset) && disposition != 1 &&
        disposition != 2) {
        std::filesystem::copy_file(asset, profile);
    }

    return profile;
}

Result Startup::call(StartupApi api, const std::vector<uint64_t>& args) {
    if (args.size() != descriptor(api).count) {
        throw GuestFault("startup ABI argument count");
    }

    std::vector<uint32_t> a(args.begin(), args.end());
    auto& m = control_.memory;
    uint32_t value = 0;
    switch (api) {
    case StartupApi::GetLastError:
        value = m.load32(active_control().cpu.fs_base + 0x34);
        break;
    case StartupApi::SetLastError:
        error(a[0]);
        break;
    case StartupApi::GetCurrentThreadId:
        value = thread_id();
        break;
    case StartupApi::GetVersion:
        value = 0x1db10106;
        break; // Observed Windows 6.1/7601 profile.
    case StartupApi::GetVersionExA: {
        uint32_t size = m.load32(a[0]);
        if (size != 148 && size != 156) {
            value = error(87);
            break;
        }

        std::vector<uint8_t> data(size);
        m.write(a[0], data.data(), data.size());
        const uint32_t fields[] = {size, 6, 1, 7601, 2};
        m.write(a[0], fields, sizeof fields);
        put_ansi(a[0] + 20, "Service Pack 1");
        if (size == 156) {
            const uint8_t extra[] = {1, 0, 0, 0, 0, 1, 1, 0};
            m.write(a[0] + 148, extra, 8);
        }

        value = 1;
        break;
    }
    case StartupApi::GetACP:
        value = 1252;
        break;
    case StartupApi::GetCPInfo: {
        if (a[0] != 0 && a[0] != 1252) {
            if (a[0] != UINT32_MAX) {
                throw GuestFault("unqualified startup codepage");
            }

            value = error(87);
            break;
        }

        uint8_t data[20] = {1, 0, 0, 0, '?', 0};
        m.write(a[1], data, sizeof data);
        value = 1;
        break;
    }
    case StartupApi::GetCommandLineA:
        value = command_line_;
        break;
    case StartupApi::GetEnvironmentStringsW: {
        std::vector<uint16_t> data;
        for (const auto& item : environment_.environment) {
            for (uint8_t c : item) {
                data.push_back(decode(c));
            }

            data.push_back(0);
        }

        if (data.empty()) {
            data.push_back(0);
        }

        data.push_back(0);
        value = control_.heap.allocate(uint32_t(data.size() * 2));
        m.write(value, data.data(), data.size() * 2);
        environments_.insert(value);
        break;
    }
    case StartupApi::FreeEnvironmentStringsW:
        if (!environments_.erase(a[0])) {
            value = error(87);
            break;
        }

        control_.heap.free(a[0]);
        value = 1;
        break;
    case StartupApi::GetStdHandle:
        value = a[0] >= 0xfffffff4 && a[0] <= 0xfffffff6 ? 0xe0000000 + (0xfffffff6 - a[0]) : error(87, UINT32_MAX);
        if (value != UINT32_MAX && !handles_.count(value)) {
            value = 0;
        }

        break;
    case StartupApi::GetStartupInfoA: {
        uint8_t data[68] = {};
        m.write(a[0], data, sizeof data);
        m.store32(a[0], 68);
        for (unsigned i = 0; i < 3; ++i) {
            m.store32(a[0] + 56 + i * 4, handles_.count(0xe0000000 + i) ? 0xe0000000 + i : 0);
        }

        break;
    }
    case StartupApi::GetModuleHandleA:
    case StartupApi::LoadLibraryA:
        if (api == StartupApi::LoadLibraryA && !a[0]) {
            value = error(87);
            break;
        }

        value = loader_.module_handle(a[0] ? basename(ansi(a[0])) : "");
        if (!value) {
            error(126);
        }

        break;
    case StartupApi::GetProcAddress: {
        std::string module = loader_.module_name(a[0]);
        if (module.empty()) {
            value = error(126);
            break;
        }

        std::string name = a[1] <= 0xffff ? "#" + std::to_string(a[1]) : ansi(a[1]);
        // Absence is a deliberate Win32 lookup failure; a present, unimplemented
        // export still returns its token and traps on dispatch.
        for (const auto& native : loader_.native_modules()) {
            if (native.name == module) {
                auto found = native.exports.find(name);
                value = found == native.exports.end() ? error(127) : found->second;
                return Result{value, {}};
            }
        }

        try {
            value = loader_.resolve(module, name);
        } catch (const GuestFault&) {
            value = error(127);
        }

        break;
    }
    case StartupApi::GetModuleFileNameA:
    case StartupApi::GetWindowsDirectoryA:
    case StartupApi::GetSystemDirectoryA: {
        std::string text;
        uint32_t pointer, capacity;
        if (api == StartupApi::GetModuleFileNameA) {
            auto name = loader_.module_name(a[0]);
            if (name.empty()) {
                value = error(126);
                break;
            }

            if (name == "sacred.exe") {
                name = "Sacred.exe";
            }

            std::string directory = std::filesystem::absolute(environment_.assets).string();
            std::replace(directory.begin(), directory.end(), '/', '\\');
            text = a[0] >= layout::hle ? "C:\\windows\\system32\\" + name : "Z:" + directory + "\\" + name;
            pointer = a[1];
            capacity = a[2];
        } else {
            text = api == StartupApi::GetWindowsDirectoryA ? "C:\\windows" : "C:\\windows\\system32";
            pointer = a[0];
            capacity = a[1];
        }

        if (capacity <= text.size()) {
            if (api == StartupApi::GetModuleFileNameA) {
                if (capacity) {
                    m.write(pointer, text.data(), capacity - 1);
                    uint8_t zero = 0;
                    m.write(pointer + capacity - 1, &zero, 1);
                }

                value = error(122, capacity);
            } else {
                value = uint32_t(text.size()) + 1;
            }
        } else {
            put_ansi(pointer, text);
            value = uint32_t(text.size());
        }

        break;
    }
    case StartupApi::HeapCreate:
        if (a[0] || (a[2] && a[1] > a[2])) {
            throw GuestFault("unobserved HeapCreate configuration");
        }

        value = next_handle_++;
        heaps_.emplace(value, a[2]);
        break;
    case StartupApi::HeapAlloc:
        value = heaps_.count(a[0]) ? allocate(a[0], a[1], a[2]) : error(6);
        break;
    case StartupApi::Malloc:
    case StartupApi::New:
        value = allocate(0, 0, a[0]);
        break;
    case StartupApi::Delete:
        if (!release(0, a[0])) {
            throw GuestFault("invalid CRT delete");
        }

        break;
    case StartupApi::HeapFree:
        if (a[1]) {
            throw GuestFault("unobserved HeapFree flags");
        }

        value = heaps_.count(a[0]) ? release(a[0], a[2]) : error(6);
        break;
    case StartupApi::HeapSize:
    case StartupApi::HeapReAlloc: {
        auto old = blocks_.find(a[2]);
        if (!heaps_.count(a[0]) || old == blocks_.end() || old->second.heap != a[0]) {
            value = error(6, api == StartupApi::HeapSize ? UINT32_MAX : 0);
            break;
        }

        if (api == StartupApi::HeapSize) {
            value = old->second.size;
            break;
        }

        if (a[1] & ~8u) {
            throw GuestFault("unobserved HeapReAlloc flags");
        }

        value = allocate(a[0], a[1], a[3]);
        if (value) {
            std::vector<uint8_t> bytes(std::min(old->second.size, a[3]));
            m.read(a[2], bytes.data(), bytes.size());
            m.write(value, bytes.data(), bytes.size());
            release(a[0], a[2]);
        }

        break;
    }
    case StartupApi::TlsAlloc:
        value = tls_count_ < 64 ? tls_count_++ : error(259, UINT32_MAX);
        break;
    case StartupApi::TlsSetValue:
        if (a[0] >= tls_count_) {
            value = error(87);
            break;
        }

        m.store32(active_control().cpu.fs_base + 0xe10 + a[0] * 4, a[1]);
        value = 1;
        break;
    case StartupApi::FlsAlloc:
        value = uint32_t(fls_callbacks_.size());
        if (value == 128) {
            value = error(259, UINT32_MAX);
            break;
        }

        fls_callbacks_.push_back(a[0]);
        break;
    case StartupApi::FlsGetValue:
    case StartupApi::FlsSetValue:
        if (a[0] >= fls_callbacks_.size()) {
            value = error(87);
            break;
        }

        if (api == StartupApi::FlsSetValue) {
            fls_values_[{active_control().cpu.fs_base, a[0]}] = a[1];
            value = 1;
        } else {
            value = fls_values_[{active_control().cpu.fs_base, a[0]}];
            error(0);
        }

        break;
    case StartupApi::InitializeCriticalSection:
    case StartupApi::InitializeCriticalSectionAndSpinCount: {
        m.check(a[0], 24, Write);
        Critical state{0, 0, a.size() == 2 ? a[1] : 0};
        if (!critical_.emplace(a[0], state).second) {
            throw GuestFault("critical section already initialized");
        }

        publish_critical(a[0], state);
        value = 1;
        break;
    }
    case StartupApi::EnterCriticalSection:
    case StartupApi::LeaveCriticalSection: {
        auto found = critical_.find(a[0]);
        if (found == critical_.end()) {
            throw GuestFault("uninitialized critical section");
        }

        auto& s = found->second;
        if (api == StartupApi::EnterCriticalSection) {
            const uint32_t self = thread_id();
            if (s.recursion && s.owner != self) {
                if (threads_.empty()) {
                    throw GuestFault("contended startup critical section");
                }

                wait_for([&] { return !s.recursion || s.owner == self; }, UINT32_MAX);
            }

            s.owner = self;
            ++s.recursion;
        } else {
            if (!s.recursion || s.owner != thread_id()) {
                throw GuestFault("foreign critical section leave");
            }

            if (!--s.recursion) {
                s.owner = 0;
                control_.scheduler().changed();
            }
        }

        publish_critical(a[0], s);
        break;
    }
    case StartupApi::InterlockedExchange:
        value = m.load32(a[0]);
        m.store32(a[0], a[1]);
        break; // Serialized guest execution.
    case StartupApi::IsProcessorFeaturePresent:
        // 0 is PF_FLOATING_POINT_PRECISION_ERRATA (observed false). 3/6/7/10 are MMX, SSE, 3DNow and SSE2 and
        // follow the frozen CPUID profile in runtime/modules/machine.cpp: leaf 1 EDX bits 23/25/26 and
        // extended leaf 0x80000001 EDX bit 31 are all set.
        if (a[0] != 0 && a[0] != 3 && a[0] != 6 && a[0] != 7 && a[0] != 10) {
            throw GuestFault("unobserved processor feature query");
        }

        value = a[0] != 0;
        break;
    case StartupApi::GetSystemTimeAsFileTime: {
        uint64_t time = file_time();
        m.store32(a[0], uint32_t(time));
        m.store32(a[0] + 4, uint32_t(time >> 32));
        break;
    }
    case StartupApi::GetTickCount:
        value = environment_.tick_count ? environment_.tick_count() : uint32_t(counter() / 1000000);
        break;
    case StartupApi::SetHandleCount:
        value = a[0];
        break; // Win32 compatibility API; no host fd limit change.
    case StartupApi::FlushInstructionCache:
        // Tokens and original code bytes are never host executable; no instruction cache aliases exist.
        m.check(a[1], a[2], Read);
        value = 1;
        break;
    case StartupApi::CreateFileA: {
        // Share mode 1 (read) and attributes 0 come from the LAN init's read-only size probe of the exe
        // (0x007d2ddf); in one process they open exactly like share mode 3 and FILE_ATTRIBUTE_NORMAL.
        if ((a[1] & ~0xc0000000u) || !a[1] || (a[2] != 3 && a[2] != 1) || a[4] < 1 || a[4] > 5 ||
            (a[5] != 0x80 && a[5] != 0) || a[6]) {
            throw GuestFault("unqualified file-open mode");
        }

        if (a[3] && (m.load32(a[3]) != 12 || m.load32(a[3] + 4))) {
            throw GuestFault("unqualified file security descriptor");
        }

        const bool write_access = a[1] & 0x40000000;
        // Win32: TRUNCATE_EXISTING requires GENERIC_WRITE (ERROR_INVALID_PARAMETER otherwise, as on current Windows).
        if (a[4] == 5 && !write_access) {
            value = error(87, UINT32_MAX);
            break;
        }

        // CREATE_NEW/CREATE_ALWAYS and any write access resolve into the profile overlay. OPEN_ALWAYS on an existing
        // file just opens it, so it only overlays when it must create the file or the handle can write.
        bool writing = write_access || a[4] == 1 || a[4] == 2;
        if (a[4] == 4 && !writing) {
            writing = !std::filesystem::exists(file_path(ansi(a[0]), false, a[4], true, true));
        }

        auto path = file_path(ansi(a[0]), writing, a[4], true, true);
        bool existed = std::filesystem::exists(path);
        int flags = write_access ? (a[1] & 0x80000000 ? O_RDWR : O_WRONLY) : O_RDONLY;
        switch (a[4]) {
        case 1:
            flags |= O_CREAT | O_EXCL;
            break;
        case 2:
            flags |= O_CREAT | O_TRUNC;
            break;
        case 3:
            break;
        case 4:
            flags |= O_CREAT;
            break;
        case 5:
            flags |= O_TRUNC;
            break;
        }

        int fd = open(path.c_str(), flags | O_CLOEXEC, 0600);
        const int open_errno = errno;
        struct stat opened{};
        const bool directory = fd >= 0 && !fstat(fd, &opened) && S_ISDIR(opened.st_mode);
        if (qa_env("SACRED_QA_IO_LOG")) {
            std::fprintf(stderr, "QA_IO CreateFileA guest=%s host=%s access=%08x disposition=%u -> %s\n",
                         ansi(a[0]).c_str(), path.c_str(), a[1], a[4],
                         fd < 0      ? std::strerror(open_errno)
                         : directory ? "directory refused"
                                     : "ok");
        }

        if (fd < 0) {
            value = error(host_error(open_errno), UINT32_MAX);
            break;
        }

        if (directory) {
            // CreateFileA without FILE_FLAG_BACKUP_SEMANTICS never opens a directory; open(2) O_RDONLY does.
            close(fd);
            value = error(5, UINT32_MAX);
            break;
        }

        if (a[4] == 2 || a[4] == 4) {
            error(existed ? 183 : 0);
        }

        if (environment_.qa_marker) {
            environment_.qa_marker("QA_FILE_OPEN " + path.filename().string());
        }

        value = next_handle_++;
        handles_.emplace(value, Handle{fd, 1, {}});
        break;
    }
    case StartupApi::ReadFile: {
        if (a[4]) {
            throw GuestFault("unobserved overlapped startup IO");
        }

        m.store32(a[3], 0);
        auto h = handles_.find(a[0]);
        if (h == handles_.end() || h->second.fd < 0) {
            value = error(6);
            break;
        }

        m.check(a[1], a[2], Write);
        std::vector<uint8_t> bytes(a[2]);
        ssize_t count = read(h->second.fd, bytes.data(), bytes.size());
        if (count < 0) {
            value = error(host_error(errno));
            break;
        }

        m.write(a[1], bytes.data(), size_t(count));
        m.store32(a[3], uint32_t(count));
        value = 1;
        break;
    }
    case StartupApi::GetFileType: {
        auto h = handles_.find(a[0]);
        value = h == handles_.end() || !h->second.type ? error(6) : h->second.type;
        break;
    }
    case StartupApi::CloseHandle: {
        if (thread_handles_.erase(uint32_t(a[0]))) {
            value = 1;
            break;
        }

        auto h = handles_.find(a[0]);
        if (h == handles_.end()) {
            value = error(6);
            break;
        }

        if (h->second.fd >= 0 && close(h->second.fd) < 0) {
            value = error(host_error(errno));
            break;
        }

        mutexes_.erase(uint32_t(a[0]));
        semaphores_.erase(uint32_t(a[0]));
        events_.erase(uint32_t(a[0]));
        mappings_.erase(uint32_t(a[0]));
        handles_.erase(h);
        value = 1;
        break;
    }
    case StartupApi::CreateMutexA: {
        if (a[0]) {
            throw GuestFault("unobserved mutex security attributes");
        }

        std::string name = a[2] ? ansi(a[2]) : "";
        int fd = -1;
        auto mutex = std::make_shared<Mutex>();
        bool exists = false;
        if (!name.empty()) {
            for (const auto& h : handles_) {
                if (h.second.name == name) {
                    fd = dup(h.second.fd);
                    if (fd < 0) {
                        throw GuestFault("cannot duplicate named mutex lifetime handle");
                    }

                    mutex = mutexes_.at(h.first);
                    exists = true;
                    break;
                }
            }

            if (!exists) {
                if (environment_.profile.empty()) {
                    throw GuestFault("named mutex requires private profile");
                }

                auto directory = std::filesystem::path(environment_.profile) / "mutexes";
                std::filesystem::create_directories(directory);
                std::vector<uint8_t> bytes(name.begin(), name.end());
                auto path = directory / sha256(bytes);
                fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
                if (fd < 0) {
                    throw GuestFault("cannot open named mutex lifetime handle");
                }

                if (flock(fd, LOCK_EX | LOCK_NB)) {
                    close(fd);
                    // Another native process owns this object. Do not invent an
                    // independently owned mutex or silently admit a second game.
                    throw GuestFault("external named mutex requires shared ownership adapter");
                }
            }
        }

        if (!exists && a[1]) {
            mutex->owner = thread_id();
            mutex->recursion = 1;
        }

        value = next_handle_++;
        handles_.emplace(value, Handle{fd, 0, name});
        mutexes_.emplace(value, std::move(mutex));
        error(exists ? 183 : 0);
        break;
    }
    case StartupApi::TimeBeginPeriod: {
        timespec resolution{};
        if (clock_getres(CLOCK_MONOTONIC, &resolution)) {
            throw GuestFault("cannot query native timer resolution");
        }

        if (a[0] < 1 || a[0] > 65535 || resolution.tv_sec || uint64_t(resolution.tv_nsec) > uint64_t(a[0]) * 1000000) {
            value = 97;
        } else {
            ++timer_periods_[a[0]]; // Native clock already provides the requested resolution.
        }

        break;
    }
    case StartupApi::InitTerm:
        if (a[1] < a[0] || (a[1] - a[0]) % 4) {
            throw GuestFault("invalid initializer table");
        }

        for (uint32_t p = a[0]; p != a[1]; p += 4) {
            if (uint32_t callback = m.load32(p)) {
                active_control().invoke(callback, {Convention::Cdecl, ReturnChannel::Void, {}, ""}, {});
                if (active_control().transferring()) {
                    break;
                }
            }
        }

        break;
    case StartupApi::DllOnExit: {
        uint32_t begin = m.load32(a[1]), end = m.load32(a[2]);
        auto b = blocks_.find(begin);
        if (b == blocks_.end() || b->second.heap || end < begin || end - begin > b->second.size || (end - begin) % 4) {
            throw GuestFault("invalid DLL onexit table");
        }

        uint32_t size = end - begin;
        uint32_t replacement = allocate(0, 0, size + 4);
        std::vector<uint8_t> data(size);
        m.read(begin, data.data(), size);
        m.write(replacement, data.data(), size);
        m.store32(replacement + size, a[0]);
        release(0, begin);
        m.store32(a[1], replacement);
        m.store32(a[2], replacement + size + 4);
        value = a[0];
        break;
    }
    case StartupApi::SplitPath: {
        std::string path = ansi(a[0]), drive, dir, file, ext;
        if (path.size() >= 2 && path[1] == ':') {
            drive = path.substr(0, 2);
            path.erase(0, 2);
        }

        auto slash = path.find_last_of("\\/");
        if (slash != std::string::npos) {
            dir = path.substr(0, slash + 1);
            path.erase(0, slash + 1);
        }

        auto dot = path.find_last_of('.');
        if (dot != std::string::npos) {
            ext = path.substr(dot);
            path.resize(dot);
        }

        file = path;
        const std::string parts[] = {drive, dir, file, ext};
        for (unsigned i = 0; i < 4; ++i) {
            if (a[i + 1]) {
                put_ansi(a[i + 1], parts[i]);
            }
        }

        break;
    }
    case StartupApi::MultiByteToWideChar: {
        if (a[0] != 0 && a[0] != 1252) {
            if (a[0] != UINT32_MAX) {
                throw GuestFault("unqualified startup codepage");
            }

            value = error(87);
            break;
        }

        if (a[1] & ~1u) {
            throw GuestFault("unqualified startup conversion flags");
        }

        if (!a[2] || !a[3] || int32_t(a[3]) < -1 || int32_t(a[5]) < 0) {
            value = error(87);
            break;
        }

        auto text = ansi(a[2], int32_t(a[3]));
        if (a[3] == UINT32_MAX) {
            text += '\0';
        }

        std::vector<uint16_t> output;
        for (uint8_t c : text) {
            output.push_back(decode(c));
        }

        if (a[5] && a[5] < output.size()) {
            value = error(122);
            break;
        }

        if (a[5]) {
            m.write(a[4], output.data(), output.size() * 2);
        }

        value = uint32_t(output.size());
        break;
    }
    case StartupApi::WideCharToMultiByte: {
        if (a[0] != 0 && a[0] != 1252) {
            if (a[0] != UINT32_MAX) {
                throw GuestFault("unqualified startup codepage");
            }

            value = error(87);
            break;
        }

        // The CRT also uses WC_COMPOSITECHECK | WC_SEPCHARS. In the qualified
        // CP1252/U+0191 domain there are no combining characters to compose.
        if ((a[1] & ~0x400u) && a[1] != 0x220) {
            throw GuestFault("unqualified startup conversion flags");
        }

        if (!a[2] || !a[3] || int32_t(a[3]) < -1 || int32_t(a[5]) < 0) {
            value = error(87);
            break;
        }

        auto text = wide(a[2], int32_t(a[3]));
        bool used = false;
        uint8_t fallback = '?';
        if (a[6]) {
            m.read(a[6], &fallback, 1);
        }

        std::vector<uint8_t> output;
        for (uint16_t c : text) {
            // Reached by TinCat's uppercase table; exact original API probe
            // records best-fit 0x83, or the caller's default with WC_NO_BEST_FIT_CHARS.
            if (c == 0x0191) {
                output.push_back(a[1] & 0x400 ? fallback : 0x83);
                used |= (a[1] & 0x400) != 0;
            } else {
                bool outside = false;
                output.push_back(encode(c, outside, fallback));
                if (outside) {
                    throw GuestFault("unqualified non-CP1252 startup conversion input");
                }
            }
        }

        if (a[5] && a[5] < output.size()) {
            value = error(122);
            break;
        }

        if (a[5]) {
            m.write(a[4], output.data(), output.size());
        }

        if (a[7]) {
            m.store32(a[7], used);
        }

        value = uint32_t(output.size());
        break;
    }
    case StartupApi::LCMapStringW:
    case StartupApi::GetStringTypeW: {
        bool mapping = api == StartupApi::LCMapStringW;
        if ((mapping && ((a[0] != 0 && a[0] != 0x409) || (a[1] != 0x100 && a[1] != 0x200))) ||
            (!mapping && a[0] != 1)) {
            throw GuestFault("unobserved startup locale operation");
        }

        uint32_t source = a[mapping ? 2 : 1], count = a[mapping ? 3 : 2];
        if (!source || !count || int32_t(count) < -1) {
            value = error(87);
            break;
        }

        auto text = wide(source, int32_t(count));
        std::vector<uint16_t> output;
        UnicodeLocale locale;
        for (uint16_t c : text) {
            bool outside = false;
            encode(c, outside, '?');
            if (outside) {
                throw GuestFault("unqualified non-CP1252 startup locale input");
            }

            if (mapping) {
                output.push_back(c == 0xb5 && a[1] == 0x200 ? c : locale.map(c, a[1] == 0x100));
            } else {
                uint16_t flags = locale.properties(c);
                // Windows C1 categories differ from POSIX for these CP1252
                // characters; qualified against all 256 original Wine entries.
                if (c == 0x20ac || c == 0x2dc || c == 0x2122) {
                    flags &= ~16u;
                }

                if (c == 0xa0) {
                    flags = (flags & ~16u) | 8u;
                }

                if (c == 0xaa || c == 0xb5 || c == 0xba) {
                    flags |= 16;
                }

                if (c == 0xad) {
                    flags |= 32;
                }

                if (c == 0xb2 || c == 0xb3 || c == 0xb9) {
                    flags |= 4;
                }

                output.push_back(flags);
            }
        }

        if (mapping && a[5] && a[5] < output.size()) {
            value = error(122);
            break;
        }

        if (!mapping || a[5]) {
            m.write(a[mapping ? 4 : 3], output.data(), output.size() * 2);
        }

        value = mapping ? uint32_t(output.size()) : 1;
        break;
    }
    case StartupApi::VirtualQuery: {
        if (a[2] < 28) {
            value = error(24);
            break;
        }

        uint32_t base = 0, size = 0;
        for (const auto& image : loader_.modules()) {
            if (a[0] >= image.base && uint64_t(a[0]) < uint64_t(image.base) + image.image_size) {
                base = image.base;
                size = image.image_size;
            }
        }

        for (const auto& image : loader_.native_modules()) {
            if (a[0] >= image.base && a[0] < image.base + 0x10000) {
                base = image.base;
                size = 0x10000;
            }
        }

        if (!size) {
            throw GuestFault("unobserved VirtualQuery region");
        }

        uint32_t page = a[0] & ~4095u, start = page, end = page + 4096;
        uint8_t permissions = m.permissions(page);
        while (start > base && m.permissions(start - 4096) == permissions) {
            start -= 4096;
        }

        while (end < base + size && m.permissions(end) == permissions) {
            end += 4096;
        }

        uint32_t protection = permissions & Execute ? (permissions & Write ? 0x40 : 0x20)
                              : permissions & Write ? 4
                              : permissions & Read  ? 2
                                                    : 1;
        uint32_t info[] = {
            start, base, 0x80, end - start, permissions ? 0x1000u : 0x2000u, permissions ? protection : 0, 0x1000000};
        m.write(a[1], info, sizeof info);
        value = sizeof info;
        break;
    }
    case StartupApi::SetUnhandledExceptionFilter:
        value = control_.unhandled_filter;
        control_.unhandled_filter = a[0];
        break;
    }

    return Result{value, {}};
}
}
