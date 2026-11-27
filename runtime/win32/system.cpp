#include "startup.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>

namespace sacred {
namespace {
constexpr uint32_t error_file_not_found = 2, error_path_not_found = 3, error_access_denied = 5,
                   error_invalid_handle = 6, error_not_enough_memory = 8, error_invalid_parameter = 87,
                   error_file_exists = 80, error_sharing_violation = 32, error_already_exists = 183,
                   error_env_not_found = 203, error_too_many_posts = 298, error_invalid_address = 487;
constexpr uint32_t mem_commit_reserve = 0x3000, mem_release = 0x8000, page_readwrite = 4;
constexpr uint32_t attribute_readonly = 1, attribute_archive = 0x20, attribute_normal = 0x80;

std::string lower(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c += 'a' - 'A';
        }
    }

    return text;
}

uint32_t errno_to_win32(int value) {
    if (value == EEXIST) {
        return error_already_exists;
    }

    if (auto code = win32_error_from_errno(value)) {
        return *code;
    }

    throw GuestFault("unmapped system host error " + std::to_string(value));
}
}

void Startup::bind_system() {
    auto bind = [&](const char* symbol, Signature signature, HostFunction function) {
        if (imported("kernel32.dll", symbol)) {
            control_.host(loader_.resolve("kernel32.dll", symbol), std::move(signature), std::move(function),
                          std::string("kernel32.dll!") + symbol);
        }
    };
    const PointerDirection in = PointerDirection::In, out = PointerDirection::Out;
    const Signature eax_value{Convention::Stdcall, ReturnChannel::Eax, {}, ""};

    bind("GetCurrentProcess", eax_value, [](Control&, const std::vector<uint64_t>&) { return Result{UINT32_MAX, {}}; });
    auto bad_pointer = [](uint8_t permission) {
        return [permission](Control& c, const std::vector<uint64_t>& a) {
            const uint32_t length = permission == Execute ? 1 : uint32_t(a[1]);
            if (!length) {
                return Result{0, {}};
            }

            try {
                c.memory.check(uint32_t(a[0]), length, permission);
            } catch (const GuestFault&) {
                return Result{1, {}};
            }

            return Result{0, {}};
        };
    };
    bind("IsBadReadPtr", {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}}, ""}, bad_pointer(Read));
    bind("IsBadWritePtr", {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}}, ""}, bad_pointer(Write));
    bind("IsBadCodePtr", {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""}, bad_pointer(Execute));
    bind("LocalFree", {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
         [](Control&, const std::vector<uint64_t>& a) {
             if (a[0]) {
                 throw GuestFault("LocalFree of a block no implemented API allocates");
             }

             return Result{0, {}};
         });
    // Creating a multiplayer game starts the dedicated server, GameServer.exe, as its own process: a Windows program
    // this port does not run. The launch fails as it would without the file, and the game logs that and goes on
    // (it never waits for the server). No other caller starts a process.
    bind("CreateProcessA",
         {Convention::Stdcall, ReturnChannel::Eax, {{4, in}, {4, in}, {4}, {4}, {4}, {4}, {4}, {4, in}, {4}, {4}}, ""},
         [this](Control&, const std::vector<uint64_t>& a) {
             const std::string application = a[0] ? ansi(uint32_t(a[0])) : std::string();
             const std::string command = a[1] ? ansi(uint32_t(a[1])) : std::string();
             std::fprintf(stderr, "CREATE_PROCESS application=%s command=\"%s\" result=unsupported\n",
                          application.c_str(), command.c_str());
             return Result{error(error_file_not_found), {}};
         });
    bind("OutputDebugStringA", {Convention::Stdcall, ReturnChannel::Void, {{4, in}}, ""},
         [this](Control&, const std::vector<uint64_t>& a) {
             if (a[0]) {
                 ansi(uint32_t(a[0]));
             }

             return Result{};
         });
    bind("GetSystemInfo", {Convention::Stdcall, ReturnChannel::Void, {{4, out}}, ""},
         [](Control& c, const std::vector<uint64_t>& a) {
             // Guest threads run serialized under one scheduler lock, so one logical processor is reported.
             const uint32_t fields[] = {0, 4096, 0x10000, 0x7ffeffff, 1, 1, 586, 0x10000, 6};
             c.memory.write(uint32_t(a[0]), fields, sizeof fields);
             return Result{};
         });
    bind("GetCurrentDirectoryA", {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4, out}}, ""},
         [this](Control&, const std::vector<uint64_t>& a) {
             std::string directory = std::filesystem::absolute(environment_.assets).string();
             std::replace(directory.begin(), directory.end(), '/', '\\');
             directory = "Z:" + directory;
             if (a[0] <= directory.size()) {
                 return Result{uint32_t(directory.size()) + 1, {}};
             }

             put_ansi(uint32_t(a[1]), directory);
             return Result{uint32_t(directory.size()), {}};
         });
    auto find_variable = [this](const std::string& name) {
        return std::find_if(environment_.environment.begin(), environment_.environment.end(),
                            [&](const std::string& item) {
                                const auto split = item.find('=', 1);
                                return split != std::string::npos && lower(item.substr(0, split)) == lower(name);
                            });
    };
    bind("GetEnvironmentVariableA", {Convention::Stdcall, ReturnChannel::Eax, {{4, in}, {4, out}, {4}}, ""},
         [this, find_variable](Control&, const std::vector<uint64_t>& a) {
             auto found = find_variable(ansi(uint32_t(a[0])));
             if (found == environment_.environment.end()) {
                 return Result{error(error_env_not_found), {}};
             }

             const std::string value = found->substr(found->find('=', 1) + 1);
             if (a[2] <= value.size()) {
                 return Result{uint32_t(value.size()) + 1, {}};
             }

             put_ansi(uint32_t(a[1]), value);
             return Result{uint32_t(value.size()), {}};
         });
    bind("SetEnvironmentVariableA", {Convention::Stdcall, ReturnChannel::Eax, {{4, in}, {4, in}}, ""},
         [this, find_variable](Control&, const std::vector<uint64_t>& a) {
             const std::string name = ansi(uint32_t(a[0]));
             if (name.empty() || name.find('=') != std::string::npos) {
                 return Result{error(error_invalid_parameter), {}};
             }

             auto found = find_variable(name);
             if (!a[1]) {
                 if (found == environment_.environment.end()) {
                     return Result{error(error_env_not_found), {}};
                 }

                 environment_.environment.erase(found);
                 return Result{1, {}};
             }

             const std::string entry = name + "=" + ansi(uint32_t(a[1]));
             if (found != environment_.environment.end()) {
                 *found = entry;
             } else {
                 environment_.environment.push_back(entry);
             }

             return Result{1, {}};
         });

    auto disk_file = [this](uint32_t handle, bool need_write) -> int {
        auto found = handles_.find(handle);
        if (found == handles_.end() || found->second.fd < 0 || found->second.type != 1) {
            return error(error_invalid_handle), -1;
        }

        const int mode = fcntl(found->second.fd, F_GETFL) & O_ACCMODE;
        if (need_write && mode == O_RDONLY) {
            return error(error_access_denied), -1;
        }

        return found->second.fd;
    };
    bind("FlushFileBuffers", {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
         [this, disk_file](Control&, const std::vector<uint64_t>& a) {
             const int fd = disk_file(uint32_t(a[0]), true);
             if (fd < 0) {
                 return Result{0, {}};
             }

             if (fsync(fd)) {
                 return Result{error(errno_to_win32(errno)), {}};
             }

             return Result{1, {}};
         });
    bind("SetEndOfFile", {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
         [this, disk_file](Control&, const std::vector<uint64_t>& a) {
             const int fd = disk_file(uint32_t(a[0]), true);
             if (fd < 0) {
                 return Result{0, {}};
             }

             const off_t position = lseek(fd, 0, SEEK_CUR);
             if (position < 0 || ftruncate(fd, position)) {
                 return Result{error(errno_to_win32(errno)), {}};
             }

             return Result{1, {}};
         });

    auto parent_exists = [this](const std::string& name) {
        const auto split = name.find_last_of("\\/");
        if (split == std::string::npos) {
            return true;
        }

        const std::string parent = name.substr(0, split);
        if (parent.empty() || parent.back() == ':') {
            return true;
        }

        return std::filesystem::exists(file_path(parent, false, 3));
    };
    bind("CreateDirectoryA", {Convention::Stdcall, ReturnChannel::Eax, {{4, in}, {4}}, ""},
         [this, parent_exists](Control&, const std::vector<uint64_t>& a) {
             if (a[1]) {
                 throw GuestFault("unobserved directory security attributes");
             }

             const std::string name = ansi(uint32_t(a[0]));
             if (std::filesystem::exists(file_path(name, false, 3))) {
                 return Result{error(error_already_exists), {}};
             }

             if (!parent_exists(name)) {
                 return Result{error(error_path_not_found), {}};
             }

             std::error_code failure;
             std::filesystem::create_directory(file_path(name, true, 2), failure);
             if (failure) {
                 return Result{error(errno_to_win32(failure.value())), {}};
             }

             return Result{1, {}};
         });
    bind("CopyFileA", {Convention::Stdcall, ReturnChannel::Eax, {{4, in}, {4, in}, {4}}, ""},
         [this, parent_exists](Control&, const std::vector<uint64_t>& a) {
             const auto from = file_path(ansi(uint32_t(a[0])), false, 3);
             struct stat source{};
             if (stat(from.c_str(), &source)) {
                 return Result{error(errno_to_win32(errno)), {}};
             }

             if (!S_ISREG(source.st_mode)) {
                 return Result{error(error_access_denied), {}};
             }

             const std::string name = ansi(uint32_t(a[1]));
             const auto existing = file_path(name, false, 3);
             struct stat target{};
             const bool present = !stat(existing.c_str(), &target);
             if (present && a[2]) {
                 return Result{error(error_file_exists), {}};
             }

             if (present && (!(target.st_mode & 0222) || S_ISDIR(target.st_mode))) {
                 return Result{error(error_access_denied), {}};
             }

             if (present && target.st_dev == source.st_dev && target.st_ino == source.st_ino) {
                 return Result{error(error_sharing_violation), {}};
             }

             if (!parent_exists(name)) {
                 return Result{error(error_path_not_found), {}};
             }

             std::error_code failure;
             std::filesystem::copy_file(from, file_path(name, true, 2),
                                        std::filesystem::copy_options::overwrite_existing, failure);
             if (failure) {
                 return Result{error(errno_to_win32(failure.value())), {}};
             }

             return Result{1, {}};
         });
    bind("SetFileAttributesA", {Convention::Stdcall, ReturnChannel::Eax, {{4, in}, {4}}, ""},
         [this](Control&, const std::vector<uint64_t>& a) {
             const std::string name = ansi(uint32_t(a[0]));
             const uint32_t attributes = uint32_t(a[1]);
             if (attributes & ~(attribute_readonly | attribute_archive | attribute_normal)) {
                 throw GuestFault("unqualified file attributes " + std::to_string(attributes));
             }

             struct stat status{};
             const auto view = file_path(name, false, 3);
             if (stat(view.c_str(), &status)) {
                 return Result{error(errno_to_win32(errno)), {}};
             }

             if (!S_ISREG(status.st_mode)) {
                 throw GuestFault("unqualified attribute change on a non-file");
             }

             const bool readonly = attributes & attribute_readonly;
             if (!readonly && (status.st_mode & 0200)) {
                 return Result{1, {}};
             }

             const auto target = file_path(name, true, 3);
             if (stat(target.c_str(), &status)) {
                 return Result{error(errno_to_win32(errno)), {}};
             }

             const mode_t mode = readonly ? status.st_mode & ~mode_t(0222) : status.st_mode | S_IWUSR;
             if (chmod(target.c_str(), mode)) {
                 return Result{error(errno_to_win32(errno)), {}};
             }

             return Result{1, {}};
         });

    bind("CreateSemaphoreA", {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}, {4}, {4, in}}, ""},
         [this](Control&, const std::vector<uint64_t>& a) {
             if (a[0]) {
                 throw GuestFault("unobserved semaphore security attributes");
             }

             const int32_t initial = int32_t(a[1]), maximum = int32_t(a[2]);
             if (maximum <= 0 || initial < 0 || initial > maximum) {
                 return Result{error(error_invalid_parameter), {}};
             }

             const std::string name = a[3] ? ansi(uint32_t(a[3])) : "";
             auto semaphore = std::make_shared<Semaphore>();
             semaphore->count = uint32_t(initial);
             semaphore->maximum = uint32_t(maximum);
             bool exists = false;
             if (!name.empty()) {
                 for (const auto& handle : handles_) {
                     if (handle.second.name == name) {
                         auto found = semaphores_.find(handle.first);
                         if (found == semaphores_.end()) {
                             return Result{error(error_invalid_handle), {}};
                         }

                         semaphore = found->second;
                         exists = true;
                         break;
                     }
                 }
             }

             const uint32_t handle = next_handle_++;
             handles_.emplace(handle, Handle{-1, 0, name});
             semaphores_.emplace(handle, std::move(semaphore));
             error(exists ? error_already_exists : 0);
             return Result{handle, {}};
         });
    bind("ReleaseSemaphore", {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}, {4, out}}, ""},
         [this](Control& c, const std::vector<uint64_t>& a) {
             auto found = semaphores_.find(uint32_t(a[0]));
             if (found == semaphores_.end()) {
                 return Result{error(error_invalid_handle), {}};
             }

             auto& semaphore = *found->second;
             if (int32_t(a[1]) <= 0) {
                 return Result{error(error_invalid_parameter), {}};
             }

             if (uint64_t(semaphore.count) + a[1] > semaphore.maximum) {
                 return Result{error(error_too_many_posts), {}};
             }

             if (a[2]) {
                 c.memory.store32(uint32_t(a[2]), semaphore.count);
             }

             semaphore.count += uint32_t(a[1]);
             control_.scheduler().changed();
             return Result{1, {}};
         });

    // Events: tincat2.dll's socket threads (LAN join) create unnamed auto/manual-reset events.
    bind("CreateEventA", {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}, {4}, {4, in}}, ""},
         [this](Control&, const std::vector<uint64_t>& a) {
             if (a[0]) {
                 throw GuestFault("unobserved event security attributes");
             }

             if (a[3]) {
                 throw GuestFault("unobserved named event");
             }

             auto event = std::make_shared<Event>();
             event->manual = a[1] != 0;
             event->signaled = a[2] != 0;
             const uint32_t handle = next_handle_++;
             handles_.emplace(handle, Handle{-1, 0, ""});
             events_.emplace(handle, std::move(event));
             error(0);
             return Result{handle, {}};
         });
    bind("SetEvent", {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
         [this](Control&, const std::vector<uint64_t>& a) {
             auto found = events_.find(uint32_t(a[0]));
             if (found == events_.end()) {
                 return Result{error(error_invalid_handle), {}};
             }

             found->second->signaled = true;
             control_.scheduler().changed();
             return Result{1, {}};
         });
    bind("ResetEvent", {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
         [this](Control&, const std::vector<uint64_t>& a) {
             auto found = events_.find(uint32_t(a[0]));
             if (found == events_.end()) {
                 return Result{error(error_invalid_handle), {}};
             }

             found->second->signaled = false;
             return Result{1, {}};
         });

    bind("VirtualAlloc", {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}, {4}, {4}}, ""},
         [this](Control& c, const std::vector<uint64_t>& a) {
             if (a[0] || a[2] != mem_commit_reserve || a[3] != page_readwrite || !a[1]) {
                 throw GuestFault("unqualified VirtualAlloc request");
             }

             if (a[1] > Memory::size / 4) {
                 return Result{error(error_not_enough_memory), {}};
             }

             const uint32_t block = c.heap.allocate(uint32_t(a[1]));
             const std::vector<uint8_t> zero(a[1]);
             c.memory.write(block, zero.data(), zero.size());
             virtual_blocks_.emplace(block, uint32_t(a[1]));
             return Result{block, {}};
         });
    bind("VirtualFree", {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}, {4}}, ""},
         [this](Control& c, const std::vector<uint64_t>& a) {
             if (a[2] != mem_release || a[1]) {
                 throw GuestFault("unqualified VirtualFree request");
             }

             auto found = virtual_blocks_.find(uint32_t(a[0]));
             if (found == virtual_blocks_.end()) {
                 return Result{error(error_invalid_address), {}};
             }

             c.heap.free(found->first);
             virtual_blocks_.erase(found);
             return Result{1, {}};
         });
}
}
