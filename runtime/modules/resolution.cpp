#include "resolution.h"
#include "resolution_table.h"
#include "../process/memory.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>

namespace sacred {
namespace {
const ResolutionTable* g_table = nullptr;
const ResolutionTable& table() {
    if (!g_table) {
        throw GuestFault("no resolution table in use (use_resolution_table)");
    }

    return *g_table;
}

std::string trim(const std::string& text) {
    size_t begin = 0, end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }

    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }

    return text.substr(begin, end - begin);
}

std::optional<int32_t> number(const std::string& text) {
    if (text.empty() || text.size() > 6 ||
        !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return std::nullopt;
    }

    return int32_t(std::stol(text));
}
}

ResolutionConfig parse_resolution_settings(const std::string& text) {
    std::optional<int32_t> width, height;
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        const size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }

        const std::string key = trim(line.substr(0, colon));
        if (key == "HD_WIDTH") {
            width = number(trim(line.substr(colon + 1)));
        } else if (key == "HD_HEIGHT") {
            height = number(trim(line.substr(colon + 1)));
        }
    }

    ResolutionConfig config;
    if (!width || !height) {
        return config;
    }

    config.enabled = true;
    config.width = std::max(*width, resolution_min_width);
    config.height = std::max(*height, resolution_min_height);
    return config;
}

ResolutionConfig display_resolution(int width, int height) {
    ResolutionConfig config;
    config.width = std::max(int32_t(width), resolution_min_width);
    config.height = std::max(int32_t(height), resolution_min_height);
    config.enabled = !(config.width == 1024 && config.height == 768);
    if (!config.enabled) {
        config.height = 768;
    }

    return config;
}

std::string option_directory(const std::string& value, const char* home) {
    std::string path = trim(value);
    if (path.size() >= 2 && (path.front() == '"' || path.front() == '\'') && path.back() == path.front()) {
        path = trim(path.substr(1, path.size() - 2));
    }

    if (home && *home && (path == "~" || path.rfind("~/", 0) == 0)) {
        std::string base = home;
        while (base.size() > 1 && base.back() == '/') {
            base.pop_back();
        }

        path = base + path.substr(1);
    }

    return path;
}

HdOptions parse_hd_options(const std::string& text, const char* env_scale, const char* env_pack, const char* env_dump,
                           const char* env_backgrounds, const char* home) {
    HdOptions options;
    // An invalid value (not an integer 1..4) is ignored with a note: the previous one (Settings.cfg's, or 1) stays.
    auto scale = [&](const std::string& value, const char* source) {
        const auto k = number(trim(value));
        if (k && *k >= 1 && *k <= 4) {
            options.render_scale = uint32_t(*k);
        } else {
            options.notes.push_back(std::string(source) + " '" + value + "' is not an integer 1..4; ignored");
        }
    };
    auto upscale = [&](const std::string& value, const char* source) {
        const auto k = number(trim(value));
        if (k && *k >= 0 && *k <= 4) {
            options.texture_upscale = *k >= 2 ? uint32_t(*k) : 0;
        } else {
            options.notes.push_back(std::string(source) + " '" + value + "' is not an integer 0..4; ignored");
        }
    };
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        const size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }

        const std::string key = trim(line.substr(0, colon));
        if (key == "RENDER_SCALE") {
            scale(trim(line.substr(colon + 1)), "RENDER_SCALE");
        } else if (key == "TEXTURE_PACK") {
            options.texture_pack = option_directory(line.substr(colon + 1), home);
        } else if (key == "MENU_BACKGROUNDS") {
            options.menu_backgrounds = option_directory(line.substr(colon + 1), home);
        } else if (key == "TEXTURE_UPSCALE") {
            upscale(trim(line.substr(colon + 1)), "TEXTURE_UPSCALE");
        }
    }

    if (env_scale && *env_scale) {
        scale(env_scale, "SACRED_RENDER_SCALE");
    }

    if (env_pack && *env_pack) {
        options.texture_pack = option_directory(env_pack, home);
    }

    if (env_dump && *env_dump) {
        options.texture_dump = env_dump;
    }

    if (env_backgrounds && *env_backgrounds) {
        options.menu_backgrounds = option_directory(env_backgrounds, home);
    }

    if (const char* env_upscale = std::getenv("SACRED_TEXTURE_UPSCALE"); env_upscale && *env_upscale) {
        upscale(env_upscale, "SACRED_TEXTURE_UPSCALE");
    }

    return options;
}

