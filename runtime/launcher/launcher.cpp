// sacred-launcher: the window a player starts. It finds (or asks for) the Sacred Gold installation, checks that it is
// the supported version, offers this project's options on a settings page (written straight into the profile's
// Settings.cfg; settings.h says what each one is), builds the game module from the player's own files the first time
// (tools/launcher/build_game.py: translator + bundled compiler, with a progress bar) and then starts the game through
// the sacred-gold script. Nothing of the game is part of the download.
//
// Layout of the bundle it expects next to itself: sacred-gold, sacred-native, lib/, source/ (translator, recipe,
// runtime headers), python/ (the interpreter the translator runs on), zig/ (the C compiler used when the system has
// none) and menu-backgrounds/. In the macOS app (Sacred Native.app) the executables are in Contents/MacOS, the
// libraries in Contents/Frameworks and the rest in Contents/Resources (resource_dir). In a build tree the same pieces
// come from SACRED_LAUNCHER_ROOT / _PYTHON / _CC. Everything it writes goes to the profile, never next to itself.
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include "platform/font.h"
#include "platform/digest.h"
#include "platform/app_icon.h"
#include "runtime/launcher/settings.h"
#include "runtime/process/loader.h"
#include <SDL.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace sacred::launcher;

namespace {
// ---------------------------------------------------------------------------------------------------- paths, config
std::string env(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

fs::path executable_dir(const char* argv0) {
    std::error_code status;
    fs::path self = fs::read_symlink("/proc/self/exe", status);
#ifdef __APPLE__
    if (status || self.empty()) {
        char path[4096];
        uint32_t size = sizeof path;
        if (_NSGetExecutablePath(path, &size) == 0) {
            self = fs::weakly_canonical(path, status);
        }
    }
#endif
    if (status || self.empty()) {
        self = fs::absolute(argv0, status);
    }

    return self.parent_path();
}

// Where the read-only pieces lie: next to the executable, or in Contents/Resources when the executable is in
// Contents/MacOS of the macOS app.
fs::path resource_dir(const fs::path& here) {
    const fs::path contents = here.parent_path();
    if (here.filename() == "MacOS" && contents.filename() == "Contents" && fs::is_directory(contents / "Resources")) {
        return contents / "Resources";
    }

    return here;
}

std::string shell_quote(const std::string& text) {
    std::string out = "'";
    for (char c : text) {
        out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    }

    return out + "'";
}

std::string trim(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }

    while (!text.empty() && text.front() == ' ') {
        text.erase(text.begin());
    }

    return text;
}

// What only the launcher keeps (launcher.cfg): the display mode, which the sacred-gold script passes on as
// --fullscreen / --windowed, and whether "no screen size" means the display's size or the original one.
struct Config {
    std::string game;
    bool fullscreen = default_fullscreen;
    bool fit = default_fit;
};

fs::path profile_dir() {
    if (const std::string chosen = env("SACRED_PROFILE"); !chosen.empty()) {
        return chosen;
    }

    char* path = SDL_GetPrefPath("Sacred Native", "Sacred Gold");
    fs::path result = path ? fs::path(path) : fs::current_path() / "profile";
    SDL_free(path);
    // SDL's path ends in a separator: without it parent_path() is the data directory the sacred-gold script uses.
    while (!result.empty() && !result.has_filename() && result.has_parent_path() && result != result.root_path()) {
        result = result.parent_path();
    }

    return result;
}

Config load_config(const fs::path& profile) {
    Config config;
    std::ifstream file(profile / "launcher.cfg");
    for (std::string line; std::getline(file, line);) {
        const size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }

        const std::string key = line.substr(0, equals), value = trim(line.substr(equals + 1));
        if (key == "fullscreen") {
            config.fullscreen = value == "1";
        } else if (key == "fit") {
            config.fit = value == "1";
        } else if (key == "resolution") {
            config.fit = value == "1"; // earlier launchers: 1 was "fit the display"
        }
    }

    std::ifstream path(profile / "game-path"); // shared with the sacred-gold script
    std::getline(path, config.game);
    config.game = trim(config.game);
    return config;
}

void save_config(const fs::path& profile, const Config& config) {
    std::error_code status;
    fs::create_directories(profile, status);
    std::ofstream(profile / "launcher.cfg") << "fullscreen=" << config.fullscreen << "\nfit=" << config.fit << "\n";
    if (!config.game.empty()) {
        std::ofstream(profile / "game-path") << config.game << "\n";
    }
}

std::vector<std::string> candidates() {
    const std::string home = env("HOME");
    std::vector<std::string> list;
    if (!env("SACRED_GOLD_DIR").empty()) {
        list.push_back(env("SACRED_GOLD_DIR"));
    }
#ifdef __APPLE__
    for (const char* base : {"/Games/Sacred Gold", "/Applications/Sacred Gold", "/GOG Games/Sacred Gold"}) {
        list.push_back(home + base);
    }
    list.push_back("/Applications/Sacred Gold");
#else
    for (const char* base :
         {"/GOG Games/Sacred Gold", "/Games/Sacred Gold", "/Games/sacred-gold", "/.wine/drive_c/GOG Games/Sacred Gold",
          "/.local/share/Steam/steamapps/common/Sacred Gold", "/.steam/steam/steamapps/common/Sacred Gold"}) {
        list.push_back(home + base);
    }
    for (const char* path : {"/opt/Sacred Gold", "/opt/sacred-gold", "/usr/local/games/Sacred Gold"}) {
        list.push_back(path);
    }
#endif
    return list;
}

// "" when the installation is the supported one, otherwise what is wrong.
std::string check_installation(const std::string& game) {
    if (game.empty()) {
        return "Choose the folder of your Sacred Gold installation.";
    }
    for (const auto& spec : sacred::sacred_modules) {
        const std::string name = spec.name == "sacred.exe" ? "Sacred.exe" : spec.name;
        std::ifstream file(fs::path(game) / name, std::ios::binary);
        if (!file) {
            return name + " is missing in this folder.";
        }
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const auto digest = sacred::host_sha256(bytes.data(), bytes.size());
        char hex[65];
        for (int i = 0; i < 32; ++i) {
            std::snprintf(hex + 2 * i, 3, "%02x", digest[i]);
        }
        const bool steam = name == "Sacred.exe" &&
            std::string_view(hex) == "4df6659352282a0e57bdf69d2ac33396200fd0b54670d327cb9822ffdc4891cd";
        if (spec.sha256 != hex && !steam) {
            return name + " is not the supported version (Sacred Gold 2.28, English GOG/Steam).";
        }
    }
    return "";
}

std::string choose_folder(const std::string& start, const std::string& title) {
#ifdef __APPLE__
    (void)start;
    const std::string command =
        "osascript -e 'POSIX path of (choose folder with prompt \"" + title + "\")' 2>/dev/null";
#else
    const std::string command = "(zenity --file-selection --directory --title=" + shell_quote(title) +
                                " --filename=" + shell_quote(start + "/") + " || kdialog --getexistingdirectory " +
                                shell_quote(start) + ") 2>/dev/null";
#endif
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return "";
    }
    char buffer[4096];
    std::string out;
    while (fgets(buffer, sizeof buffer, pipe)) {
        out += buffer;
    }
    pclose(pipe);
    out = trim(out);
    while (out.size() > 1 && out.back() == '/') {
        out.pop_back();
    }
    return out;
}

// ------------------------------------------------------------------------------------------- the options on disk
// The page's values: every key of options() and the screen size. They are written into the profile's
// Settings.cfg as soon as one changes, over whatever else the file holds.
struct Choices {
    std::map<std::string, std::string> values;
    ScreenSize size;
};
Choices read_choices(const SettingsFile& file, const Config& config) {
    Choices choices;
    for (const auto& option : options()) {
        choices.values[option.key] = file.get(option.key);
    }
    choices.size = read_screen_size(file, config.fit);
    return choices;
}

// A first run seeds Settings.cfg as the sacred-gold script does: the installation's file with FULLSCREEN 0, SOUND 1
// and SHOWMOVIE 1, or a minimal one. False when there is no file yet and no installation to seed it from.
bool seed_settings(const fs::path& settings, const std::string& game, bool game_ok) {
    if (fs::exists(settings)) {
        return true;
    }
    if (!game_ok) {
        return false;
    }
    std::error_code status;
    fs::create_directories(settings.parent_path(), status);
    SettingsFile file;
    if (!file.load(fs::path(game) / "Settings.cfg")) {
        file.set("LANGUAGE", "US");
    }
    file.set("FULLSCREEN", "0");
    file.set("SOUND", "1");
    file.set("SHOWMOVIE", "1");
    return file.save(settings);
}

