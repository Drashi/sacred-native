#include "font_match.h"
#include "../runtime/process/memory.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <mutex>

namespace sacred {
namespace {
struct Entry {
    std::string family; // lower case
    FontFace face;
    uint32_t weight;
    bool italic;
};

std::string lower(std::string text) {
    for (char& c : text) {
        c = char(std::tolower(static_cast<unsigned char>(c)));
    }

    return text;
}

// Every face of one file; files FreeType cannot open are skipped.
void scan_file(FT_Library library, const std::string& path, std::vector<Entry>& out) {
    FT_Face face = nullptr;
    if (FT_New_Face(library, path.c_str(), 0, &face)) {
        return;
    }

    const long faces = face->num_faces;
    for (long index = 0;;) {
        if (face->family_name) {
            auto* os2 = static_cast<TT_OS2*>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2));
            const uint32_t weight = os2 && os2->usWeightClass                  ? os2->usWeightClass
                                    : (face->style_flags & FT_STYLE_FLAG_BOLD) ? 700
                                                                               : 400;
            out.push_back(
                {lower(face->family_name), {path, index}, weight, (face->style_flags & FT_STYLE_FLAG_ITALIC) != 0});
        }

        FT_Done_Face(face);
        if (++index >= faces || FT_New_Face(library, path.c_str(), index, &face)) {
            return;
        }
    }
}

bool font_file(const std::filesystem::path& path) {
    const std::string extension = lower(path.extension().string());
    return extension == ".ttf" || extension == ".otf" || extension == ".ttc" || extension == ".otc";
}

const std::vector<Entry>& system_fonts(FT_Library library) {
    static std::once_flag once;
    static std::vector<Entry> fonts;
    std::call_once(once, [library] {
        std::vector<std::string> files;
        for (const auto& directory : system_font_directories()) {
            std::error_code status;
            std::filesystem::recursive_directory_iterator it(
                directory, std::filesystem::directory_options::skip_permission_denied, status),
                end;
            for (; !status && it != end; it.increment(status)) {
                if (font_file(it->path()) && it->is_regular_file(status)) {
                    files.push_back(it->path().string());
                }
            }
        }

        std::sort(files.begin(), files.end()); // same choice on every run, whatever the directory order
        for (const auto& file : files) {
            scan_file(library, file, fonts);
        }
    });
    return fonts;
}

// Windows family names the game may ask for and the faces that stand in for them on other systems, best first.
const std::vector<std::string>& substitutes(const std::string& family) {
    static const std::vector<std::string> serif = {"times new roman", "liberation serif", "tinos",
                                                   "times",           "dejavu serif",     "noto serif"};
    static const std::vector<std::string> sans = {"arial",       "liberation sans", "arimo",  "helvetica",
                                                  "dejavu sans", "noto sans",       "verdana"};
    static const std::vector<std::string> mono = {"courier new",      "liberation mono", "cousine", "courier",
                                                  "dejavu sans mono", "noto sans mono",  "menlo"};
    static const std::vector<std::string> none;
    if (family == "times new roman" || family == "times" || family == "serif" || family == "ms serif" ||
        family == "georgia" || family == "book antiqua" || family == "palatino linotype") {
        return serif;
    }

    if (family == "courier new" || family == "courier" || family == "monospace" || family == "lucida console" ||
        family == "consolas" || family == "fixedsys") {
        return mono;
    }

    if (family == "arial" || family == "helvetica" || family == "sans-serif" || family == "ms sans serif" ||
        family == "tahoma" || family == "verdana" || family == "microsoft sans serif" || family == "system") {
        return sans;
    }

    return none;
}

const Entry* best(const std::vector<Entry>& fonts, const std::string& family, uint32_t weight, bool italic) {
    const Entry* chosen = nullptr;
    uint32_t chosen_cost = 0;
    for (const auto& entry : fonts) {
        if (entry.family != family) {
            continue;
        }

        // Slant outweighs any weight difference, as in fontconfig.
        const uint32_t cost =
            (entry.italic != italic ? 10000u : 0u) + uint32_t(std::abs(int32_t(entry.weight) - int32_t(weight)));
        if (!chosen || cost < chosen_cost) {
            chosen = &entry;
            chosen_cost = cost;
        }
    }

    return chosen;
}
}

std::vector<std::string> system_font_directories() {
    std::vector<std::string> directories;
    if (const char* list = std::getenv("SACRED_FONT_DIRS"); list && *list) {
        std::string text(list);
        for (size_t at = 0; at <= text.size();) {
            const size_t end = std::min(text.find(':', at), text.size());
            if (end > at) {
                directories.push_back(text.substr(at, end - at));
            }

            at = end + 1;
        }

        return directories;
    }

    const char* home = std::getenv("HOME");
#ifdef __APPLE__
    directories = {"/System/Library/Fonts", "/Library/Fonts"};
    if (home) {
        directories.push_back(std::string(home) + "/Library/Fonts");
    }
#else
    directories = {"/usr/share/fonts", "/usr/local/share/fonts"};
    if (const char* data = std::getenv("XDG_DATA_HOME"); data && *data) {
        directories.push_back(std::string(data) + "/fonts");
    } else if (home) {
        directories.push_back(std::string(home) + "/.local/share/fonts");
    }

    if (home) {
        directories.push_back(std::string(home) + "/.fonts");
    }
#endif
    return directories;
}

FontFace match_font(FT_Library library, const std::string& family, uint32_t weight, bool italic,
                    const std::vector<std::string>& resources) {
    std::vector<Entry> application;
    for (const auto& path : resources) {
        scan_file(library, path, application);
    }

    const std::string wanted = lower(family);
    if (const Entry* hit = best(application, wanted, weight, italic)) {
        return hit->face;
    }

    const auto& fonts = system_fonts(library);
    if (const Entry* hit = best(fonts, wanted, weight, italic)) {
        return hit->face;
    }

    for (const auto& name : substitutes(wanted)) {
        if (const Entry* hit = best(fonts, name, weight, italic)) {
            return hit->face;
        }
    }

    for (const auto& name : substitutes("sans-serif")) {
        if (const Entry* hit = best(fonts, name, weight, italic)) {
            return hit->face;
        }
    }

    if (!fonts.empty()) {
        return fonts.front().face;
    }

    if (!application.empty()) {
        return application.front().face;
    }

    throw GuestFault("no font found (set SACRED_FONT_DIRS to a directory with .ttf files)");
}
}
