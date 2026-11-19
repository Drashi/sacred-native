#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace sacred {
struct KerningPair {
    uint16_t first, second;
    int32_t amount;
};
struct TextRun {
    int32_t width = 0, height = 0;
    std::vector<uint8_t> alpha;
};

struct FontMetrics {
    int32_t height, ascent, descent, internal_leading, average_width, max_width;
    uint32_t weight;
    bool italic, fixed_pitch;
    uint8_t first, last, default_char, break_char;
};

class Font {
public:
    Font(const std::string& family, int32_t height, uint32_t weight, bool italic,
         const std::vector<std::string>& resources = {});
    ~Font();
    // Grayscale-antialiased single line of ASCII text; alpha is width*height, top-down.
    TextRun render(const std::u16string& text) const;
    std::pair<int32_t, int32_t> measure(const std::u16string& text) const;
    std::vector<KerningPair> kerning_pairs() const;
    FontMetrics metrics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
class FontResource {
public:
    explicit FontResource(const std::string& path);
    ~FontResource();
    uint32_t count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