// Writes the page's values into Settings.cfg. MENU_BACKGROUNDS points at the bundle's wide menu pictures whenever the
// screen is wider than the original (unless the player named another directory there).
bool write_choices(const fs::path& settings, const Choices& choices, const fs::path& pictures) {
    SettingsFile file;
    file.load(settings);
    for (const auto& entry : choices.values) {
        file.set(entry.first, entry.second);
    }
    write_screen_size(file, choices.size);
    const std::string current = file.get("MENU_BACKGROUNDS");
    const bool original = choices.size.width == 0 && !choices.size.fit;
    if (fs::exists(pictures) && (current.empty() || current == pictures.string())) {
        file.set("MENU_BACKGROUNDS", original ? "" : pictures.string());
    }
    return file.save(settings);
}

// ------------------------------------------------------------------------------------------------------- the build
struct Build {
    std::mutex lock;
    std::string step = "", error, module;
    int done = 0, total = 0;
    std::atomic<bool> running{false}, finished{false}, failed{false};
    std::thread worker;

    void start(const std::string& command) {
        running = true;
        worker = std::thread([this, command] {
            FILE* pipe = popen((command + " 2>&1").c_str(), "r");
            if (!pipe) {
                fail("cannot start the build");
                return;
            }
            char buffer[8192];
            while (fgets(buffer, sizeof buffer, pipe)) {
                std::string line = trim(buffer);
                std::lock_guard<std::mutex> guard(lock);
                if (line.rfind("STEP ", 0) == 0) {
                    const size_t space = line.find(' ', 5);
                    step = line.substr(space + 1);
                } else if (line.rfind("PROGRESS ", 0) == 0) {
                    std::sscanf(line.c_str() + 9, "%d %d", &done, &total);
                } else if (line.rfind("DONE ", 0) == 0) {
                    module = line.substr(5);
                } else if (line.rfind("ERROR ", 0) == 0) {
                    error += line.substr(6) + "\n";
                }
            }
            const int status = pclose(pipe);
            std::lock_guard<std::mutex> guard(lock);
            if (status != 0 || module.empty()) {
                if (error.empty()) {
                    error = "the build stopped (status " + std::to_string(status) + ")";
                }
                failed = true;
            }
            finished = true;
            running = false;
        });
    }
    void fail(const std::string& text) {
        std::lock_guard<std::mutex> guard(lock);
        error = text;
        failed = finished = true;
        running = false;
    }
    ~Build() {
        if (worker.joinable()) {
            worker.join();
        }
    }
};

// ----------------------------------------------------------------------------------------------------------- the UI
std::u16string utf16(const std::string& text) {
    std::u16string out;
    for (size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        uint32_t code = c;
        int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        if (extra) {
            code = c & (0x3f >> extra);
        }
        for (int k = 1; k <= extra && i + k < text.size(); ++k) {
            code = code << 6 | (text[i + k] & 0x3f);
        }
        i += size_t(extra) + 1;
        out += code < 0x10000 ? char16_t(code) : u'?';
    }
    return out;
}

// The window's layout is in logical pixels (W x H); on a HiDPI display the renderer has more pixels than that, and
// text, shapes and the logo are drawn at the renderer's own resolution so that they stay sharp.
constexpr int W = 800, H = 600;
constexpr int side = 232; // the sidebar: logo, pages, Play
enum class Style { body, small, heading, title, page, button, play };
// The colours of Sacred's own menus: the slate of the Underworld menu's stone, the gold of the menu texts.
struct Palette {
    SDL_Color ink{228, 231, 224, 255}, dim{152, 163, 160, 255}, faint{102, 113, 112, 255}, gold{238, 198, 86, 255},
        bright{252, 226, 144, 255}, good{146, 208, 124, 255}, bad{236, 126, 104, 255}, warn{234, 184, 98, 255},
        line{52, 64, 66, 255}, stone{112, 128, 128, 255}, // a hovered or neutral edge
        bg_top{23, 31, 33, 255}, bg_bottom{7, 10, 11, 255}, glow{40, 72, 74, 255}, side_shade{0, 0, 0, 100},
        side_edge{84, 100, 100, 150}, panel_top{29, 37, 39, 232}, panel_bottom{20, 26, 28, 232},
        panel_edge{66, 80, 82, 255}, field_top{9, 12, 13, 255}, field_bottom{15, 19, 20, 255},
        field_edge{62, 75, 77, 255}, button_top{50, 61, 63, 255}, button_bottom{35, 43, 45, 255},
        button_hover_top{64, 77, 79, 255}, button_hover_bottom{45, 55, 57, 255}, button_edge{94, 109, 110, 255},
        off_top{40, 45, 46, 255}, off_bottom{32, 36, 37, 255}, off_edge{58, 65, 66, 255}, off_ink{108, 116, 116, 255},
        gold_top{240, 200, 90, 255}, gold_bottom{168, 118, 28, 255}, gold_hover_top{250, 216, 112, 255},
        gold_hover_bottom{190, 136, 38, 255}, gold_edge{255, 228, 150, 255}, gold_ink{34, 24, 6, 255},
        tab_top{46, 57, 59, 255}, tab_bottom{35, 43, 45, 255}, tab_edge{82, 97, 99, 255}, hover_top{34, 42, 44, 200},
        hover_bottom{28, 35, 37, 200}, hover_edge{60, 72, 74, 200}, select_top{26, 33, 35, 255},
        select_bottom{19, 24, 26, 255}, select_hover_top{39, 48, 50, 255}, select_hover_bottom{30, 37, 39, 255},
        select_edge{72, 86, 88, 255}, chip_top{41, 51, 53, 255}, chip_bottom{32, 40, 42, 255},
        chip_edge{80, 94, 96, 255}, track{30, 38, 40, 255};
} const colors;

SDL_Color mix(SDL_Color a, SDL_Color b, float t) {
    auto channel = [t](Uint8 x, Uint8 y) {
        return Uint8(std::lround(float(x) + (float(y) - float(x)) * t));
    };
    return {channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b), channel(a.a, b.a)};
}

// Shapes as signed distances (logical pixels, negative inside), drawn with one pixel of antialiasing.
float box_distance(float x, float y, float cx, float cy, float hw, float hh, float r) {
    const float qx = std::fabs(x - cx) - (hw - r), qy = std::fabs(y - cy) - (hh - r);
    const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
}
float segment_distance(float x, float y, float ax, float ay, float bx, float by, float r) {
    const float px = x - ax, py = y - ay, dx = bx - ax, dy = by - ay;
    const float t = std::clamp((px * dx + py * dy) / (dx * dx + dy * dy), 0.0f, 1.0f);
    return std::hypot(px - dx * t, py - dy * t) - r;
}
float triangle_distance(float x, float y, float ax, float ay, float bx, float by, float cx, float cy) {
    const float e[3][2] = {{bx - ax, by - ay}, {cx - bx, cy - by}, {ax - cx, ay - cy}};
    const float v[3][2] = {{x - ax, y - ay}, {x - bx, y - by}, {x - cx, y - cy}};
    const float sign = e[0][0] * e[2][1] - e[0][1] * e[2][0] > 0 ? 1.0f : -1.0f;
    float best = 1e9f, inside = 1e9f;
    for (int i = 0; i < 3; ++i) {
        const float t =
            std::clamp((v[i][0] * e[i][0] + v[i][1] * e[i][1]) / (e[i][0] * e[i][0] + e[i][1] * e[i][1]), 0.0f, 1.0f);
        const float qx = v[i][0] - e[i][0] * t, qy = v[i][1] - e[i][1] * t;
        best = std::min(best, qx * qx + qy * qy);
        inside = std::min(inside, sign * (v[i][0] * e[i][1] - v[i][1] * e[i][0]));
    }
    return inside > 0 ? -std::sqrt(best) : std::sqrt(best);
}
float coverage(float distance, float pixel) {
    return std::clamp(0.5f - distance / pixel, 0.0f, 1.0f);
}

