// sacred-launcher: the window a player starts. It finds (or asks for) the Sacred Gold installation, checks that it is
// the supported version, offers this project's options on a settings page (written straight into the profile's
// Settings.cfg; settings.h says what each one is), builds the game module from the player's own files the first time
// (tools/launcher/build_game.py: translator + bundled compiler, with a progress bar) and then starts the game through
// the sacred-gold script. Nothing of the game is part of the download.
//
// Layout of the bundle it expects next to itself: sacred-gold, sacred-native, lib/, source/ (translator, recipe,
// runtime headers), python/ (the interpreter the translator runs on) and zig/ (the C compiler used when the system has
// none). In a build tree the same pieces come from SACRED_LAUNCHER_ROOT / _PYTHON / _CC.
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include "platform/font.h"
#include "platform/digest.h"
#include "runtime/launcher/settings.h"
#include "runtime/process/loader.h"
#include <SDL.h>
#include <algorithm>
#include <atomic>
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
        if (spec.sha256 != hex) {
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

constexpr int W = 800, H = 600;
enum class Style { body, small, heading, title };
struct Palette {
    SDL_Color ink{235, 220, 190, 255}, dim{170, 150, 120, 255}, gold{240, 200, 120, 255}, good{140, 200, 120, 255},
        bad{235, 120, 100, 255}, warn{235, 180, 90, 255}, line{110, 90, 60, 255};
} const colors;

class Ui {
public:
    Ui(SDL_Renderer* renderer)
        : renderer_(renderer), body_("Arial", -15, 400, false), small_("Arial", -13, 400, false),
          heading_("Arial", -16, 700, false), title_("Times New Roman", -30, 700, false) {}
    ~Ui() {
        for (auto& entry : cache_) {
            SDL_DestroyTexture(entry.second.texture);
        }
    }
    struct Text {
        SDL_Texture* texture;
        int w, h;
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
                                   : style == Style::heading ? heading_
                                   : style == Style::small   ? small_
                                                             : body_;
        const sacred::TextRun run = font.render(utf16(value.empty() ? " " : value));
        SDL_Texture* texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC,
                                                 std::max(1, run.width), std::max(1, run.height));
        std::vector<uint32_t> pixels(size_t(std::max(1, run.width)) * std::max(1, run.height), 0);
        for (size_t i = 0; i < run.alpha.size(); ++i) {
            pixels[i] = uint32_t(run.alpha[i]) << 24 | 0xffffff;
        }
        SDL_UpdateTexture(texture, nullptr, pixels.data(), std::max(1, run.width) * 4);
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        return cache_[key] = Text{texture, run.width, run.height};
    }
    int draw_text(const std::string& value, int x, int y, SDL_Color color, Style style = Style::body,
                  int max_width = 0) {
        std::string shown = value;
        if (max_width > 0) { // long paths: keep the end
            while (shown.size() > 4 && text(shown, style).w > max_width) {
                shown = "..." + shown.substr(4);
            }
        }
        const Text& t = text(shown, style);
        SDL_SetTextureColorMod(t.texture, color.r, color.g, color.b);
        SDL_Rect rect{x, y, t.w, t.h};
        SDL_RenderCopy(renderer_, t.texture, nullptr, &rect);
        return t.h;
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
        SDL_RenderFillRect(renderer_, &rect);
    }
    void frame(SDL_Rect rect, SDL_Color color) {
        SDL_SetRenderDrawColor(renderer_, color.r, color.g, color.b, color.a);
        SDL_RenderDrawRect(renderer_, &rect);
    }
    bool inside(SDL_Rect rect) const {
        return mx >= rect.x && mx < rect.x + rect.w && my >= rect.y && my < rect.y + rect.h && mx >= clip.x &&
               mx < clip.x + clip.w && my >= clip.y && my < clip.y + clip.h;
    }
    // A button; true when it was clicked this frame.
    bool button(SDL_Rect rect, const std::string& label, bool enabled, bool primary = false, bool selected = false) {
        const bool hover = enabled && inside(rect);
        const SDL_Color base = !enabled              ? SDL_Color{60, 52, 44, 255}
                               : primary || selected ? SDL_Color{150, 98, 30, 255}
                                                     : SDL_Color{82, 66, 48, 255};
        fill(rect, hover ? SDL_Color{Uint8(base.r + 30), Uint8(base.g + 24), Uint8(base.b + 14), 255} : base);
        frame(rect, {200, 160, 90, 255});
        const Text& t = text(label);
        draw_text(label, rect.x + (rect.w - t.w) / 2, rect.y + (rect.h - t.h) / 2,
                  enabled ? SDL_Color{245, 230, 200, 255} : SDL_Color{140, 130, 115, 255});
        return hover && clicked;
    }
    // "<  value  >": a click on the left third steps back, anywhere else forward. Returns -1, 0 or +1.
    int selector(SDL_Rect rect, const std::string& label, bool enabled, bool changed_from_default) {
        const bool hover = enabled && inside(rect);
        fill(rect, hover ? SDL_Color{70, 56, 40, 255} : SDL_Color{48, 38, 28, 255});
        frame(rect, changed_from_default ? colors.gold : colors.line);
        const SDL_Color arrow = enabled ? colors.gold : colors.line;
        draw_text("<", rect.x + 10, rect.y + (rect.h - text("<").h) / 2, arrow);
        draw_text(">", rect.x + rect.w - 10 - text(">").w, rect.y + (rect.h - text(">").h) / 2, arrow);
        const Text& t = text(label);
        draw_text(label, rect.x + (rect.w - t.w) / 2, rect.y + (rect.h - t.h) / 2,
                  enabled ? colors.ink : SDL_Color{140, 130, 115, 255}, Style::body, rect.w - 50);
        if (!hover || !clicked) {
            return 0;
        }
        return mx < rect.x + rect.w / 3 ? -1 : 1;
    }

private:
    SDL_Renderer* renderer_;
    sacred::Font body_, small_, heading_, title_;
    std::map<std::string, Text> cache_;
};

