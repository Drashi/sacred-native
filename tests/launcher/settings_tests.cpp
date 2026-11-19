// The launcher's settings page model: Settings.cfg editing that keeps the game's lines, the option choices and the
// screen size (runtime/launcher/settings.h).
#include "runtime/launcher/settings.h"
#include <cstdio>
#include <string>

using namespace sacred::launcher;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++failures;
    }
}
}

int main() {
    { // set() replaces in place, drops duplicates, inserts new keys in sorted order, removes on ""
        SettingsFile file;
        file.parse("AUTOSAVE : 0\r\nFPS_LIMIT : 90\r\nSOUND : 1\r\nFPS_LIMIT : 100\r\n");
        check(file.get("FPS_LIMIT") == "100", "get reads the last line of a key");
        file.set("FPS_LIMIT", "120");
        check(file.text() == "AUTOSAVE : 0\r\nFPS_LIMIT : 120\r\nSOUND : 1\r\n",
              "set replaces the first, drops the rest");
        file.set("LEGACY_UI", "1");
        check(file.text() == "AUTOSAVE : 0\r\nFPS_LIMIT : 120\r\nLEGACY_UI : 1\r\nSOUND : 1\r\n",
              "new key in sorted place");
        file.set("FPS_LIMIT", "");
        check(!file.has("FPS_LIMIT") && file.lines().size() == 3, "\"\" removes the key");
        file.set("ZZZ", "x");
        check(file.lines().back() == "ZZZ : x", "a key after all others goes last");
    }

    { // choices: defaults, equivalent spellings, values from the file that are none of the choices
        const Option& fps = *find_option("FPS_LIMIT");
        check(fps.index_of("") == 0 && fps.index_of("60") == 0, "FPS_LIMIT 60 is the default");
        check(fps.choices[fps.index_of("AUTO")].value == "auto", "auto in any case");
        check(fps.choices[fps.index_of("0120")].value == "120", "numbers compare as numbers");
        check(fps.index_of("100") == fps.choices.size(), "an unknown value is none of the choices");
        const Option& upscale = *find_option("TEXTURE_UPSCALE");
        check(upscale.index_of("1") == 0 && upscale.index_of("0") == 0, "TEXTURE_UPSCALE 0 and 1 are off");
        for (const auto& option : options()) {
            check(!option.choices.empty() && option.choices[0].value.empty(),
                  "every option's first choice is the default");
        }
    }

    { // screen size
        SettingsFile file;
        check(read_screen_size(file, false) == ScreenSize{0, 0, false}, "no keys: original");
        check(read_screen_size(file, true) == ScreenSize{0, 0, true}, "no keys: fit when the launcher says so");
        file.set("HD_WIDTH", "1920");
        check(read_screen_size(file, true) == ScreenSize{0, 0, true}, "one key alone is no size");
        file.set("HD_HEIGHT", "600");
        check(read_screen_size(file, false) == ScreenSize{1920, 720, false}, "raised to the minimum");
        write_screen_size(file, {0, 0, true});
        check(!file.has("HD_WIDTH") && !file.has("HD_HEIGHT"), "fit writes no keys");
        const auto sizes = screen_sizes(1920, 1080, {1700, 960, false});
        check(sizes[0] == ScreenSize{0, 0, false} && sizes[1] == ScreenSize{0, 0, true}, "original and fit first");
        bool has_current = false, too_big = false;
        for (const auto& size : sizes) {
            has_current |= size == ScreenSize{1700, 960, false};
            too_big |= size.width > 1920;
        }

        check(has_current, "the file's size is offered");
        check(!too_big, "no size larger than the display");
        const auto odd = screen_sizes(1512, 982, {0, 0, false});
        check(odd.back() == ScreenSize{1512, 982, false}, "the display's own size is offered");
    }

    { // defaults and summary
        SettingsFile file;
        file.parse(
            "FPS_LIMIT : 120\r\nSHOW_FPS : 1\r\nLEGACY_UI : 1\r\nHD_WIDTH : 1920\r\nHD_HEIGHT : 1080\r\nSOUND : 1\r\n");
        check(summary(file, read_screen_size(file, false), true) ==
                  "Fullscreen, 1920 x 1080, 120 FPS, FPS counter, original Sacred look",
              "summary");
        reset_to_defaults(file);
        check(file.text() == "FPS_LIMIT : auto\r\nRENDER_SCALE : 2\r\nSOUND : 1\r\nTEXTURE_UPSCALE : 2\r\n",
              "defaults: the launcher's recommended values, no screen size, the game's keys kept");
    }

    if (failures) {
        return 1;
    }

    std::puts("launcher settings: ok");
    return 0;
}