// The small line icons, in a box of `s` logical pixels.
enum class Icon { check, cross, folder, gear, play, monitor, eye, picture, shield, left, right, reset, info, game };
float icon_distance(Icon icon, float x, float y, float s) {
    const float u = x / s, v = y / s;
    auto seg = [&](float ax, float ay, float bx, float by, float r) {
        return segment_distance(u, v, ax, ay, bx, by, r);
    };
    const float len = std::hypot(u - 0.5f, v - 0.5f);
    float d = 1e9f;
    switch (icon) {
    case Icon::check:
        d = std::min(seg(0.22f, 0.53f, 0.42f, 0.72f, 0.08f), seg(0.42f, 0.72f, 0.78f, 0.30f, 0.08f));
        break;
    case Icon::cross:
        d = std::min(seg(0.3f, 0.3f, 0.7f, 0.7f, 0.08f), seg(0.7f, 0.3f, 0.3f, 0.7f, 0.08f));
        break;
    case Icon::folder:
        d = std::min(box_distance(u, v, 0.5f, 0.57f, 0.42f, 0.27f, 0.07f),
                     box_distance(u, v, 0.30f, 0.30f, 0.22f, 0.10f, 0.06f));
        break;
    case Icon::gear: {
        float teeth = 1e9f;
        for (int k = 0; k < 8; ++k) {
            const float a = float(k) * 0.785398f;
            teeth = std::min(teeth, seg(0.5f + 0.30f * std::cos(a), 0.5f + 0.30f * std::sin(a),
                                        0.5f + 0.40f * std::cos(a), 0.5f + 0.40f * std::sin(a), 0.075f));
        }
        d = std::max(std::min(len - 0.31f, teeth), -(len - 0.13f));
        break;
    }
    case Icon::play:
        d = triangle_distance(u, v, 0.30f, 0.18f, 0.30f, 0.82f, 0.84f, 0.50f) - 0.03f;
        break;
    case Icon::monitor:
        d = std::min({std::fabs(box_distance(u, v, 0.5f, 0.42f, 0.42f, 0.29f, 0.06f)) - 0.06f,
                      seg(0.5f, 0.72f, 0.5f, 0.84f, 0.06f), seg(0.30f, 0.86f, 0.70f, 0.86f, 0.06f)});
        break;
    case Icon::eye: {
        const float lens = std::max(std::hypot(u - 0.5f, v - 1.02f) - 0.66f, std::hypot(u - 0.5f, v + 0.02f) - 0.66f);
        d = std::min(std::fabs(lens) - 0.055f, len - 0.13f);
        break;
    }
    case Icon::picture:
        d = std::min({std::fabs(box_distance(u, v, 0.5f, 0.5f, 0.42f, 0.34f, 0.07f)) - 0.06f,
                      triangle_distance(u, v, 0.18f, 0.76f, 0.42f, 0.46f, 0.62f, 0.76f),
                      triangle_distance(u, v, 0.48f, 0.76f, 0.64f, 0.56f, 0.82f, 0.76f),
                      std::hypot(u - 0.66f, v - 0.34f) - 0.08f});
        break;
    case Icon::shield: {
        const float body = std::min(box_distance(u, v, 0.5f, 0.34f, 0.34f, 0.20f, 0.05f),
                                    triangle_distance(u, v, 0.16f, 0.46f, 0.84f, 0.46f, 0.5f, 0.92f));
        d = std::min(std::fabs(body) - 0.055f, seg(0.5f, 0.26f, 0.5f, 0.66f, 0.055f));
        break;
    }
    case Icon::left:
        d = std::min(seg(0.62f, 0.22f, 0.36f, 0.5f, 0.08f), seg(0.36f, 0.5f, 0.62f, 0.78f, 0.08f));
        break;
    case Icon::right:
        d = std::min(seg(0.38f, 0.22f, 0.64f, 0.5f, 0.08f), seg(0.64f, 0.5f, 0.38f, 0.78f, 0.08f));
        break;
    case Icon::reset: {
        const float angle = std::atan2(v - 0.5f, u - 0.5f); // 0 to the right, negative upwards
        const float arc = angle > -1.75f && angle < -0.55f ? 1e9f : std::fabs(len - 0.30f) - 0.07f;
        d = std::min(arc, triangle_distance(u, v, 0.36f, 0.06f, 0.56f, 0.22f, 0.34f, 0.36f));
        break;
    }
    case Icon::info:
        d = std::min({std::fabs(len - 0.38f) - 0.06f, seg(0.5f, 0.46f, 0.5f, 0.70f, 0.065f),
                      std::hypot(u - 0.5f, v - 0.30f) - 0.07f});
        break;
    case Icon::game:
        d = std::min({std::fabs(box_distance(u, v, 0.5f, 0.55f, 0.44f, 0.24f, 0.2f)) - 0.06f,
                      seg(0.25f, 0.55f, 0.39f, 0.55f, 0.05f), seg(0.32f, 0.48f, 0.32f, 0.62f, 0.05f),
                      std::hypot(u - 0.64f, v - 0.50f) - 0.05f, std::hypot(u - 0.74f, v - 0.60f) - 0.05f});
        break;
    }
    return d * s;
}

class Ui {
public:
    // `name_font`: the file of the game's Carolingia (the font of Sacred's menus) for the project's name, or "" for
    // Arial.
    Ui(SDL_Renderer* renderer, float scale, const std::string& name_font)
        : renderer_(renderer), scale_(scale), body_("Arial", px(15), 400, false), small_("Arial", px(13), 400, false),
          heading_("Arial", px(15), 700, false),
          title_(name_font.empty() ? "Arial" : "Carolingia", px(name_font.empty() ? 24 : 29),
                 name_font.empty() ? 700 : 400, false,
                 name_font.empty() ? std::vector<std::string>{} : std::vector<std::string>{name_font}),
          page_("Arial", px(22), 700, false), button_("Arial", px(15), 700, false), play_("Arial", px(20), 700, false) {
    }
    ~Ui() {
        for (auto& entry : cache_) {
            SDL_DestroyTexture(entry.second.texture);
        }
        for (auto& entry : shapes_) {
            SDL_DestroyTexture(entry.second);
        }
    }
    struct Text {
        SDL_Texture* texture;
        int w, h;     // logical
        float fw, fh; // logical, exact
    };

    // Mouse state of this frame, in logical coordinates; `clip` limits where clicks count (the scrolled list).
    int mx = -1, my = -1;
    bool clicked = false;
    SDL_Rect clip{0, 0, W, H};