size_t parse_texture_cache_mb(const char* value, std::string* note) {
    // Upscaled textures the game drops and loads again are taken from here instead of being upscaled again (~7 ms
    // each on a worker). 512 MB held ~700 MB of heap in a town for few hits; 128 MB keeps the recent ones.
    constexpr size_t fallback = 128;
    if (!value || !*value) {
        return fallback;
    }

    const auto megabytes = number(trim(value));
    if (megabytes && *megabytes >= 1 && *megabytes <= 65536) {
        return size_t(*megabytes);
    }

    if (note) {
        *note = std::string("SACRED_TEXTURE_CACHE_MB '") + value + "' is not an integer 1..65536; using 128";
    }

    return fallback;
}

HdOptions read_hd_options(const std::string& settings_path) {
    std::ifstream file(settings_path, std::ios::binary);
    std::ostringstream text;
    if (file) {
        text << file.rdbuf();
    }

    return parse_hd_options(text.str(), std::getenv("SACRED_RENDER_SCALE"), std::getenv("SACRED_TEXTURE_PACK"),
                            std::getenv("SACRED_TEXTURE_DUMP"), std::getenv("SACRED_MENU_BACKGROUNDS"),
                            std::getenv("HOME"));
}

ResolutionConfig read_resolution_settings(const std::string& settings_path) {
    std::ifstream file(settings_path, std::ios::binary);
    if (!file) {
        return {};
    }

    std::ostringstream text;
    text << file.rdbuf();
    return parse_resolution_settings(text.str());
}

FpsLimit parse_fps_limit(const std::string& text, const char* env_limit) {
    FpsLimit limit;
    auto take = [&](const std::string& value, const char* source) {
        if (value == "auto" || value == "AUTO") {
            limit.automatic = true;
            return;
        }

        const auto fps = number(value);
        if (fps && *fps >= fps_limit_min && *fps <= fps_limit_max) {
            limit.fps = *fps;
            limit.automatic = false;
        } else {
            limit.notes.push_back(std::string(source) + " '" + value + "' is not auto or an integer 30..240; ignored");
        }
    };
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        const size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }

        if (trim(line.substr(0, colon)) == "FPS_LIMIT") {
            take(trim(line.substr(colon + 1)), "FPS_LIMIT");
        }
    }

    if (env_limit && *env_limit) {
        take(trim(env_limit), "SACRED_FPS_LIMIT");
    }

    return limit;
}

FpsLimit read_fps_limit(const std::string& settings_path) {
    std::ifstream file(settings_path, std::ios::binary);
    std::ostringstream text;
    if (file) {
        text << file.rdbuf();
    }

    return parse_fps_limit(text.str(), std::getenv("SACRED_FPS_LIMIT"));
}

bool has_wait_retrace(const std::string& settings_path) {
    std::ifstream file(settings_path, std::ios::binary);
    std::string line;
    while (std::getline(file, line)) {
        const size_t colon = line.find(':');
        if (colon != std::string::npos && trim(line.substr(0, colon)) == "WAITRETRACE") {
            return true;
        }
    }

    return false;
}

std::string read_setting(const std::string& settings_path, const std::string& key) {
    std::ifstream file(settings_path, std::ios::binary);
    std::string value;
    for (std::string line; std::getline(file, line);) {
        const size_t colon = line.find(':');
        if (colon != std::string::npos && trim(line.substr(0, colon)) == key) {
            value = trim(line.substr(colon + 1));
        }
    }

    return value;
}

