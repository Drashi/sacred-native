#pragma once
#include <cstdint>
#include <string>
#include <vector>

typedef struct FT_LibraryRec_* FT_Library;

namespace sacred {
// A font file and the face inside it.
struct FontFace {
    std::string path;
    long index = 0;
};

// Our own replacement for fontconfig's match (CreateFont semantics): the family is looked up among the
// application fonts first (AddFontResource files), then among the system fonts, by name and then through a table of
// metric-compatible substitutes (Times New Roman -> Liberation Serif, ...); within a family the face closest in
// weight and slant wins. Never fails while any font exists: an unknown family falls back to a sans-serif face.
FontFace match_font(FT_Library library, const std::string& family, uint32_t weight, bool italic,
                    const std::vector<std::string>& resources);

// System font directories searched (SACRED_FONT_DIRS, ':'-separated, replaces them when set).
std::vector<std::string> system_font_directories();
}