int step(int index, int delta, int count) {
    return count ? ((index + delta) % count + count) % count : 0;
}
}

int main(int, char** argv) {
    const fs::path here = executable_dir(argv[0]);
    const fs::path source =
        env("SACRED_LAUNCHER_ROOT").empty() ? here / "source" : fs::path(env("SACRED_LAUNCHER_ROOT"));
    const std::string python = !env("SACRED_LAUNCHER_PYTHON").empty()    ? env("SACRED_LAUNCHER_PYTHON")
                               : fs::exists(here / "python/bin/python3") ? (here / "python/bin/python3").string()
                                                                         : "python3";
    // Compilers in the order build_game.py tries them: the system's first, the bundled Zig after it.
    std::string compilers;
    if (!env("SACRED_LAUNCHER_CC").empty()) {
        compilers = " --cc " + shell_quote(env("SACRED_LAUNCHER_CC"));
    } else {
        compilers = " --cc " + shell_quote(env("CC").empty() ? "cc" : env("CC"));
        if (fs::exists(here / "zig/zig")) {
            compilers += " --cc " + shell_quote(shell_quote((here / "zig/zig").string()) + " cc");
        }
    }
    const fs::path script =
        !env("SACRED_LAUNCHER_GAME").empty() ? fs::path(env("SACRED_LAUNCHER_GAME")) : here / "sacred-gold";
    const fs::path pictures = here / "menu-backgrounds";

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
    std::unique_ptr<Ui> ui;
    try {
        ui = std::make_unique<Ui>(renderer);
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
    const SDL_Rect list{0, 72, W, 464}; // the settings page's scrolled area
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
        ui->clip = {0, 0, W, H};
        const bool busy = build.running;
        ui->fill({0, 0, W, H}, {28, 22, 18, 255});
        ui->fill({0, 0, W, 64}, {48, 34, 22, 255});
        ui->draw_text("Sacred Native", 28, 14, colors.gold, Style::title);
        if (ui->button({W - 268, 17, 116, 30}, "Game", true, false, page == Page::play)) {
            page = Page::play;
        }
        if (ui->button({W - 144, 17, 116, 30}, "Settings", !busy, false, page == Page::settings)) {
            page = Page::settings;
            editing = false;
            SDL_StopTextInput();
        }

        if (page == Page::play) {
            int y = 86;
            ui->draw_text("Game folder (your installation, only ever read):", 28, y, colors.dim);
            y += 26;
            SDL_Rect field{28, y, 600, 30};
            ui->fill(field, editing ? SDL_Color{50, 40, 30, 255} : SDL_Color{40, 32, 26, 255});
            ui->frame(field, editing ? colors.gold : colors.line);
            ui->draw_text(editing ? typed + "|" : (config.game.empty() ? "(none)" : config.game), 36, y + 6, colors.ink,
                          Style::body, 580);
            if (!busy && ui->clicked && ui->inside(field) && !editing) {
                editing = true;
                typed = config.game;
                SDL_StartTextInput();
            }
            if (ui->button({638, y, 134, 30}, "Browse...", !busy)) {
                const std::string chosen =
                    choose_folder(config.game.empty() ? env("HOME") : config.game, "Sacred Gold folder");
                if (!chosen.empty()) {
                    config.game = typed = chosen;
                    problem = check_installation(config.game);
                }
                editing = false;
                SDL_StopTextInput();
            }
            y += 38;
            ui->draw_text(problem.empty() ? "Sacred Gold 2.28 found." : problem, 28, y,
                          problem.empty() ? colors.good : colors.bad, Style::body, 744);

            y += 44;
            ui->draw_text("Settings", 28, y, colors.gold, Style::heading);
            y += 28;
            SettingsFile shown;
            for (const auto& entry : choices.values) {
                shown.set(entry.first, entry.second);
            }
            y += ui->draw_wrapped(summary(shown, choices.size, config.fullscreen) + ".", 28, y, 600, colors.ink,
                                  Style::body, 22);
            if (ui->button({638, y - 26, 134, 30}, "Change...", !busy)) {
                page = Page::settings;
            }
            y += 14;
            ui->draw_text("Settings and saves:", 28, y, colors.dim);
            ui->draw_text(profile.string(), 28 + ui->text("Settings and saves:").w + 8, y, colors.dim, Style::body,
                          736 - ui->text("Settings and saves:").w);

            y += 46;
            if (busy || build.finished) {
                std::lock_guard<std::mutex> guard(build.lock);
                ui->draw_text(build.failed         ? "The build failed:"
                              : build.step.empty() ? "Starting..."
                                                   : build.step,
                              28, y, build.failed ? colors.bad : colors.ink);
                if (!build.failed) {
                    SDL_Rect bar{28, y + 26, 744, 16};
                    ui->frame(bar, colors.line);
                    const float share = build.total ? float(build.done) / float(build.total) : 0.0f;
                    ui->fill({bar.x + 2, bar.y + 2, int(float(bar.w - 4) * share), bar.h - 4}, {200, 140, 50, 255});
                    if (build.total) {
                        const std::string count = std::to_string(build.done) + " / " + std::to_string(build.total);
                        ui->draw_text(count, 772 - ui->text(count).w, y, colors.dim);
                    }
                    ui->draw_text(
                        "The first start translates the game from your files; this takes a few minutes, once.", 28,
                        y + 48, colors.dim, Style::body, 744);
                } else {
                    std::istringstream lines(build.error);
                    int line_y = y + 24;
                    for (std::string line; std::getline(lines, line) && line_y < 530; line_y += 20) {
                        ui->draw_text(line, 28, line_y, colors.bad, Style::body, 744);
                    }
                }
            }
        } else {
            // ------------------------------------------------------------------------------ the settings page
            const int max_scroll = std::max(0, content_height - list.h);
            scroll -= wheel * 40;
            if (content_height) {
                scroll = std::clamp(scroll, 0, max_scroll); // known from the second frame on
            }
            SDL_RenderSetClipRect(renderer, &list);
            ui->clip = list;
            const int label_x = 28, help_w = 430, control_x = 500, control_w = 272;
            int y = list.y + 14 - scroll;
            auto heading = [&](const char* title) {
                y += 6;
                ui->draw_text(title, label_x, y, colors.gold, Style::heading);
                ui->fill({label_x, y + 24, control_x + control_w - label_x, 1}, colors.line);
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
                y += std::max(height, 34) + 16;
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
                        ui->selector({control_x, top, control_w, 30}, list_of[index].label, !busy, changed);
                    if (delta) {
                        choices.values[key] = list_of[size_t(step(int(index), delta, int(list_of.size())))].value;
                        persist();
                    }
                });
            };

            heading(section_title(Section::display));
            row("Display mode",
                "Fullscreen covers the whole display with a borderless window. Alt+Enter (Option+Enter on macOS) "
                "switches while you play.",
                "", [&](int top) {
                    if (ui->selector({control_x, top, control_w, 30}, config.fullscreen ? "Fullscreen" : "Window",
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
                        const int delta = ui->selector({control_x, top, control_w, 30}, sizes[index].label(), !busy,
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
            heading(section_title(Section::look));
            option_row("LEGACY_UI");
            heading(section_title(Section::graphics));
            option_row("RENDER_SCALE");
            {
                const Option& scale = *find_option("RENDER_SCALE");
                const Option& upscale = *find_option("TEXTURE_UPSCALE");
                const bool hidden = upscale.index_of(choices.values["TEXTURE_UPSCALE"]) != 0 &&
                                    scale.index_of(choices.values["RENDER_SCALE"]) == 0;
                option_row("TEXTURE_UPSCALE", hidden ? "Set the render scale to 2x or more to see them." : "");
            }
            content_height = y + scroll - list.y;
            SDL_RenderSetClipRect(renderer, nullptr);
            ui->clip = {0, 0, W, H};
            if (max_scroll > 0) { // where the list stands
                const int bar_h = std::max(30, list.h * list.h / content_height);
                const int bar_y = list.y + (list.h - bar_h) * scroll / max_scroll;
                ui->fill({W - 8, list.y, 4, list.h}, {48, 38, 28, 255});
                ui->fill({W - 8, bar_y, 4, bar_h}, colors.line);
            }
            ui->fill({0, list.y + list.h, W, 1}, colors.line);
            if (ui->button({28, 554, 150, 32}, "Defaults", !busy)) {
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
            ui->draw_text(saved_note, 192, 562, colors.dim, Style::small, 400);
        }

        if (build.finished && !build.failed) {
            launch = true;
        }
        if (ui->button({612, 548, 160, 40}, busy ? "Building..." : "Play", !busy && problem.empty(), true)) {
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
            build.start(shell_quote(python) + " " + shell_quote((source / "tools/launcher/build_game.py").string()) +
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
