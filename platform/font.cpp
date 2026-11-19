#include "font.h"
#include "../runtime/process/memory.h"
#include "font_match.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H
#include <algorithm>
#include <cstdio>
#include <map>

namespace sacred {
namespace {
struct FaceLifetime {
    FT_Library library = nullptr;
    FT_Face face = nullptr;
    ~FaceLifetime() {
        if (face) {
            FT_Done_Face(face);
        }

        if (library) {
            FT_Done_FreeType(library);
        }
    }
};
}

struct Font::Impl : FaceLifetime {
    uint32_t weight = 0;
    bool italic = false;
};
struct FontResource::Impl : FaceLifetime {
    uint32_t count = 0;
};
FontResource::FontResource(const std::string& path) : impl_(std::make_unique<Impl>()) {
    if (FT_Init_FreeType(&impl_->library) || FT_New_Face(impl_->library, path.c_str(), -1, &impl_->face)) {
        return;
    }

    impl_->count = uint32_t(impl_->face->num_faces);
}

FontResource::~FontResource() = default;
uint32_t FontResource::count() const {
    return impl_->count;
}

Font::Font(const std::string& family, int32_t height, uint32_t weight, bool italic,
           const std::vector<std::string>& resources)
    : impl_(std::make_unique<Impl>()) {
    if (!height || height == INT32_MIN || weight > 1000) {
        throw GuestFault("unqualified logical font size or weight");
    }

    if (FT_Init_FreeType(&impl_->library)) {
        throw GuestFault("cannot initialize FreeType");
    }

    const FontFace face = match_font(impl_->library, family, weight ? weight : 400, italic, resources);
    if (FT_New_Face(impl_->library, face.path.c_str(), face.index, &impl_->face)) {
        throw GuestFault("cannot open matched native font");
    }

    FT_Size_RequestRec request{};
    request.type = height > 0 ? FT_SIZE_REQUEST_TYPE_REAL_DIM : FT_SIZE_REQUEST_TYPE_NOMINAL;
    request.height = FT_Long(height > 0 ? height : -height) * 64;
    if (FT_Request_Size(impl_->face, &request)) {
        throw GuestFault("cannot size matched native font");
    }

    impl_->weight = weight ? weight : 400;
    impl_->italic = italic;
    std::fprintf(stderr, "FONT requested=%s resolved=%s height=%d weight=%u italic=%u\n", family.c_str(),
                 impl_->face->family_name, height, weight, italic ? 1u : 0u);
}

Font::~Font() = default;
FontMetrics Font::metrics() const {
    FT_Face face = impl_->face;
    const auto& size = face->size->metrics;
    FontMetrics result{};
    result.ascent = int32_t((size.ascender + 63) >> 6);
    result.descent = int32_t((-size.descender + 63) >> 6);
    result.height = result.ascent + result.descent;
    result.internal_leading = std::max<int32_t>(0, result.height - int32_t(size.y_ppem));
    result.max_width = int32_t((size.max_advance + 63) >> 6);
    result.weight = impl_->weight;
    result.italic = impl_->italic;
    result.fixed_pitch = FT_IS_FIXED_WIDTH(face);
    result.first = 0x20;
    result.last = 0xff;
    result.break_char = 0x20;
    auto* os2 = static_cast<TT_OS2*>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2));
    if (!os2 || !face->units_per_EM) {
        throw GuestFault("native font has no OS/2 metrics");
    }

    result.average_width =
        int32_t((int64_t(os2->xAvgCharWidth) * size.x_ppem + face->units_per_EM / 2) / face->units_per_EM);
    result.default_char = uint8_t(os2->usDefaultChar);
    result.break_char = uint8_t(os2->usBreakChar);
    return result;
}

namespace {
std::vector<FT_UInt> glyph_indices(FT_Face face, const std::u16string& text) {
    std::vector<FT_UInt> glyphs;
    for (char16_t character : text) {
        glyphs.push_back(FT_Get_Char_Index(face, character));
    }

    return glyphs;
}

int32_t advance_of(FT_Face face, FT_UInt glyph) {
    if (FT_Load_Glyph(face, glyph, FT_LOAD_DEFAULT)) {
        throw GuestFault("cannot load native glyph");
    }

    return int32_t((face->glyph->advance.x + 32) >> 6);
}
}

