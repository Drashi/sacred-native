#include "registry.h"
#include <algorithm>

namespace sacred {
namespace {
constexpr uint32_t error_not_found = 2, error_invalid_handle = 6, error_more_data = 234;
std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return value;
}

std::string root_name(uint32_t handle) {
    switch (handle) {
    case 0x80000000:
        return "hkcr";
    case 0x80000001:
        return "hkcu";
    case 0x80000002:
        return "hklm";
    case 0x80000003:
        return "hku";
    default:
        return {};
    }
}
}

std::string Registry::text(uint32_t pointer) const {
    std::string result;
    for (;;) {
        uint8_t byte;
        control_.memory.read(pointer + uint32_t(result.size()), &byte, 1);
        if (!byte) {
            return result;
        }

        if (result.size() > 4096) {
            throw GuestFault("unterminated registry string");
        }

        result.push_back(char(byte));
    }
}

std::string Registry::path(uint32_t parent, const std::string& subkey) const {
    std::string base = root_name(parent);
    if (base.empty()) {
        auto found = handles_.find(parent);
        if (found == handles_.end()) {
            throw GuestFault("invalid registry key handle");
        }

        base = found->second;
    }

    std::string tail = lower(subkey);
    std::replace(tail.begin(), tail.end(), '/', '\\');
    return tail.empty() ? base : base + "\\" + tail;
}

uint32_t Registry::open(uint32_t parent, const std::string& subkey, uint32_t result, bool create,
                        uint32_t* disposition) {
    if (root_name(parent).empty() && !handles_.count(parent)) {
        return error_invalid_handle;
    }

    const std::string full = path(parent, subkey);
    const bool root = full.find('\\') == std::string::npos;
    if (!root && !keys_.count(full)) {
        if (!create) {
            return error_not_found;
        }

        keys_.emplace(full, Values{});
        if (disposition) {
            *disposition = 1;
        }
    } else if (disposition) {
        *disposition = 2;
    }

    const uint32_t handle = next_handle_++;
    handles_.emplace(handle, full);
    control_.memory.store32(result, handle);
    return 0;
}

void Registry::bind() {
    control_.host(
        loader_.resolve("advapi32.dll", "RegOpenKeyA"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4, PointerDirection::In}, {4, PointerDirection::Out}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            return Result{
                open(uint32_t(a[0]), a[1] ? text(uint32_t(a[1])) : std::string(), uint32_t(a[2]), false, nullptr), {}};
        },
        "advapi32.dll!RegOpenKeyA");
    control_.host(
        loader_.resolve("advapi32.dll", "RegOpenKeyExA"),
        {Convention::Stdcall,
         ReturnChannel::Eax,
         {{4}, {4, PointerDirection::In}, {4}, {4}, {4, PointerDirection::Out}},
         ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            return Result{
                open(uint32_t(a[0]), a[1] ? text(uint32_t(a[1])) : std::string(), uint32_t(a[4]), false, nullptr), {}};
        },
        "advapi32.dll!RegOpenKeyExA");
    control_.host(
        loader_.resolve("advapi32.dll", "RegCreateKeyExA"),
        {Convention::Stdcall,
         ReturnChannel::Eax,
         {{4},
          {4, PointerDirection::In},
          {4},
          {4},
          {4},
          {4},
          {4},
          {4, PointerDirection::Out},
          {4, PointerDirection::Out}},
         ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            if (a[6]) {
                throw GuestFault("unqualified registry security attributes");
            }

            uint32_t disposition = 0;
            const uint32_t status = open(uint32_t(a[0]), text(uint32_t(a[1])), uint32_t(a[7]), true, &disposition);
            if (!status && a[8]) {
                c.memory.store32(uint32_t(a[8]), disposition);
            }

            return Result{status, {}};
        },
        "advapi32.dll!RegCreateKeyExA");
    control_.host(
        loader_.resolve("advapi32.dll", "RegCloseKey"), {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            if (!root_name(uint32_t(a[0])).empty()) {
                return Result{0, {}};
            }

            return Result{handles_.erase(uint32_t(a[0])) ? 0u : error_invalid_handle, {}};
        },
        "advapi32.dll!RegCloseKey");
    control_.host(
        loader_.resolve("advapi32.dll", "RegQueryValueExA"),
        {Convention::Stdcall,
         ReturnChannel::Eax,
         {{4},
          {4, PointerDirection::In},
          {4},
          {4, PointerDirection::Out},
          {4, PointerDirection::Out},
          {4, PointerDirection::InOut}},
         ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            auto key = handles_.find(uint32_t(a[0]));
            if (key == handles_.end()) {
                return Result{error_invalid_handle, {}};
            }

            auto values = keys_.find(key->second);
            if (values == keys_.end()) {
                return Result{error_not_found, {}};
            }

            auto value = values->second.find(lower(a[1] ? text(uint32_t(a[1])) : std::string()));
            if (value == values->second.end()) {
                return Result{error_not_found, {}};
            }

            if (a[3]) {
                c.memory.store32(uint32_t(a[3]), value->second.type);
            }

            const uint32_t size = uint32_t(value->second.data.size());
            if (!a[5]) {
                return Result{a[4] ? 87u : 0u, {}};
            }

            const uint32_t capacity = c.memory.load32(uint32_t(a[5]));
            c.memory.store32(uint32_t(a[5]), size);
            if (!a[4]) {
                return Result{0, {}};
            }

            if (capacity < size) {
                return Result{error_more_data, {}};
            }

            c.memory.write(uint32_t(a[4]), value->second.data.data(), size);
            return Result{0, {}};
        },
        "advapi32.dll!RegQueryValueExA");
    control_.host(
        loader_.resolve("advapi32.dll", "RegSetValueExA"),
        {Convention::Stdcall,
         ReturnChannel::Eax,
         {{4}, {4, PointerDirection::In}, {4}, {4}, {4, PointerDirection::In}, {4}},
         ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            auto key = handles_.find(uint32_t(a[0]));
            if (key == handles_.end()) {
                return Result{error_invalid_handle, {}};
            }

            std::vector<uint8_t> bytes(a[5]);
            if (!bytes.empty()) {
                c.memory.read(uint32_t(a[4]), bytes.data(), bytes.size());
            }

            keys_[key->second][lower(a[1] ? text(uint32_t(a[1])) : std::string())] = Value{uint32_t(a[3]), bytes};
            return Result{0, {}};
        },
        "advapi32.dll!RegSetValueExA");
}
}