    const Text& text(const std::string& value, Style style = Style::body) {
        const std::string key = char('0' + int(style)) + value;
        auto found = cache_.find(key);
        if (found != cache_.end()) {
            return found->second;
        }
        if (cache_.size() > 600) {
            for (auto& entry : cache_) {
                SDL_DestroyTexture(entry.second.texture);
            }
            cache_.clear();
        }
        const sacred::Font& font = style == Style::title     ? title_
                                   : style == Style::page    ? page_
                                   : style == Style::heading ? heading_
                                   : style == Style::small   ? small_
                                   : style == Style::button  ? button_
                                   : style == Style::play    ? play_
                                                             : body_;
        const sacred::TextRun run = font.render(utf16(value.empty() ? " " : value));
        const int rw = std::max(1, run.width), rh = std::max(1, run.height);
        SDL_Texture* texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, rw, rh);
        std::vector<uint32_t> pixels(size_t(rw) * size_t(rh), 0);
        for (size_t i = 0; i < run.alpha.size(); ++i) {
            pixels[i] = uint32_t(run.alpha[i]) << 24 | 0xffffff;
        }
        SDL_UpdateTexture(texture, nullptr, pixels.data(), rw * 4);
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        const float fw = float(rw) / scale_, fh = float(rh) / scale_;
        return cache_[key] = Text{texture, int(std::ceil(fw)), int(std::ceil(fh)), fw, fh};
    }
    // Long texts lose their start ("...") to fit `max_width`; paths keep their informative end that way.
    std::string fit(const std::string& value, Style style, int max_width) {
        std::string shown = value;
        while (max_width > 0 && shown.size() > 4 && text(shown, style).w > max_width) {
            shown = "..." + shown.substr(4);
        }
        return shown;
    }
    int draw_text(const std::string& value, int x, int y, SDL_Color color, Style style = Style::body,
                  int max_width = 0) {
        const Text& t = text(fit(value, style, max_width), style);
        SDL_SetTextureColorMod(t.texture, color.r, color.g, color.b);
        SDL_SetTextureAlphaMod(t.texture, color.a);
        const SDL_FRect rect{float(x), float(y), t.fw, t.fh};
        SDL_RenderCopyF(renderer_, t.texture, nullptr, &rect);
        return t.h;
    }
    void draw_centered(const std::string& value, SDL_Rect rect, SDL_Color color, Style style) {
        const Text& t = text(value, style);
        draw_text(value, rect.x + (rect.w - t.w) / 2, rect.y + (rect.h - t.h) / 2, color, style);
    }
    // Words of `value` broken into lines no wider than `width`.
    std::vector<std::string> wrap(const std::string& value, int width, Style style) {
        std::vector<std::string> lines;
        std::istringstream words(value);
        std::string line;
        for (std::string word; words >> word;) {
            const std::string longer = line.empty() ? word : line + " " + word;
            if (!line.empty() && text(longer, style).w > width) {
                lines.push_back(line);
                line = word;
            } else {
                line = longer;
            }
        }
        if (!line.empty()) {
            lines.push_back(line);
        }
        return lines;
    }
    int draw_wrapped(const std::string& value, int x, int y, int width, SDL_Color color, Style style, int step) {
        int lines = 0;
        for (const auto& line : wrap(value, width, style)) {
            draw_text(line, x, y + step * lines++, color, style);
        }
        return lines * step;
    }
    void fill(SDL_Rect rect, SDL_Color color) {
        SDL_SetRenderDrawColor(renderer_, color.r, color.g, color.b, color.a);
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_RenderFillRect(renderer_, &rect);
    }

    // A rounded rectangle with a vertical gradient and a border of `border_width` logical pixels.
    void panel(SDL_Rect rect, int radius, SDL_Color top, SDL_Color bottom, SDL_Color border, float border_width = 1) {
        char key[160];
        std::snprintf(key, sizeof key, "panel %d %d %d %08x %08x %08x %.2f", rect.w, rect.h, radius, pack(top),
                      pack(bottom), pack(border), double(border_width));
        const float w = float(rect.w), h = float(rect.h), r = float(radius);
        draw_shape(key, rect, [&](float x, float y, float pixel) {
            const float d = box_distance(x, y, w / 2, h / 2, w / 2, h / 2, r);
            const float outer = coverage(d, pixel),
                        inner = border_width > 0 ? coverage(d + border_width, pixel) : outer;
            return blend(mix(top, bottom, std::clamp(y / h, 0.0f, 1.0f)), inner, border, outer - inner);
        });
    }
    void icon(Icon which, int x, int y, int size, SDL_Color color) {
        char key[64];
        std::snprintf(key, sizeof key, "icon %d %d", int(which), size);
        const float s = float(size);
        SDL_Texture* texture = shape(key, size, size, [&](float px, float py, float pixel) {
            return Rgba{1, 1, 1, coverage(icon_distance(which, px, py, s), pixel)};
        });
        SDL_SetTextureColorMod(texture, color.r, color.g, color.b);
        SDL_SetTextureAlphaMod(texture, color.a);
        const SDL_FRect rect{float(x), float(y), s, s};
        SDL_RenderCopyF(renderer_, texture, nullptr, &rect);
    }
    // The full-window backdrop: a dark warm gradient with a faint glow behind the logo.
    void backdrop() {
        draw_shape("backdrop", {0, 0, W, H}, [](float x, float y, float) {
            const float glow = std::max(0.0f, 1.0f - std::hypot(x - 116.0f, (y - 100.0f) * 1.2f) / 420.0f);
            const float fall = std::max(0.0f, 1.0f - std::hypot(x - 520.0f, y - 40.0f) / 700.0f);
            SDL_Color base = mix(colors.bg_top, colors.bg_bottom, y / float(H));
            base = mix(base, colors.glow, 0.30f * glow * glow + 0.10f * fall * fall);
            return Rgba{float(base.r) / 255, float(base.g) / 255, float(base.b) / 255, 1};
        });
    }
    // A thin line fading out at both ends, with a small diamond in the middle when `diamond`.
    void divider(int x, int y, int w, SDL_Color color, bool diamond) {
        char key[64];
        std::snprintf(key, sizeof key, "divider %d %d %08x", w, int(diamond), pack(color));
        const float fw = float(w);
        draw_shape(key, {x, y - 5, w, 11}, [&](float px, float py, float pixel) {
            const float along = std::sin(3.14159f * std::clamp(px / fw, 0.0f, 1.0f));
            float a = coverage(std::fabs(py - 5.5f) - 0.5f, pixel) * along;
            if (diamond) {
                a = std::max(a, coverage(std::fabs(px - fw / 2) + std::fabs(py - 5.5f) - 4.0f, pixel));
            }
            return Rgba{float(color.r) / 255, float(color.g) / 255, float(color.b) / 255, a * float(color.a) / 255};
        });
    }
    // The project's logo, scaled down to `size` logical pixels by averaging (no shimmering edges).
    void logo(const sacred::IconImage& image, int x, int y, int size, Uint8 alpha = 255) {
        if (image.rgba.empty()) {
            return;
        }
        const std::string key = "logo " + std::to_string(size);
        auto found = shapes_.find(key);
        SDL_Texture* texture = found != shapes_.end() ? found->second : nullptr;
        if (!texture) {
            const int n = std::max(1, int(std::lround(float(size) * scale_)));
            std::vector<uint32_t> pixels(size_t(n) * size_t(n));
            const float step = float(image.width) / float(n);
            for (int ty = 0; ty < n; ++ty) {
                for (int tx = 0; tx < n; ++tx) {
                    float sum[4] = {0, 0, 0, 0};
                    int count = 0;
                    const int x0 = int(float(tx) * step), x1 = std::max(x0 + 1, int(float(tx + 1) * step));
                    const int y0 = int(float(ty) * step), y1 = std::max(y0 + 1, int(float(ty + 1) * step));
                    for (int sy = y0; sy < y1 && sy < image.height; ++sy) {
                        for (int sx = x0; sx < x1 && sx < image.width; ++sx) {
                            const uint8_t* p = &image.rgba[(size_t(sy) * size_t(image.width) + size_t(sx)) * 4];
                            const float a = float(p[3]) / 255;
                            sum[0] += float(p[0]) * a, sum[1] += float(p[1]) * a, sum[2] += float(p[2]) * a;
                            sum[3] += a;
                            ++count;
                        }
                    }
                    const float a = sum[3] / float(std::max(1, count));
                    auto c = [&](float v) {
                        return uint32_t(std::lround(sum[3] > 0 ? v / sum[3] : 0));
                    };
                    pixels[size_t(ty) * size_t(n) + size_t(tx)] =
                        uint32_t(std::lround(a * 255)) << 24 | c(sum[0]) << 16 | c(sum[1]) << 8 | c(sum[2]);
                }
            }
            texture = upload(pixels, n, n);
            shapes_[key] = texture;
        }
        SDL_SetTextureAlphaMod(texture, alpha);
        const SDL_FRect rect{float(x), float(y), float(size), float(size)};
        SDL_RenderCopyF(renderer_, texture, nullptr, &rect);
    }

    bool inside(SDL_Rect rect) const {
        return mx >= rect.x && mx < rect.x + rect.w && my >= rect.y && my < rect.y + rect.h && mx >= clip.x &&
               mx < clip.x + clip.w && my >= clip.y && my < clip.y + clip.h;
    }
    enum class Kind { normal, primary, play };
    // A button; true when it was clicked this frame.
    bool button(SDL_Rect rect, const std::string& label, bool enabled, Kind kind = Kind::normal,
                const Icon* glyph = nullptr) {
        const bool hover = enabled && inside(rect);
        const bool gold = kind != Kind::normal;
        SDL_Color top, bottom, border, ink;
        if (!enabled) {
            top = colors.off_top, bottom = colors.off_bottom, border = colors.off_edge, ink = colors.off_ink;
        } else if (gold) {
            top = hover ? colors.gold_hover_top : colors.gold_top;
            bottom = hover ? colors.gold_hover_bottom : colors.gold_bottom;
            border = colors.gold_edge, ink = colors.gold_ink;
        } else {
            top = hover ? colors.button_hover_top : colors.button_top;
            bottom = hover ? colors.button_hover_bottom : colors.button_bottom;
            border = hover ? colors.stone : colors.button_edge, ink = colors.ink;
        }
        panel(rect, kind == Kind::play ? 10 : 7, top, bottom, border);
        const Style style = kind == Kind::play ? Style::play : Style::button;
        const Text& t = text(label, style);
        const int icon_size = kind == Kind::play ? 20 : 16, gap = glyph ? icon_size + 8 : 0;
        const int x = rect.x + (rect.w - t.w - gap) / 2;
        if (glyph) {
            icon(*glyph, x, rect.y + (rect.h - icon_size) / 2, icon_size, ink);
        }
        draw_text(label, x + gap, rect.y + (rect.h - t.h) / 2, ink, style);
        return hover && clicked;
    }
    // A page of the sidebar; true when it was clicked this frame.
    bool tab(SDL_Rect rect, const std::string& label, Icon glyph, bool active, bool enabled) {
        const bool hover = enabled && inside(rect);
        if (active) {
            panel(rect, 8, colors.tab_top, colors.tab_bottom, colors.tab_edge);
            panel({rect.x + 1, rect.y + 8, 3, rect.h - 16}, 1, colors.gold, colors.gold, colors.gold, 0);
        } else if (hover) {
            panel(rect, 8, colors.hover_top, colors.hover_bottom, colors.hover_edge);
        }
        const SDL_Color ink = active ? colors.bright : enabled ? (hover ? colors.ink : colors.dim) : colors.faint;
        icon(glyph, rect.x + 16, rect.y + (rect.h - 18) / 2, 18, active ? colors.gold : ink);
        draw_text(label, rect.x + 46, rect.y + (rect.h - text(label, Style::button).h) / 2, ink, Style::button);
        return hover && clicked;
    }
    // "<  value  >": a click on the left third steps back, anywhere else forward. Returns -1, 0 or +1.
    int selector(SDL_Rect rect, const std::string& label, bool enabled, bool changed_from_default) {
        const bool hover = enabled && inside(rect);
        panel(rect, rect.h / 2, hover ? colors.select_hover_top : colors.select_top,
              hover ? colors.select_hover_bottom : colors.select_bottom,
              changed_from_default ? colors.gold
              : hover              ? colors.stone
                                   : colors.select_edge);
        const SDL_Color arrow = enabled ? (hover ? colors.bright : colors.gold) : colors.faint;
        icon(Icon::left, rect.x + 8, rect.y + (rect.h - 16) / 2, 16, arrow);
        icon(Icon::right, rect.x + rect.w - 24, rect.y + (rect.h - 16) / 2, 16, arrow);
        const std::string shown = fit(label, Style::body, rect.w - 56);
        const Text& t = text(shown);
        draw_text(shown, rect.x + (rect.w - t.w) / 2, rect.y + (rect.h - t.h) / 2,
                  !enabled               ? colors.faint
                  : changed_from_default ? colors.bright
                                         : colors.ink);
        if (!hover || !clicked) {
            return 0;
        }
        return mx < rect.x + rect.w / 3 ? -1 : 1;
    }
    // A rounded label; returns its width.
    int chip(const std::string& label, int x, int y) {
        const Text& t = text(label, Style::small);
        const int w = t.w + 22;
        panel({x, y, w, 26}, 13, colors.chip_top, colors.chip_bottom, colors.chip_edge);
        draw_text(label, x + 11, y + (26 - t.h) / 2, colors.ink, Style::small);
        return w;
    }
    // A round status mark: a check on green or a cross on red.
    void status(bool good, int x, int y) {
        panel({x, y, 18, 18}, 9, good ? SDL_Color{120, 190, 100, 255} : SDL_Color{220, 110, 90, 255},
              good ? SDL_Color{88, 150, 72, 255} : SDL_Color{180, 80, 64, 255},
              good ? SDL_Color{170, 230, 150, 255} : SDL_Color{250, 160, 140, 255});
        icon(good ? Icon::check : Icon::cross, x + 1, y + 1, 16, {24, 20, 16, 255});
    }