std::pair<int32_t, int32_t> Font::measure(const std::u16string& text) const {
    FT_Face face = impl_->face;
    int32_t width = 0;
    for (FT_UInt glyph : glyph_indices(face, text)) {
        width += advance_of(face, glyph);
    }

    return {width,
            int32_t((face->size->metrics.ascender + 63) >> 6) + int32_t((-face->size->metrics.descender + 63) >> 6)};
}

TextRun Font::render(const std::u16string& text) const {
    FT_Face face = impl_->face;
    const int32_t ascent = int32_t((face->size->metrics.ascender + 63) >> 6);
    const auto glyphs = glyph_indices(face, text);
    TextRun run;
    auto size = measure(text);
    run.width = size.first;
    run.height = size.second;
    run.alpha.assign(size_t(run.width) * run.height, 0);
    int32_t pen = 0;
    for (FT_UInt glyph : glyphs) {
        if (FT_Load_Glyph(face, glyph, FT_LOAD_RENDER)) {
            throw GuestFault("cannot render native glyph");
        }

        const FT_GlyphSlot slot = face->glyph;
        if (slot->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY && slot->bitmap.width) {
            throw GuestFault("unqualified native glyph pixel mode");
        }

        for (unsigned row = 0; row < slot->bitmap.rows; ++row) {
            for (unsigned column = 0; column < slot->bitmap.width; ++column) {
                int32_t x = pen + slot->bitmap_left + int32_t(column), y = ascent - slot->bitmap_top + int32_t(row);
                if (x < 0 || y < 0 || x >= run.width || y >= run.height) {
                    continue;
                }

                uint8_t& target = run.alpha[size_t(y) * run.width + x];
                target = uint8_t(std::max<int>(target, slot->bitmap.buffer[row * slot->bitmap.pitch + column]));
            }
        }

        pen += int32_t((slot->advance.x + 32) >> 6);
    }

    return run;
}

std::vector<KerningPair> Font::kerning_pairs() const {
    FT_Face face = impl_->face;
    FT_ULong length = 0;
    if (FT_Load_Sfnt_Table(face, FT_MAKE_TAG('k', 'e', 'r', 'n'), 0, nullptr, &length)) {
        return {};
    }

    std::vector<uint8_t> table(length);
    if (FT_Load_Sfnt_Table(face, FT_MAKE_TAG('k', 'e', 'r', 'n'), 0, table.data(), &length)) {
        throw GuestFault("cannot read native kerning table");
    }

    auto word = [&](size_t at) {
        if (at + 2 > table.size()) {
            throw GuestFault("truncated native kerning table");
        }

        return uint16_t(table[at] << 8 | table[at + 1]);
    };
    if (word(0) != 0) {
        throw GuestFault("unqualified native kerning table version");
    }

    std::map<FT_UInt, uint16_t> codes;
    FT_UInt glyph = 0;
    for (FT_ULong code = FT_Get_First_Char(face, &glyph); glyph; code = FT_Get_Next_Char(face, code, &glyph)) {
        if (code <= 0xffff && !codes.count(glyph)) {
            codes[glyph] = uint16_t(code);
        }
    }

    std::map<std::pair<uint16_t, uint16_t>, int32_t> pairs;
    size_t at = 4;
    for (unsigned subtable = 0; subtable < word(2); ++subtable) {
        const uint16_t size = word(at + 2), coverage = word(at + 4);
        if ((coverage >> 8) == 0 && (coverage & 1)) {
            for (unsigned i = 0; i < word(at + 6); ++i) {
                const size_t entry = at + 14 + i * 6;
                auto left = codes.find(word(entry)), right = codes.find(word(entry + 2));
                const int16_t value = int16_t(word(entry + 4));
                if (left == codes.end() || right == codes.end() || !value) {
                    continue;
                }

                const int64_t scaled = int64_t(value) * face->size->metrics.x_ppem;
                pairs[{left->second, right->second}] = int32_t(
                    (scaled + (scaled < 0 ? -int64_t(face->units_per_EM) / 2 : int64_t(face->units_per_EM) / 2)) /
                    face->units_per_EM);
            }
        }

        at += size;
    }

    std::vector<KerningPair> result;
    for (const auto& pair : pairs) {
        result.push_back({pair.first.first, pair.first.second, pair.second});
    }

    return result;
}
}
