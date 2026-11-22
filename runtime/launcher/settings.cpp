#include "settings.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace sacred::launcher {
namespace {
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

// The key of a "KEY : value" line, "" for anything else.
std::string key_of(const std::string& line) {
    const size_t colon = line.find(':');
    return colon == std::string::npos ? std::string() : trim(line.substr(0, colon));
}

bool all_digits(const std::string& text) {
    return !text.empty() && text.size() <= 6 &&
           std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

bool same_value(const std::string& a, const std::string& b) {
    if (all_digits(a) && all_digits(b)) {
        return std::stol(a) == std::stol(b);
    }

    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

int number(const std::string& text) {
    return all_digits(text) ? int(std::stol(text)) : -1;
}
}

// ------------------------------------------------------------------------------------------------------ the file
bool SettingsFile::load(const std::filesystem::path& file) {
    lines_.clear();
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return false;
    }

    std::ostringstream text;
    text << in.rdbuf();
    parse(text.str());
    return true;
}

void SettingsFile::parse(const std::string& text) {
    lines_.clear();
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        lines_.push_back(line);
    }
}

std::string SettingsFile::text() const {
    std::string out;
    for (const auto& line : lines_) {
        out += line + "\r\n";
    }

    return out;
}

bool SettingsFile::save(const std::filesystem::path& file) const {
    // Written next to the file and renamed over it, so that a crash never leaves half a Settings.cfg.
    const std::filesystem::path partial = file.string() + ".launcher-tmp";
    {
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }

        out << text();
        if (!out) {
            return false;
        }
    }

    std::error_code status;
    std::filesystem::rename(partial, file, status);
    if (status) {
        std::filesystem::remove(partial, status);
    }

    return !status;
}

std::string SettingsFile::get(const std::string& key) const {
    std::string value;
    for (const auto& line : lines_) {
        if (key_of(line) == key) {
            value = trim(line.substr(line.find(':') + 1));
        }
    }

    return value;
}

bool SettingsFile::has(const std::string& key) const {
    return std::any_of(lines_.begin(), lines_.end(), [&](const std::string& line) { return key_of(line) == key; });
}

void SettingsFile::set(const std::string& key, const std::string& value) {
    const std::string line = key + " : " + value;
    bool placed = false;
    for (auto it = lines_.begin(); it != lines_.end();) {
        if (key_of(*it) != key) {
            ++it;
            continue;
        }

        if (!placed && !value.empty()) {
            *it++ = line;
            placed = true;
        } else {
            it = lines_.erase(it);
        }
    }

    if (placed || value.empty()) {
        return;
    }

    // A new key goes where the game's sorted order puts it (the game sorts when it writes the file anyway).
    auto at = std::find_if(lines_.begin(), lines_.end(), [&](const std::string& other) {
        const std::string other_key = key_of(other);
        return !other_key.empty() && other_key > key;
    });
    lines_.insert(at, line);
}

// --------------------------------------------------------------------------------------------------- the options
size_t Option::index_of(const std::string& value) const {
    if (value.empty()) {
        return 0;
    }

    for (const auto& same : same_as_default) {
        if (same_value(value, same)) {
            return 0;
        }
    }

    for (size_t i = 1; i < choices.size(); ++i) {
        if (same_value(value, choices[i].value)) {
            return i;
        }
    }

    return choices.size(); // not among the choices
}

const std::vector<Option>& options() {
    static const std::vector<Option> list = {
        {"FPS_LIMIT",
         Section::display,
         "Frame rate limit",
         "The game's frame cap. Movement, animation and game logic keep their speed at any rate; a network game stays "
         "at 60. \"Display\" follows the screen's refresh rate.",
         {{"", "60 (original)"},
          {"75", "75"},
          {"90", "90"},
          {"120", "120"},
          {"144", "144"},
          {"165", "165"},
          {"240", "240"},
          {"auto", "Display"}},
         {"60"},
         "auto"},
        {"SHOW_FPS",
         Section::display,
         "FPS counter",
         "Shows the frames drawn per second in the top-left corner of the game.",
         {{"", "Off"}, {"1", "On"}},
         {"0"},
         ""},
        {"LEGACY_UI",
         Section::look,
         "Interface",
         "Original Sacred brings back the bronze frames with vines, the sunlit gate in the main menu, the dwarven "
         "hall behind the options and the old menu music. Both looks come from your installation.",
         {{"", "Sacred Underworld"}, {"1", "Original Sacred"}},
         {"0"},
         ""},
        {"RENDER_SCALE",
         Section::graphics,
         "Render scale",
         "The 3D scene and the screen are drawn at a multiple of the screen size and filtered down: sharper models "
         "and edges, at that multiple squared in graphics work.",
         {{"", "1x (off)"}, {"2", "2x"}, {"3", "3x"}, {"4", "4x"}},
         {"1"},
         "2"},
        {"TEXTURE_UPSCALE",
         Section::graphics,
         "HD textures",
         "The game's own textures upscaled in the background the first time they are used. They only "
         "show with a render scale of 2x or more.",
         {{"", "Original"}, {"2", "2x upscale"}, {"3", "3x upscale"}, {"4", "4x upscale"}},
         {"0", "1"},
         "2"},
        {"AUTO_PICKUP",
         Section::gameplay,
         "Auto pick-up",
         "The hero picks up the loot lying next to it while standing or walking, without a click. What it takes "
         "follows the game's own pick-up setting in the in-game Options. Items you drop stay where they are. Single "
         "player only, and not on horseback.",
         {{"", "Off"}, {"1", "On"}},
         {"0"},
         ""},
    };
    return list;
}