int32_t resolve_fps_limit(const FpsLimit& limit, int refresh_hz) {
    const int32_t fps = limit.automatic ? (refresh_hz > 0 ? int32_t(refresh_hz) : fps_limit_default) : limit.fps;
    return std::clamp(fps, fps_limit_min, fps_limit_max);
}

namespace {
unsigned write_cells(Memory& memory, const ResolutionConfig& config, int32_t fps, bool fps_only) {
    const int32_t width = config.enabled ? config.width : 1024, height = config.enabled ? config.height : 768;
    unsigned writes = 0;
    for (unsigned i = 0; i < table().write_count; ++i) {
        const ResolutionWrite& entry = table().writes[i];
        if (!entry.size || (!entry.always && !config.enabled) || (fps_only && !entry.fps)) {
            continue;
        }

        // Data sites may sit in read-only sections: the loader-side write goes straight to the mapped image after
        // the committed-range check (the guest itself never gains write access there).
        memory.check(entry.address, entry.size, 0);
        const uint64_t value = table().value(i, width, height, config.enabled ? 1 : 0, fps);
        unsigned char bytes[8];
        for (unsigned b = 0; b < 8; ++b) {
            bytes[b] = static_cast<unsigned char>(value >> (8 * b));
        }

        std::memcpy(reinterpret_cast<void*>(memory.host_address(entry.address)), bytes, entry.size);
        ++writes;
    }

    return writes;
}
}

unsigned apply_resolution(Memory& memory, const ResolutionConfig& config, int32_t fps) {
    return write_cells(memory, config, fps, false);
}

unsigned apply_fps_limit(Memory& memory, const ResolutionConfig& config, int32_t fps) {
    return write_cells(memory, config, fps, true);
}

std::vector<std::string> parse_option_settings(const std::string& text) {
    std::vector<std::string> on;
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        const size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }

        const std::string key = trim(line.substr(0, colon)), value = trim(line.substr(colon + 1));
        for (unsigned i = 0; i < table().option_count; ++i) {
            if (key != table().options[i].setting) {
                continue;
            }

            const auto known = std::find(on.begin(), on.end(), key);
            const bool enabled = !value.empty() && value != "0";
            if (enabled && known == on.end()) {
                on.push_back(key);
            }

            if (!enabled && known != on.end()) {
                on.erase(known);
            }
        }
    }

    return on;
}

std::vector<std::string> apply_options(Memory& memory, const std::string& settings_path) {
    std::ifstream file(settings_path, std::ios::binary);
    std::ostringstream text;
    if (file) {
        text << file.rdbuf();
    }

    const std::vector<std::string> on = parse_option_settings(text.str());
    for (unsigned i = 0; i < table().blob_count; ++i) {
        const ResolutionBlob& blob = table().blobs[i];
        if (!blob.size) {
            continue;
        }

        memory.check(blob.address, blob.size, 0);
        std::memcpy(reinterpret_cast<void*>(memory.host_address(blob.address)), blob.bytes, blob.size);
    }

    for (unsigned i = 0; i < table().option_write_count; ++i) {
        const ResolutionOptionWrite& write = table().option_writes[i];
        if (!write.size || std::find(on.begin(), on.end(), write.setting) == on.end()) {
            continue;
        }

        memory.check(write.address, write.size, 0);
        std::memcpy(reinterpret_cast<void*>(memory.host_address(write.address)), write.bytes, write.size);
    }

    for (unsigned i = 0; i < table().option_count; ++i) {
        const ResolutionOption& option = table().options[i];
        if (!option.address) {
            continue;
        }

        memory.check(option.address, 4, 0);
        const uint32_t value = std::find(on.begin(), on.end(), option.setting) != on.end() ? 1u : 0u;
        std::memcpy(reinterpret_cast<void*>(memory.host_address(option.address)), &value, 4);
    }

    return on;
}

uint32_t resolution_inset_word() {
    return table().inset_address;
}

void use_resolution_table(const ResolutionTable& resolution) {
    g_table = &resolution;
}
}