private:
    struct Rgba {
        float r, g, b, a;
    };
    static uint32_t pack(SDL_Color c) { return uint32_t(c.r) << 24 | uint32_t(c.g) << 16 | uint32_t(c.b) << 8 | c.a; }
    // `fill` over `fill_share` of the pixel and `edge` over `edge_share`, as straight alpha.
    static Rgba blend(SDL_Color fill, float fill_share, SDL_Color edge, float edge_share) {
        const float fa = fill_share * float(fill.a) / 255, ea = edge_share * float(edge.a) / 255, a = fa + ea;
        if (a <= 0) {
            return {0, 0, 0, 0};
        }
        auto channel = [&](Uint8 f, Uint8 e) {
            return (float(f) * fa + float(e) * ea) / (255 * a);
        };
        return {channel(fill.r, edge.r), channel(fill.g, edge.g), channel(fill.b, edge.b), std::min(1.0f, a)};
    }
    int px(int logical) const { return -std::max(1, int(std::lround(float(logical) * scale_))); }
    SDL_Texture* upload(const std::vector<uint32_t>& pixels, int w, int h) {
        SDL_Texture* texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
        SDL_UpdateTexture(texture, nullptr, pixels.data(), w * 4);
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        return texture;
    }
    // A cached texture of w x h logical pixels at the renderer's resolution; `paint` gives the colour at a logical
    // point and the size of one pixel there.
    SDL_Texture* shape(const std::string& key, int w, int h, const std::function<Rgba(float, float, float)>& paint) {
        auto found = shapes_.find(key);
        if (found != shapes_.end()) {
            return found->second;
        }
        const int pw = std::max(1, int(std::lround(float(w) * scale_))),
                  ph = std::max(1, int(std::lround(float(h) * scale_)));
        const float pixel = float(w) / float(pw);
        std::vector<uint32_t> pixels(size_t(pw) * size_t(ph));
        for (int y = 0; y < ph; ++y) {
            for (int x = 0; x < pw; ++x) {
                const Rgba c = paint((float(x) + 0.5f) * pixel, (float(y) + 0.5f) * pixel, pixel);
                auto byte = [](float v) {
                    return uint32_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255));
                };
                pixels[size_t(y) * size_t(pw) + size_t(x)] =
                    byte(c.a) << 24 | byte(c.r) << 16 | byte(c.g) << 8 | byte(c.b);
            }
        }
        return shapes_[key] = upload(pixels, pw, ph);
    }
    void draw_shape(const std::string& key, SDL_Rect rect, const std::function<Rgba(float, float, float)>& paint) {
        SDL_Texture* texture = shape(key, rect.w, rect.h, paint);
        SDL_SetTextureAlphaMod(texture, 255);
        const SDL_FRect target{float(rect.x), float(rect.y), float(rect.w), float(rect.h)};
        SDL_RenderCopyF(renderer_, texture, nullptr, &target);
    }

    SDL_Renderer* renderer_;
    float scale_;
    sacred::Font body_, small_, heading_, title_, page_, button_, play_;
    std::map<std::string, Text> cache_;
    std::map<std::string, SDL_Texture*> shapes_;
};

int step(int index, int delta, int count) {
    return count ? ((index + delta) % count + count) % count : 0;
}

// The summary of the settings page as short labels ("Fullscreen", "render 2x", ...), first letters raised.
std::vector<std::string> summary_items(const std::string& text) {
    std::vector<std::string> items;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find(", ", start);
        std::string item = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!item.empty()) {
            item[0] = char(std::toupper(static_cast<unsigned char>(item[0])));
            items.push_back(item == "FPS at the display's refresh rate" ? "FPS at the display's rate" : item);
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 2;
    }
    return items;
}
}

// The game's Carolingia (the font of its menus) in the installation, "" when it has none: only the project's name
// is written with it, everything else with Arial.
static std::string carolingia(const std::string& game) {
    for (const char* name : {"font/CAROLING.TTF", "font/Caroling.ttf", "font/caroling.ttf", "FONT/CAROLING.TTF"}) {
        if (!game.empty() && fs::is_regular_file(fs::path(game) / name)) {
            return (fs::path(game) / name).string();
        }
    }
    return "";
}

