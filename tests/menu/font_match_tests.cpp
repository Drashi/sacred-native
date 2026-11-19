// Our font matching (platform/font_match.cpp), which replaced fontconfig: application fonts first, then system fonts
// by name, metric-compatible substitutes for Windows families, nearest weight and slant, and a fallback.
#include "platform/font_match.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>

using namespace sacred;
static unsigned checks = 0;
static void require(bool condition, const std::string& what) {
    ++checks;
    if (!condition) {
        throw std::runtime_error("FAIL: " + what);
    }
}

static std::string family_of(FT_Library library, const FontFace& face) {
    FT_Face opened = nullptr;
    if (FT_New_Face(library, face.path.c_str(), face.index, &opened)) {
        throw std::runtime_error("cannot open " + face.path);
    }

    std::string family = opened->family_name ? opened->family_name : "";
    FT_Done_Face(opened);
    return family;
}

int main() {
    try {
        FT_Library library = nullptr;
        if (FT_Init_FreeType(&library)) {
            throw std::runtime_error("FreeType");
        }

        // A system font: the first file of the system directories that FreeType opens.
        std::string sample;
        for (const auto& directory : system_font_directories()) {
            std::error_code status;
            for (std::filesystem::recursive_directory_iterator it(directory, status), end;
                 !status && it != end && sample.empty(); it.increment(status)) {
                const auto extension = it->path().extension().string();
                FT_Face face = nullptr;
                if ((extension == ".ttf" || extension == ".otf") &&
                    !FT_New_Face(library, it->path().c_str(), 0, &face)) {
                    if (face->family_name) {
                        sample = it->path().string();
                    }

                    FT_Done_Face(face);
                }
            }
        }

        if (sample.empty()) {
            std::puts("FONT_MATCH_SKIP no system fonts");
            return 0;
        }

        const std::string family = family_of(library, {sample, 0});

        require(family_of(library, match_font(library, family, 400, false, {})) == family, "system family by name");
        require(family_of(library, match_font(library, family + "-not-installed", 400, false, {})).size() > 0,
                "an unknown family falls back to some face");
        // An application font wins over the system and is found by its own family name, in any case.
        require(match_font(library, family, 400, false, {sample}).path == sample, "application font by name");
        std::string upper = family;
        for (char& c : upper) {
            c = char(std::toupper(static_cast<unsigned char>(c)));
        }

        require(match_font(library, upper, 400, false, {sample}).path == sample, "family names ignore case");
        // Times New Roman: itself or a metric-compatible serif when one is installed.
        const std::string times = family_of(library, match_font(library, "Times New Roman", 400, false, {}));
        bool serif_installed = false;
        for (const char* name : {"Times New Roman", "Liberation Serif", "Tinos"}) {
            serif_installed = serif_installed || family_of(library, match_font(library, name, 400, false, {})) == name;
        }

        if (serif_installed) {
            require(times == "Times New Roman" || times == "Liberation Serif" || times == "Tinos",
                    "Times New Roman substitute is " + times);
        }

        // Bold asks for the bold face when the family has one.
        const FontFace regular = match_font(library, "Liberation Serif", 400, false, {});
        const FontFace bold = match_font(library, "Liberation Serif", 700, false, {});
        if (family_of(library, regular) == "Liberation Serif") {
            require(bold.path != regular.path, "bold face");
        }

        FT_Done_FreeType(library);
        std::printf("FONT_MATCH_PASS checks=%u sample=%s times=%s\n", checks, family.c_str(), times.c_str());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