const Option* find_option(const std::string& key) {
    for (const auto& option : options()) {
        if (option.key == key) {
            return &option;
        }
    }

    return nullptr;
}

const char* section_title(Section section) {
    switch (section) {
    case Section::display:
        return "Display";
    case Section::look:
        return "Look";
    case Section::graphics:
        return "Graphics";
    case Section::gameplay:
        return "Gameplay";
    }

    return "";
}

// ------------------------------------------------------------------------------------------------ the screen size
std::string ScreenSize::label() const {
    if (width == 0) {
        return fit ? "Fit the display" : "1024 x 768 (original)";
    }

    return std::to_string(width) + " x " + std::to_string(height);
}

std::vector<ScreenSize> screen_sizes(int display_w, int display_h, const ScreenSize& current) {
    std::vector<ScreenSize> list = {{0, 0, false}, {0, 0, true}};
    static const int common[][2] = {{1280, 720},  {1366, 768},  {1600, 900},  {1920, 1080},
                                    {2560, 1080}, {2560, 1440}, {3440, 1440}, {3840, 2160}};
    for (const auto& size : common) {
        if (display_w <= 0 || (size[0] <= display_w && size[1] <= display_h)) {
            list.push_back({size[0], size[1], false});
        }
    }

    auto known = [&](const ScreenSize& size) {
        return std::find(list.begin(), list.end(), size) != list.end();
    };
    if (display_w > 0 && !(display_w == 1024 && display_h == 768)) {
        const ScreenSize own{std::max(display_w, 1024), std::max(display_h, 720), false};
        if (!known(own)) {
            list.push_back(own);
        }
    }

    if (!known(current)) {
        list.push_back(current);
    }

    std::sort(list.begin() + 2, list.end(), [](const ScreenSize& a, const ScreenSize& b) {
        return a.width != b.width ? a.width < b.width : a.height < b.height;
    });
    return list;
}

ScreenSize read_screen_size(const SettingsFile& settings, bool launcher_fit) {
    const int width = number(settings.get("HD_WIDTH")), height = number(settings.get("HD_HEIGHT"));
    if (width < 0 || height < 0) {
        return {0, 0, launcher_fit};
    }

    return {std::max(width, 1024), std::max(height, 720), false};
}

void write_screen_size(SettingsFile& settings, const ScreenSize& size) {
    settings.set("HD_WIDTH", size.width ? std::to_string(size.width) : "");
    settings.set("HD_HEIGHT", size.width ? std::to_string(size.height) : "");
}

void reset_to_defaults(SettingsFile& settings) {
    for (const auto& option : options()) {
        settings.set(option.key, option.recommended);
    }

    for (const char* key : {"HD_WIDTH", "HD_HEIGHT"}) {
        settings.set(key, "");
    }
}

std::string summary(const SettingsFile& settings, const ScreenSize& size, bool fullscreen) {
    std::string text = fullscreen ? "Fullscreen" : "Window";
    text += ", " + (size.width == 0 && size.fit ? std::string(fullscreen ? "the display's size" : "1024 x 768")
                                                : size.label());
    auto choice = [&](const char* key) {
        const Option& option = *find_option(key);
        const size_t index = option.index_of(settings.get(key));
        return index < option.choices.size() ? index : size_t(-1);
    };
    const std::string fps = settings.get("FPS_LIMIT");
    const size_t fps_index = choice("FPS_LIMIT");
    if (fps_index != size_t(-1) && find_option("FPS_LIMIT")->choices[fps_index].value == "auto") {
        text += ", FPS at the display's refresh rate";
    } else {
        text += ", " + (fps_index == 0 ? std::string("60") : fps) + " FPS";
    }

    if (choice("SHOW_FPS") == 1) {
        text += ", FPS counter";
    }

    if (choice("LEGACY_UI") == 1) {
        text += ", original Sacred look";
    }

    if (choice("RENDER_SCALE") != 0) {
        text += ", render " + settings.get("RENDER_SCALE") + "x";
    }

    if (choice("TEXTURE_UPSCALE") != 0) {
        text += ", HD textures " + settings.get("TEXTURE_UPSCALE") + "x";
    }

    if (!settings.get("TEXTURE_PACK").empty()) {
        text += ", texture pack";
    }

    if (choice("AUTO_PICKUP") == 1) {
        text += ", auto pick-up";
    }

    return text;
}
}