int main(int, char** argv) {
    const fs::path here = executable_dir(argv[0]), resources = resource_dir(here);
    const fs::path source =
        env("SACRED_LAUNCHER_ROOT").empty() ? resources / "source" : fs::path(env("SACRED_LAUNCHER_ROOT"));
    const std::string python = !env("SACRED_LAUNCHER_PYTHON").empty() ? env("SACRED_LAUNCHER_PYTHON")
                               : fs::exists(resources / "python/bin/python3")
                                   ? (resources / "python/bin/python3").string()
                                   : "python3";
    // Compilers in the order build_game.py tries them: the system's first, the bundled Zig after it.
    std::string compilers;
    if (!env("SACRED_LAUNCHER_CC").empty()) {
        compilers = " --cc " + shell_quote(env("SACRED_LAUNCHER_CC"));
    } else {
        compilers = " --cc " + shell_quote(env("CC").empty() ? "cc" : env("CC"));
        if (fs::exists(resources / "zig/zig")) {
            compilers += " --cc " + shell_quote(shell_quote((resources / "zig/zig").string()) + " cc");
        }
    }
    const fs::path script =
        !env("SACRED_LAUNCHER_GAME").empty() ? fs::path(env("SACRED_LAUNCHER_GAME")) : here / "sacred-gold";
    const fs::path pictures = resources / "menu-backgrounds";

    sacred::set_app_identity();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "sacred-launcher: %s\n", SDL_GetError());
        return 1;
    }
    const fs::path profile = profile_dir();
    const fs::path module_dir = profile.parent_path() / "game-module";
    const fs::path settings = profile / "Settings.cfg";
    Config config = load_config(profile);
    if (config.game.empty() || !fs::exists(fs::path(config.game) / "Sacred.exe")) {
        for (const auto& candidate : candidates()) {
            if (fs::exists(fs::path(candidate) / "Sacred.exe")) {
                config.game = candidate;
                break;
            }
        }
    }
    std::string problem = check_installation(config.game);
    Choices choices;
    {
        SettingsFile file;
        file.load(settings);
        choices = read_choices(file, config);
        // A profile the launcher has not seen yet starts with its defaults, except for keys the file already sets.
        if (!fs::exists(profile / "launcher.cfg")) {
            for (const auto& option : options()) {
                if (choices.values[option.key].empty()) {
                    choices.values[option.key] = option.recommended;
                }
            }
        }
    }
    SDL_DisplayMode display{};
    if (SDL_GetDesktopDisplayMode(0, &display) != 0) {
        display.w = display.h = 0;
    }

    SDL_Window* window = SDL_CreateWindow("Sacred Native", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H,
                                          SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC) : nullptr;
    if (!renderer) {
        renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE) : nullptr;
    }
    if (!renderer) {
        std::fprintf(stderr, "sacred-launcher: %s\n", SDL_GetError());
        return 1;
    }
    SDL_RenderSetLogicalSize(renderer, W, H);
    float scale = 1;
    {
        int ow = W, oh = H;
        SDL_GetRendererOutputSize(renderer, &ow, &oh);
        scale = std::clamp(std::min(float(ow) / float(W), float(oh) / float(H)), 1.0f, 4.0f);
    }
    // The project's logo: the window's icon (on macOS SDL also shows it in the Dock) and the sidebar's emblem. In a
    // build tree it comes from the source's data/icons.
    sacred::IconImage logo;
    if (sacred::load_app_icon(logo) || sacred::load_png_icon(source / "data/icons/sacred-native-512.png", logo)) {
        sacred::set_window_icon(window, logo);
    }
    std::unique_ptr<Ui> ui;
    std::string name_font; // the font file the UI was made with
    try {
        ui = std::make_unique<Ui>(renderer, scale, name_font = carolingia(config.game));
    } catch (const std::exception& error) {
        std::fprintf(stderr, "sacred-launcher: no usable font: %s\n", error.what());
        return 1;
    }

    // Every change is saved at once; before the first start (no Settings.cfg and no installation to seed it from)
    // it waits for Play.
    std::string saved_note;
    auto persist = [&] {
        save_config(profile, config);
        if (!seed_settings(settings, config.game, problem.empty())) {
            saved_note = "Saved when you first press Play.";
            return;
        }
        saved_note = write_choices(settings, choices, pictures) ? "Saved to " + settings.string()
                                                                : "Cannot write " + settings.string();
    };
    saved_note = fs::exists(settings) ? "Saved to " + settings.string() : "Saved when you first press Play.";

    Build build;
    enum class Page { play, settings } page = Page::play;
    bool editing = false, launch = false, quit = false;
    int scroll = 0, content_height = 0;
    std::string typed = config.game;
    // For checks without a person: SACRED_LAUNCHER_PAGE=settings opens that page, SACRED_LAUNCHER_SCROLL=N scrolls
    // it, SACRED_LAUNCHER_CLICKS="X,Y X,Y ..." clicks there (window coordinates, one per frame from the third) and
    // SACRED_LAUNCHER_SNAPSHOT=FILE.bmp saves the window after them and quits.
    if (env("SACRED_LAUNCHER_PAGE") == "settings") {
        page = Page::settings;
    }
    const std::string snapshot = env("SACRED_LAUNCHER_SNAPSHOT");
    if (!env("SACRED_LAUNCHER_SCROLL").empty()) {
        scroll = std::atoi(env("SACRED_LAUNCHER_SCROLL").c_str());
    }
    std::vector<std::pair<int, int>> scripted;
    {
        std::istringstream clicks(env("SACRED_LAUNCHER_CLICKS"));
        for (std::string click; clicks >> click;) {
            if (const size_t comma = click.find(','); comma != std::string::npos) {
                scripted.push_back({std::atoi(click.c_str()), std::atoi(click.c_str() + comma + 1)});
            }
        }
    }
    int frames = 0;
    std::string ui_game = config.game;
    const int left = side + 24, right = W - 24;   // the page's content
    const SDL_Rect list{side, 88, W - side, 446}; // the settings page's scrolled area
    while (!quit && !launch) {
        ui->clicked = false;
        int wheel = 0;
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                quit = true;
            } else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
                ui->clicked = true;
            } else if (event.type == SDL_MOUSEWHEEL) {
                wheel += event.wheel.y;
            } else if (editing && event.type == SDL_TEXTINPUT) {
                typed += event.text.text;
            } else if (editing && event.type == SDL_KEYDOWN) {
                if (event.key.keysym.sym == SDLK_BACKSPACE && !typed.empty()) {
                    do {
                        typed.pop_back();
                    } while (!typed.empty() && (typed.back() & 0xc0) == 0x80);
                } else if (event.key.keysym.sym == SDLK_RETURN || event.key.keysym.sym == SDLK_KP_ENTER) {
                    editing = false;
                    SDL_StopTextInput();
                    config.game = trim(typed);
                    problem = check_installation(config.game);
                } else if (event.key.keysym.sym == SDLK_ESCAPE) {
                    editing = false;
                    SDL_StopTextInput();
                    typed = config.game;
                }
            } else if (!editing && event.type == SDL_KEYDOWN && page == Page::settings) {
                if (event.key.keysym.sym == SDLK_DOWN || event.key.keysym.sym == SDLK_PAGEDOWN) {
                    wheel -= 3;
                } else if (event.key.keysym.sym == SDLK_UP || event.key.keysym.sym == SDLK_PAGEUP) {
                    wheel += 3;
                } else if (event.key.keysym.sym == SDLK_ESCAPE) {
                    page = Page::play;
                }
            }
        }
        { // the logical size may differ from the window's pixels (HiDPI)
            int mx, my, ww, wh;
            SDL_GetMouseState(&mx, &my);
            SDL_GetWindowSize(window, &ww, &wh);
            ui->mx = int(float(mx) * float(W) / float(std::max(1, ww)));
            ui->my = int(float(my) * float(H) / float(std::max(1, wh)));
        }
        if (frames >= 2 && size_t(frames - 2) < scripted.size()) {
            ui->mx = scripted[size_t(frames - 2)].first;
            ui->my = scripted[size_t(frames - 2)].second;
            ui->clicked = true;
        }
        if (config.game != ui_game) { // another installation: the name's font may now be there (or gone)
            ui_game = config.game;
            if (const std::string font = carolingia(config.game); font != name_font) {
                try {
                    auto remade = std::make_unique<Ui>(renderer, scale, font);
                    remade->mx = ui->mx, remade->my = ui->my, remade->clicked = ui->clicked;
                    ui = std::move(remade);
                    name_font = font;
                } catch (const std::exception&) {
                } // keep the current one
            }
        }
        ui->clip = {0, 0, W, H};
        const bool busy = build.running;
        ui->backdrop();

        // ------------------------------------------------------------------------------------------- the sidebar
        ui->fill({0, 0, side, H}, colors.side_shade);
        ui->fill({side - 1, 0, 1, H}, colors.side_edge);
        ui->logo(logo, (side - 132) / 2, 28, 132);
        ui->draw_centered("Sacred Native", {0, 168, side, 40}, colors.gold, Style::title);
        ui->divider(24, 226, side - 48, colors.stone, true);
        if (ui->tab({16, 246, side - 32, 40}, "Game", Icon::game, page == Page::play, true)) {
            page = Page::play;
        }
        if (ui->tab({16, 292, side - 32, 40}, "Settings", Icon::gear, page == Page::settings, !busy)) {
            page = Page::settings;
            editing = false;
            SDL_StopTextInput();
        }

        if (page == Page::play) {
            // ------------------------------------------------------------------------------- the game page
            ui->draw_text(problem.empty() ? "Ready to play" : "Find your Sacred Gold", left, 26, colors.gold,
                          Style::page);
            ui->draw_text(problem.empty() ? "Your installation is checked; Play starts the game."
                                          : "Sacred Native runs your own copy of Sacred Gold 2.28.",
                          left, 58, colors.dim, Style::small);

            const int status_lines =
                problem.empty() ? 1 : int(ui->wrap(problem, right - left - 50, Style::body).size());
            SDL_Rect card{left - 8, 92, right - left + 16, 112 + 20 * status_lines};
            ui->panel(card, 12, colors.panel_top, colors.panel_bottom, colors.panel_edge);
            ui->icon(Icon::folder, left + 8, card.y + 16, 18, colors.gold);
            ui->draw_text("Game folder", left + 34, card.y + 16, colors.ink, Style::heading);
            ui->draw_text("only ever read", right - 8 - ui->text("only ever read", Style::small).w, card.y + 18,
                          colors.faint, Style::small);
            SDL_Rect field{left + 8, card.y + 46, right - left - 140, 34};
            const bool field_hover = !busy && ui->inside(field);
            ui->panel(field, 7, colors.field_top, colors.field_bottom,
                      editing       ? colors.gold
                      : field_hover ? colors.stone
                                    : colors.field_edge);
            ui->draw_text(editing ? typed + "|" : (config.game.empty() ? "(none)" : config.game), field.x + 12,
                          field.y + 8, config.game.empty() && !editing ? colors.faint : colors.ink, Style::body,
                          field.w - 24);
            if (!busy && ui->clicked && ui->inside(field) && !editing) {
                editing = true;
                typed = config.game;
                SDL_StartTextInput();
            }
            const Icon folder = Icon::folder;
            if (ui->button({right - 8 - 116, field.y, 116, 34}, "Browse", !busy, Ui::Kind::normal, &folder)) {
                const std::string chosen =
                    choose_folder(config.game.empty() ? env("HOME") : config.game, "Sacred Gold folder");
                if (!chosen.empty()) {
                    config.game = typed = chosen;
                    problem = check_installation(config.game);
                }
                editing = false;
                SDL_StopTextInput();
            }
            ui->status(problem.empty(), left + 8, card.y + 96);
            ui->draw_wrapped(problem.empty() ? "Sacred Gold 2.28 found" : problem, left + 34, card.y + 96,
                             right - left - 50, problem.empty() ? colors.good : colors.bad, Style::body, 20);

            card = {left - 8, card.y + card.h + 14, right - left + 16, 0};
            SettingsFile shown;
            for (const auto& entry : choices.values) {
                shown.set(entry.first, entry.second);
            }
            const std::vector<std::string> items = summary_items(summary(shown, choices.size, config.fullscreen));
            { // the chips' height first, for the card behind them
                int x = left + 8, rows = 1;
                for (const auto& item : items) {
                    const int w = ui->text(item, Style::small).w + 22;
                    if (x + w > right - 8) {
                        x = left + 8, ++rows;
                    }
                    x += w + 8;
                }
                card.h = 52 + rows * 34 + 40;
            }
            ui->panel(card, 12, colors.panel_top, colors.panel_bottom, colors.panel_edge);
            ui->icon(Icon::gear, left + 8, card.y + 16, 18, colors.gold);
            ui->draw_text("Settings", left + 34, card.y + 16, colors.ink, Style::heading);
            if (ui->button({right - 8 - 96, card.y + 11, 96, 28}, "Change", !busy)) {
                page = Page::settings;
            }
            int x = left + 8, y = card.y + 52;
            for (const auto& item : items) {
                const int w = ui->text(item, Style::small).w + 22;
                if (x + w > right - 8) {
                    x = left + 8, y += 34;
                }
                x += ui->chip(item, x, y) + 8;
            }
            y += 42;
            ui->fill({left + 8, y - 8, right - left - 16, 1}, colors.line);
            ui->draw_text("Settings and saves:", left + 8, y, colors.faint, Style::small);
            const int label_w = ui->text("Settings and saves:", Style::small).w + 6;
            ui->draw_text(profile.string(), left + 8 + label_w, y, colors.dim, Style::small,
                          right - left - 16 - label_w);

            y = card.y + card.h + 14;
            if (busy || build.finished) {
                std::lock_guard<std::mutex> guard(build.lock);
                SDL_Rect progress{left - 8, y, right - left + 16, build.failed ? 120 : 92};
                ui->panel(progress, 12, colors.panel_top, colors.panel_bottom,
                          build.failed ? SDL_Color{150, 70, 56, 255} : colors.panel_edge);
                ui->draw_text(build.failed         ? "The build failed:"
                              : build.step.empty() ? "Starting..."
                                                   : build.step,
                              left + 8, y + 14, build.failed ? colors.bad : colors.ink, Style::heading);
                if (!build.failed) {
                    if (build.total) {
                        const std::string count = std::to_string(build.done) + " / " + std::to_string(build.total);
                        ui->draw_text(count, right - 8 - ui->text(count, Style::small).w, y + 16, colors.dim,
                                      Style::small);
                    }
                    const SDL_Rect bar{left + 8, y + 42, right - left - 16, 12};
                    ui->panel(bar, 6, colors.field_top, colors.field_bottom, colors.field_edge);
                    const float share = build.total ? float(build.done) / float(build.total) : 0.0f;
                    const int filled = int(float(bar.w - 4) * share);
                    if (filled >= 8) {
                        ui->panel({bar.x + 2, bar.y + 2, filled, bar.h - 4}, 4, colors.gold_hover_top,
                                  colors.gold_bottom, colors.gold_hover_top, 0);
                    }
                    ui->draw_text("The first start translates the game from your files: a few minutes, once.", left + 8,
                                  y + 64, colors.faint, Style::small, right - left - 16);
                } else {
                    std::istringstream lines(build.error);
                    int line_y = y + 40;
                    for (std::string line; std::getline(lines, line) && line_y < y + 110; line_y += 18) {
                        ui->draw_text(line, left + 8, line_y, colors.bad, Style::small, right - left - 16);
                    }
                }
            } else {
                ui->icon(Icon::info, left, H - 40, 16, colors.faint);
                ui->draw_text("Alt+Enter (Option+Enter on macOS) switches between window and fullscreen in game.",
                              left + 24, H - 40, colors.faint, Style::small, right - left - 24);
            }
        } else {
            // ------------------------------------------------------------------------------ the settings page
            ui->draw_text("Settings", left, 26, colors.gold, Style::page);
            ui->draw_text("Written to the game's Settings.cfg as soon as you change them.", left, 58, colors.dim,
                          Style::small);
            const int max_scroll = std::max(0, content_height - list.h);
            scroll -= wheel * 40;
            if (content_height) {
                scroll = std::clamp(scroll, 0, max_scroll); // known from the second frame on
            }
            SDL_RenderSetClipRect(renderer, &list);
            ui->clip = list;
            const int label_x = left, control_w = 196, control_x = right - control_w, help_w = control_x - 20 - left;
            int y = list.y + 8 - scroll;
            auto heading = [&](Section section) {
                static const Icon icons[] = {Icon::monitor, Icon::eye, Icon::picture, Icon::shield};
                y += 6;
                ui->icon(icons[int(section)], label_x, y, 18, colors.gold);
                ui->draw_text(section_title(section), label_x + 26, y, colors.gold, Style::heading);
                const int after = label_x + 26 + ui->text(section_title(section), Style::heading).w + 12;
                ui->fill({after, y + 10, right - after, 1}, colors.line);
                y += 34;
            };
            // One row: its title and help on the left, its control on the right; `warning` under the help.
            auto row = [&](const std::string& title, const std::string& help, const std::string& warning,
                           const std::function<void(int)>& control) {
                ui->draw_text(title, label_x, y, colors.ink);
                int height = 22 + ui->draw_wrapped(help, label_x, y + 22, help_w, colors.dim, Style::small, 17);
                if (!warning.empty()) {
                    height +=
                        ui->draw_wrapped(warning, label_x, y + height + 2, help_w, colors.warn, Style::small, 17) + 2;
                }
                control(y + 2);
                y += std::max(height, 34) + 18;
            };
            // A row for one of options(): its choices plus the file's value when that is none of them.
            auto option_row = [&](const std::string& key, const std::string& warning = "") {
                const Option& option = *find_option(key);
                std::vector<Choice> list_of = option.choices;
                const std::string value = choices.values[key];
                size_t index = option.index_of(value);
                if (index == list_of.size()) {
                    list_of.push_back({value, value + " (from Settings.cfg)"});
                }
                row(option.title, option.help, warning, [&](int top) {
                    const bool changed = index != option.index_of(option.recommended);
                    const int delta =
                        ui->selector({control_x, top, control_w, 32}, list_of[index].label, !busy, changed);
                    if (delta) {
                        choices.values[key] = list_of[size_t(step(int(index), delta, int(list_of.size())))].value;
                        persist();
                    }
                });
            };

            heading(Section::display);
            row("Display mode",
                "Fullscreen covers the whole display with a borderless window. Alt+Enter (Option+Enter on macOS) "
                "switches while you play.",
                "", [&](int top) {
                    if (ui->selector({control_x, top, control_w, 32}, config.fullscreen ? "Fullscreen" : "Window",
                                     !busy, config.fullscreen != default_fullscreen)) {
                        config.fullscreen = !config.fullscreen;
                        persist();
                    }
                });
            {
                const std::vector<ScreenSize> sizes = screen_sizes(display.w, display.h, choices.size);
                const size_t index = size_t(std::find(sizes.begin(), sizes.end(), choices.size) - sizes.begin());
                std::string warning;
                if (choices.size.fit && !config.fullscreen) {
                    warning = "In a window this is the original 1024 x 768; choose a size to get a larger window.";
                }
                row("Screen size",
                    "The game's own screen size. Larger sizes show more of the world around the hero; menus keep "
                    "their 1024 x 768 layout, centred, with wide pictures at the sides.",
                    warning, [&](int top) {
                        const int delta = ui->selector({control_x, top, control_w, 32}, sizes[index].label(), !busy,
                                                       !(sizes[index] == ScreenSize{0, 0, default_fit}));
                        if (delta) {
                            choices.size = sizes[size_t(step(int(index), delta, int(sizes.size())))];
                            config.fit = choices.size.fit;
                            persist();
                        }
                    });
            }
            option_row("FPS_LIMIT");
            option_row("SHOW_FPS");
            heading(Section::look);
            option_row("LEGACY_UI");
            heading(Section::graphics);
            option_row("RENDER_SCALE");
            {
                const Option& scale_option = *find_option("RENDER_SCALE");
                const Option& upscale = *find_option("TEXTURE_UPSCALE");
                const bool hidden = upscale.index_of(choices.values["TEXTURE_UPSCALE"]) != 0 &&
                                    scale_option.index_of(choices.values["RENDER_SCALE"]) == 0;
                option_row("TEXTURE_UPSCALE", hidden ? "Set the render scale to 2x or more to see them." : "");
            }
            heading(Section::gameplay);
            option_row("AUTO_PICKUP");
            option_row("GAMEPAD");
            content_height = y + scroll - list.y;
            SDL_RenderSetClipRect(renderer, nullptr);
            ui->clip = {0, 0, W, H};
            if (max_scroll > 0) { // where the list stands
                const int bar_h = std::max(30, list.h * list.h / content_height);
                const int bar_y = list.y + (list.h - bar_h) * scroll / max_scroll;
                ui->panel({W - 10, list.y + 4, 4, list.h - 8}, 2, colors.track, colors.track, colors.track, 0);
                ui->panel({W - 10, bar_y + 4, 4, bar_h - 8}, 2, colors.stone, colors.stone, colors.stone, 0);
            }
            ui->divider(side + 12, list.y + list.h + 1, W - side - 24, colors.line, false);
            const Icon reset = Icon::reset;
            if (ui->button({left, 550, 130, 34}, "Defaults", !busy, Ui::Kind::normal, &reset)) {
                SettingsFile file;
                for (const auto& entry : choices.values) {
                    file.set(entry.first, entry.second);
                }
                reset_to_defaults(file);
                config.fullscreen = default_fullscreen;
                config.fit = default_fit;
                choices = read_choices(file, config);
                persist();
            }
            { // "Saved to" stays; the path loses its start when it is too long
                const std::string prefix = "Saved to ";
                const bool path = saved_note.rfind(prefix, 0) == 0;
                const int prefix_w = path ? ui->text(prefix, Style::small).w + 4 : 0;
                if (path) {
                    ui->draw_text(prefix, left + 144, 559, colors.faint, Style::small);
                }
                ui->draw_text(path ? saved_note.substr(prefix.size()) : saved_note, left + 144 + prefix_w, 559,
                              colors.faint, Style::small, right - left - 144 - prefix_w);
            }
        }

        if (build.finished && !build.failed) {
            launch = true;
        }
        const Icon play = Icon::play;
        if (ui->button({16, H - 76, side - 32, 56}, busy ? "Building..." : "Play", !busy && problem.empty(),
                       Ui::Kind::play, busy ? nullptr : &play)) {
            page = Page::play;
            persist();
            std::error_code status;
            fs::create_directories(module_dir, status);
            build.finished = build.failed = false;
            build.error.clear();
            build.step.clear();
            build.module.clear();
            if (build.worker.joinable()) {
                build.worker.join();
            }
            // -B: no __pycache__ next to the translator or the bundled standard library (in the macOS app they are
            // covered by its signature, and the app may lie where the player cannot write).
            build.start(shell_quote(python) + " -B " + shell_quote((source / "tools/launcher/build_game.py").string()) +
                        " --assets " + shell_quote(config.game) + " --recipe " +
                        shell_quote((source / "recipe.json.gz").string()) + " --out " +
                        shell_quote(module_dir.string()) + compilers);
        }
        if (!snapshot.empty() && ++frames == 3 + int(scripted.size())) {
            int ow, oh;
            SDL_GetRendererOutputSize(renderer, &ow, &oh);
            if (SDL_Surface* shot = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888)) {
                SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, shot->pixels, shot->pitch);
                SDL_SaveBMP(shot, snapshot.c_str());
                SDL_FreeSurface(shot);
            }
            quit = true;
        }
        SDL_RenderPresent(renderer);
        SDL_Delay(busy ? 30 : 15);
    }
    std::string module_path;
    {
        std::lock_guard<std::mutex> guard(build.lock);
        module_path = build.module;
    }
    if (build.worker.joinable()) {
        build.worker.join();
    }
    ui.reset();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    if (!launch) {
        return 0;
    }

    // The Play button has written Settings.cfg (persist). "Fit the display" is the engine's automatic size in
    // fullscreen; the original size turns that off.
    if (choices.size.width == 0 && !choices.size.fit) {
        setenv("SACRED_HD_AUTO", "0", 1);
    }
    const std::string game = config.game, mode = config.fullscreen ? "--fullscreen" : "--windowed";
    const std::string profile_text = profile.string(), script_text = script.string();
    std::vector<const char*> args = {script_text.c_str(), "--profile",         profile_text.c_str(),
                                     mode.c_str(),        game.c_str(),        "--",
                                     "--game-module",     module_path.c_str(), nullptr};
    execv(script_text.c_str(), const_cast<char* const*>(args.data()));
    std::perror("sacred-launcher: cannot start sacred-gold");
    return 1;
}
