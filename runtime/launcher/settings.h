#pragma once
// The launcher's model of the options this project adds to Settings.cfg (and the one original key that matters with
// them): what each key means for a player, the values it offers, and an editor for the file that keeps every other
// line as the game wrote it. No SDL here, so the tests can use it (tests/launcher/settings_tests.cpp).
#include <filesystem>
#include <string>
#include <vector>

namespace sacred::launcher {

// Settings.cfg as "KEY : value" lines. The game sorts its keys and writes CR LF; keys it does not know (ours) are
// kept as they are. A key may appear twice in a hand-edited file: get() reads the last one (as our parsers in
// runtime/modules/resolution.cpp do), set() replaces the first and drops the others.
class SettingsFile {
public:
    bool load(const std::filesystem::path& file); // false when there is no file (the lines are then empty)
    bool save(const std::filesystem::path& file) const;
    void parse(const std::string& text);
    std::string text() const;                      // CR LF lines
    std::string get(const std::string& key) const; // "" when absent
    bool has(const std::string& key) const;
    void set(const std::string& key, const std::string& value); // "" removes the key
    const std::vector<std::string>& lines() const { return lines_; }

private:
    std::vector<std::string> lines_;
};

struct Choice {
    std::string value; // as written to Settings.cfg; "" means the key is left out (the default)
    std::string label;
};

enum class Section { display, look, graphics, gameplay };

// One option of the settings page with fixed choices. Values found in the file that are not among the choices (a
// hand-edited FPS_LIMIT : 100) are added as an extra choice so that nothing is lost by opening the page.
struct Option {
    std::string key;
    Section section;
    std::string title, help;
    std::vector<Choice> choices;              // the first is the default, value ""
    std::vector<std::string> same_as_default; // values that mean the default too (FPS_LIMIT : 60)
    std::string recommended; // what a new profile starts with and the Defaults button sets ("": the game's default)
    // Index of the choice the file's value stands for ("" for the default). Values are compared case-insensitively
    // and as numbers where both are numbers, so "02" is "2" and "AUTO" is "auto".
    size_t index_of(const std::string& value) const;
};
const std::vector<Option>& options();
const Option* find_option(const std::string& key);
const char* section_title(Section section);

// The screen size: HD_WIDTH and HD_HEIGHT together. `fit` (no keys, fullscreen at the display's own size) and
// `original` (no keys, 1024x768) differ only in the launcher (SACRED_HD_AUTO=0 for the original size).
struct ScreenSize {
    int width = 0, height = 0; // 0 x 0: no keys
    bool fit = false;
    std::string label() const;
    bool operator==(const ScreenSize& other) const {
        return width == other.width && height == other.height && fit == other.fit;
    }
};
// The sizes the page offers, in order: original, fit, the common ones up to the display's size (`display_w` x
// `display_h`, 0 when unknown: all), the display's own size when it is not among them, and `current` when it is
// something else (a size typed into the file).
std::vector<ScreenSize> screen_sizes(int display_w, int display_h, const ScreenSize& current);
// The size Settings.cfg names (HD_WIDTH and HD_HEIGHT, raised to 1024x720 as the engine does), or the original or
// fit one from the launcher's own choice when the keys are absent.
ScreenSize read_screen_size(const SettingsFile& settings, bool launcher_fit);
void write_screen_size(SettingsFile& settings, const ScreenSize& size);

// The launcher's own defaults: fullscreen at the display's size (no HD_WIDTH/HD_HEIGHT), and each option's
// `recommended` value.
constexpr bool default_fullscreen = true, default_fit = true;
// Every key the settings page manages, back to the launcher's defaults (the screen size included; the launcher sets
// MENU_BACKGROUNDS itself at every start). Other keys stay.
void reset_to_defaults(SettingsFile& settings);

// "1024x768, 120 FPS, Original Sacred, HD textures" for the play page.
std::string summary(const SettingsFile& settings, const ScreenSize& size, bool fullscreen);
}
