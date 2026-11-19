#include "menu.h"
#include "../ddraw/menu_backdrops.h"
#include "startup.h"
#include "../../platform/font.h"
#include <cstring>
#include <fstream>
#include <iterator>
#include <algorithm>

namespace sacred {
uint32_t Menu::surface_dc(uint32_t data, uint32_t width, uint32_t height, uint32_t pitch) {
    control_.memory.check(data, uint64_t(pitch) * height, Read | Write);
    uint32_t dc = next_dc_++;
    contexts_.emplace(dc, DeviceContext{Bitmap{width, height, pitch, 32, data, true}});
    return dc;
}

void Menu::release_surface_dc(uint32_t dc) {
    if (!contexts_.erase(dc)) {
        throw GuestFault("invalid surface DC");
    }
}

void Menu::bind_gdi() {
    control_.host(
        loader_.resolve("gdi32.dll", "AddFontResourceA"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4, PointerDirection::In}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            if (!files_) {
                throw GuestFault("font registration requires the guest filesystem");
            }

            std::string path = files_->read_path(ansi(uint32_t(a[0]))).string();
            auto found = font_resources_.find(path);
            if (found != font_resources_.end()) {
                ++found->second.references;
                return Result{found->second.native->count(), {}};
            }

            auto native = std::make_unique<FontResource>(path);
            uint32_t count = native->count();
            if (count) {
                font_resources_.emplace(path, FontResourceEntry{1, std::move(native)});
            }

            return Result{count, {}};
        },
        "gdi32.dll!AddFontResourceA");
    control_.host(
        loader_.resolve("gdi32.dll", "RemoveFontResourceA"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4, PointerDirection::In}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            if (!files_) {
                throw GuestFault("font removal requires the guest filesystem");
            }

            auto found = font_resources_.find(files_->read_path(ansi(uint32_t(a[0]))).string());
            if (found == font_resources_.end()) {
                return Result{};
            }

            if (!--found->second.references) {
                font_resources_.erase(found);
            }

            return Result{1, {}};
        },
        "gdi32.dll!RemoveFontResourceA");
    control_.host(
        loader_.resolve("gdi32.dll", "DeleteDC"), {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            auto found = contexts_.find(uint32_t(a[0]));
            if (found == contexts_.end() || !found->second.compatible) {
                throw GuestFault("invalid or non-memory DC deletion");
            }

            bitmap_contexts_.erase(found->second.bitmap);
            c.heap.free(found->second.stock_data);
            contexts_.erase(found);
            return Result{1, {}};
        },
        "gdi32.dll!DeleteDC");
    control_.host(
        loader_.resolve("gdi32.dll", "BitBlt"), {Convention::Stdcall, ReturnChannel::Eax, std::vector<Argument>(9), ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            auto target = contexts_.find(uint32_t(a[0])), source = contexts_.find(uint32_t(a[5]));
            if (target == contexts_.end() || source == contexts_.end()) {
                throw GuestFault("invalid blit DC");
            }

            if (target->second.window || source->second.window) {
                throw GuestFault("unqualified blit to or from a window DC");
            }

            const auto& dst = target->second.target;
            const auto& src = source->second.target;
            if (backdrops_) {
                auto backdrop = backdrop_bitmaps_.find(source->second.bitmap);
                if (backdrop != backdrop_bitmaps_.end()) {
                    backdrops_->frame = backdrop->second + 1;
                }
            }

            if (a[8] != 0xcc0020 || (src.bits != 24 && src.bits != 32) || (dst.bits != 24 && dst.bits != 32)) {
                throw GuestFault("unqualified GDI blit raster operation or format");
            }

            int64_t x = int32_t(a[1]), y = int32_t(a[2]), width = int32_t(a[3]), height = int32_t(a[4]);
            if (width <= 0 || height <= 0) {
                throw GuestFault("unqualified GDI blit dimensions");
            }

            int64_t left = std::max<int64_t>(0, x), top = std::max<int64_t>(0, y);
            int64_t right = std::min<int64_t>(dst.width, x + width), bottom = std::min<int64_t>(dst.height, y + height);
            if (left >= right || top >= bottom) {
                return Result{1, {}};
            }

            int64_t sx = int32_t(a[6]) + left - x, sy = int32_t(a[7]) + top - y;
            if (sx < 0 || sy < 0 || sx + right - left > src.width || sy + bottom - top > src.height) {
                throw GuestFault("unqualified GDI blit source extent");
            }

            std::vector<uint8_t> input(size_t(src.pitch) * src.height), output(size_t(dst.pitch) * dst.height);
            c.memory.read(src.data, input.data(), input.size());
            c.memory.read(dst.data, output.data(), output.size());
            for (int64_t row = 0; row < bottom - top; ++row) {
                for (int64_t column = 0; column < right - left; ++column) {
                    uint32_t sr = src.top_down ? sy + row : src.height - 1 - (sy + row);
                    uint32_t dr = dst.top_down ? top + row : dst.height - 1 - (top + row);
                    size_t from = size_t(sr) * src.pitch + size_t(sx + column) * (src.bits / 8);
                    size_t to = size_t(dr) * dst.pitch + size_t(left + column) * (dst.bits / 8);
                    for (unsigned channel = 0; channel < 3; ++channel) {
                        output[to + channel] = input[from + channel];
                    }

                    if (dst.bits == 32) {
                        output[to + 3] = 0;
                    }
                }
            }

            c.memory.write(dst.data, output.data(), output.size());
            return Result{1, {}};
        },
        "gdi32.dll!BitBlt");
    control_.host(
        loader_.resolve("gdi32.dll", "SelectObject"), {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            auto found = contexts_.find(uint32_t(a[0]));
            if (found == contexts_.end()) {
                throw GuestFault("invalid object selection DC");
            }

            auto& dc = found->second;
            uint32_t object = uint32_t(a[1]);
            if (!object) {
                return Result{};
            }

            auto bitmap = bitmaps_.find(object);
            if (bitmap != bitmaps_.end() || object == 0xe8000000) {
                if (!dc.compatible) {
                    throw GuestFault("unqualified surface DC bitmap replacement");
                }

                auto owner = bitmap_contexts_.find(object);
                if (owner != bitmap_contexts_.end() && owner->second != a[0]) {
                    return Result{};
                }

                uint32_t previous = dc.bitmap;
                bitmap_contexts_.erase(previous);
                dc.bitmap = object;
                if (bitmap != bitmaps_.end()) {
                    dc.target = bitmap->second;
                    bitmap_contexts_[object] = uint32_t(a[0]);
                } else {
                    dc.target = Bitmap{1, 1, 4, 1, dc.stock_data, true};
                }

                return Result{previous, {}};
            }

            if (fonts_.count(object) || object == 0xe9000000) {
                uint32_t previous = dc.font;
                dc.font = object;
                return Result{previous, {}};
            }

            throw GuestFault("invalid or unqualified GDI object selection");
        },
        "gdi32.dll!SelectObject");
    control_.host(
        loader_.resolve("gdi32.dll", "CreateCompatibleDC"), {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            if (a[0] && !contexts_.count(uint32_t(a[0]))) {
                throw GuestFault("invalid compatible DC source");
            }

            uint32_t data = c.heap.allocate(4);
            DeviceContext context{Bitmap{1, 1, 4, 1, data, true}};
            context.stock_data = data;
            context.compatible = true;
            uint32_t dc = next_dc_++;
            contexts_.emplace(dc, std::move(context));
            return Result{dc, {}};
        },
        "gdi32.dll!CreateCompatibleDC");
    control_.host(
        loader_.resolve("gdi32.dll", "CreateFontIndirectA"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4, PointerDirection::In}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            FontEntry font;
            c.memory.read(uint32_t(a[0]), font.logical.data(), font.logical.size());
            if (c.memory.load32(uint32_t(a[0]) + 4) || c.memory.load32(uint32_t(a[0]) + 8) ||
                c.memory.load32(uint32_t(a[0]) + 12)) {
                throw GuestFault("unqualified logical font width or rotation");
            }

            const char* family = reinterpret_cast<const char*>(font.logical.data() + 28);
            size_t length = 0;
            while (length < 32 && family[length]) {
                ++length;
            }

            std::vector<std::string> resources;
            for (const auto& resource : font_resources_) {
                resources.push_back(resource.first);
            }

            font.native =
                std::make_unique<Font>(std::string(family, length), int32_t(c.memory.load32(uint32_t(a[0]))),
                                       c.memory.load32(uint32_t(a[0]) + 16), font.logical[20] != 0, resources);
            uint32_t handle = next_font_++;
            fonts_.emplace(handle, std::move(font));
            return Result{handle, {}};
        },
        "gdi32.dll!CreateFontIndirectA");
    control_.host(
        loader_.resolve("gdi32.dll", "SetBkMode"), {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            auto found = contexts_.find(uint32_t(a[0]));
            if (found == contexts_.end()) {
                throw GuestFault("invalid background mode DC");
            }

            if (a[1] != 1 && a[1] != 2) {
                throw GuestFault("unqualified background mode");
            }

            uint32_t previous = found->second.background_mode;
            found->second.background_mode = uint32_t(a[1]);
            return Result{previous, {}};
        },
        "gdi32.dll!SetBkMode");
    control_.host(
        loader_.resolve("gdi32.dll", "DeleteObject"), {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            const uint32_t object = uint32_t(a[0]);
            auto selected = [&](auto member) {
                for (const auto& dc : contexts_) {
                    if (dc.second.*member == object) {
                        return true;
                    }
                }

                return false;
            };
            if (auto font = fonts_.find(object); font != fonts_.end()) {
                if (selected(&DeviceContext::font)) {
                    return Result{};
                }

                fonts_.erase(font);
                return Result{1, {}};
            }

            if (auto bitmap = bitmaps_.find(object); bitmap != bitmaps_.end()) {
                if (bitmap_contexts_.count(object) || selected(&DeviceContext::bitmap)) {
                    return Result{};
                }

                c.heap.free(bitmap->second.data);
                bitmaps_.erase(bitmap);
                backdrop_bitmaps_.erase(object);
                return Result{1, {}};
            }

            if (brushes_.count(object)) {
                return Result{1, {}};
            }

            throw GuestFault("unqualified DeleteObject handle");
        },
        "gdi32.dll!DeleteObject");
    control_.host(
        loader_.resolve("gdi32.dll", "SetBkColor"), {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            auto found = contexts_.find(uint32_t(a[0]));
            if (found == contexts_.end()) {
                throw GuestFault("invalid background color DC");
            }

            if (a[1] >> 24) {
                throw GuestFault("unqualified palette background color");
            }

            uint32_t previous = found->second.background_color;
            found->second.background_color = uint32_t(a[1]);
            return Result{previous, {}};
        },
        "gdi32.dll!SetBkColor");
    control_.host(
        loader_.resolve("gdi32.dll", "GetTextMetricsA"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4, PointerDirection::Out}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            auto found = contexts_.find(uint32_t(a[0]));
            if (found == contexts_.end()) {
                throw GuestFault("invalid text metrics DC");
            }

            auto font = fonts_.find(found->second.font);
            if (font == fonts_.end()) {
                throw GuestFault("unqualified default GDI font");
            }

            const FontMetrics m = font->second.native->metrics();
            uint8_t out[56] = {};
            auto put = [&](unsigned at, uint32_t value) {
                std::memcpy(out + at, &value, 4);
            };
            put(0, m.height);
            put(4, m.ascent);
            put(8, m.descent);
            put(12, m.internal_leading);
            put(16, 0);
            put(20, m.average_width);
            put(24, m.max_width);
            put(28, m.weight);
            put(32, 0);
            put(36, 96);
            put(40, 96);
            out[44] = m.first;
            out[45] = m.last;
            out[46] = m.default_char;
            out[47] = m.break_char;
            out[48] = m.italic;
            out[51] = uint8_t((m.fixed_pitch ? 0 : 1) | 4);
            out[52] = font->second.logical[23];
            c.memory.write(uint32_t(a[1]), out, sizeof out);
            return Result{1, {}};
        },
        "gdi32.dll!GetTextMetricsA");
    control_.host(
        loader_.resolve("gdi32.dll", "GetDeviceCaps"), {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            if (!contexts_.count(uint32_t(a[0]))) {
                throw GuestFault("invalid device caps DC");
            }

            switch (uint32_t(a[1])) {
            case 12:
                return Result{32, {}}; // BITSPIXEL: native presentation is 32-bit.
            case 14:
                return Result{1, {}}; // PLANES
            case 88:
            case 90:
                return Result{96, {}}; // LOGPIXELSX/Y: SDL window, 100% scaling.
            default:
                throw GuestFault("unqualified GetDeviceCaps index " + std::to_string(uint32_t(a[1])));
            }
        },
        "gdi32.dll!GetDeviceCaps");
    control_.host(
        loader_.resolve("gdi32.dll", "SetTextColor"), {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            auto found = contexts_.find(uint32_t(a[0]));
            if (found == contexts_.end()) {
                throw GuestFault("invalid text color DC");
            }

            if (a[1] >> 24) {
                throw GuestFault("unqualified palette text color");
            }

            uint32_t previous = found->second.text_color;
            found->second.text_color = uint32_t(a[1]);
            return Result{previous, {}};
        },
        "gdi32.dll!SetTextColor");
    const Signature draw_signature{Convention::Stdcall,
                                   ReturnChannel::Eax,
                                   {{4}, {4, PointerDirection::In}, {4}, {4, PointerDirection::InOut}, {4}},
                                   ""};
    control_.host(
        loader_.resolve("user32.dll", "DrawTextA"), draw_signature,
        [this](Control& c, const std::vector<uint64_t>& a) {
            return draw_text(c, uint32_t(a[0]), guest_text(c, uint32_t(a[1]), int32_t(a[2]), false), uint32_t(a[3]),
                             uint32_t(a[4]));
        },
        "user32.dll!DrawTextA");
    control_.host(
        loader_.resolve("user32.dll", "DrawTextW"), draw_signature,
        [this](Control& c, const std::vector<uint64_t>& a) {
            return draw_text(c, uint32_t(a[0]), guest_text(c, uint32_t(a[1]), int32_t(a[2]), true), uint32_t(a[3]),
                             uint32_t(a[4]));
        },
        "user32.dll!DrawTextW");
    control_.host(
        loader_.resolve("gdi32.dll", "TextOutW"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}, {4}, {4, PointerDirection::In}, {4}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            if (int32_t(a[4]) < 0) {
                throw GuestFault("invalid TextOutW length");
            }

            auto& dc = text_context(uint32_t(a[0]));
            const auto text = guest_text(c, uint32_t(a[3]), int32_t(a[4]), true);
            paint_run(c, dc, fonts_.at(dc.font).native->render(text), int32_t(a[1]), int32_t(a[2]));
            return Result{1, {}};
        },
        "gdi32.dll!TextOutW");
    control_.host(
        loader_.resolve("gdi32.dll", "GetTextExtentPoint32W"),
        {Convention::Stdcall,
         ReturnChannel::Eax,
         {{4}, {4, PointerDirection::In}, {4}, {4, PointerDirection::Out}},
         ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            if (int32_t(a[2]) < 0) {
                throw GuestFault("invalid GetTextExtentPoint32W length");
            }

            auto& dc = text_context(uint32_t(a[0]));
            auto size = fonts_.at(dc.font).native->measure(guest_text(c, uint32_t(a[1]), int32_t(a[2]), true));
            c.memory.store32(uint32_t(a[3]), uint32_t(size.first));
            c.memory.store32(uint32_t(a[3]) + 4, uint32_t(size.second));
            return Result{1, {}};
        },
        "gdi32.dll!GetTextExtentPoint32W");
    control_.host(
        loader_.resolve("gdi32.dll", "GetKerningPairsW"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}, {4, PointerDirection::Out}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            auto& dc = text_context(uint32_t(a[0]));
            const auto pairs = fonts_.at(dc.font).native->kerning_pairs();
            if (!a[1] && !a[2]) {
                return Result{uint32_t(pairs.size()), {}};
            }

            if (!a[2] || a[1] < pairs.size()) {
                c.memory.store32(c.cpu.fs_base + 0x34, 122);
                return Result{};
            }

            for (size_t i = 0; i < pairs.size(); ++i) {
                const uint32_t at = uint32_t(a[2]) + uint32_t(i) * 8;
                c.memory.store32(at, uint32_t(pairs[i].first) | uint32_t(pairs[i].second) << 16);
                c.memory.store32(at + 4, uint32_t(pairs[i].amount));
            }

            return Result{uint32_t(pairs.size()), {}};
        },
        "gdi32.dll!GetKerningPairsW");
    control_.host(
        loader_.resolve("user32.dll", "GetDC"), {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            if (!windows_.count(uint32_t(a[0]))) {
                throw GuestFault("unqualified GetDC window");
            }

            uint32_t dc = next_dc_++;
            DeviceContext context{Bitmap{0, 0, 0, 32, 0, true}};
            context.window = uint32_t(a[0]);
            contexts_.emplace(dc, std::move(context));
            return Result{dc, {}};
        },
        "user32.dll!GetDC");
    control_.host(
        loader_.resolve("user32.dll", "ReleaseDC"), {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            auto found = contexts_.find(uint32_t(a[1]));
            if (found == contexts_.end() || found->second.window != a[0]) {
                throw GuestFault("invalid window DC release");
            }

            contexts_.erase(found);
            return Result{1, {}};
        },
        "user32.dll!ReleaseDC");
    control_.host(
        loader_.resolve("user32.dll", "LoadImageA"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4, PointerDirection::In}, {4}, {4}, {4}, {4}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            if (a[2] || a[3] || a[4] || (a[5] != 0x2010 && a[5] != 0x2000)) {
                throw GuestFault("unqualified bitmap source, size or flags");
            }

            std::vector<uint8_t> bytes;
            uint32_t header = 0, offset = 40;
            if (a[5] & 0x10) {
                if (!files_ || a[0]) {
                    throw GuestFault("unqualified file bitmap instance");
                }

                auto path = files_->read_path(ansi(uint32_t(a[1])));
                std::ifstream input(path, std::ios::binary);
                if (!input) {
                    c.memory.store32(c.cpu.fs_base + 0x34, std::filesystem::exists(path) ? 5 : 2);
                    return Result{};
                }

                bytes.assign(std::istreambuf_iterator<char>(input), {});
                if (bytes.size() < 54 || bytes[0] != 'B' || bytes[1] != 'M') {
                    throw GuestFault("invalid bitmap file header");
                }

                header = 14;
                offset = uint32_t(bytes[10]) | uint32_t(bytes[11]) << 8 | uint32_t(bytes[12]) << 16 |
                         uint32_t(bytes[13]) << 24;
            } else {
                uint32_t instance = a[0] ? uint32_t(a[0]) : layout::exe;
                uint32_t entry = resource(instance, uint32_t(a[1]), 2);
                if (!entry) {
                    c.memory.store32(c.cpu.fs_base + 0x34, 1814);
                    return Result{};
                }

                bytes.resize(c.memory.load32(entry + 4));
                c.memory.read(instance + c.memory.load32(entry), bytes.data(), bytes.size());
            }

            if (bytes.size() < header + 40) {
                throw GuestFault("truncated bitmap header");
            }

            auto word = [&](size_t offset) {
                return uint32_t(bytes[offset]) | uint32_t(bytes[offset + 1]) << 8;
            };
            auto dword = [&](size_t offset) {
                return word(offset) | word(offset + 2) << 16;
            };
            uint32_t width = dword(header + 4), height = dword(header + 8), bits = word(header + 14);
            if (dword(header) != 40 || word(header + 12) != 1 || bits != 24 || dword(header + 16) || !width ||
                !height || width > INT32_MAX || height > INT32_MAX) {
                throw GuestFault("unqualified bitmap format bpp=" + std::to_string(bits));
            }

            uint64_t pitch = (uint64_t(width) * 3 + 3) & ~uint64_t{3}, size = pitch * height;
            if (offset < header + 40 || uint64_t(offset) + size > bytes.size() || size > UINT32_MAX) {
                throw GuestFault("invalid bitmap pixel extent");
            }

            // A loading screen with an opaque wide image (MENU_BACKGROUNDS) in a frame wider than 4:3 draws the
            // image's centre, so it meets the image's sides around the inset without a seam.
            int backdrop = -1;
            if (backdrops_ && backdrops_->wide && (a[5] & 0x10) && width == 1024 && height == 768) {
                backdrop = backdrops_->loading(ansi(uint32_t(a[1])));
                if (backdrop >= 0) {
                    std::vector<uint8_t> centre(size);
                    if (backdrops_->at(size_t(backdrop)).alpha ||
                        !backdrops_->centre_bitmap(size_t(backdrop), width, height, uint32_t(pitch), centre)) {
                        backdrop = -1;
                    } else {
                        std::copy(centre.begin(), centre.end(), bytes.begin() + offset);
                    }
                }
            }

            uint32_t data = c.heap.allocate(uint32_t(size));
            c.memory.write(data, bytes.data() + offset, size);
            uint32_t handle = next_bitmap_++;
            bitmaps_.emplace(handle, Bitmap{width, height, uint32_t(pitch), 24, data});
            if (backdrop >= 0) {
                backdrop_bitmaps_[handle] = size_t(backdrop);
            }

            return Result{handle, {}};
        },
        "user32.dll!LoadImageA");
}

std::u16string Menu::guest_text(Control& c, uint32_t pointer, int32_t count, bool wide) const {
    std::u16string text;
    for (uint32_t i = 0; count < 0 || i < uint32_t(count); ++i) {
        uint16_t unit;
        if (wide) {
            uint8_t bytes[2];
            c.memory.read(pointer + i * 2, bytes, 2);
            unit = uint16_t(bytes[0] | bytes[1] << 8);
        } else {
            uint8_t byte;
            c.memory.read(pointer + i, &byte, 1);
            if (byte >= 0x80) {
                throw GuestFault("unqualified non-ASCII GDI text");
            }

            unit = byte;
        }

        if (count < 0 && !unit) {
            break;
        }

        text.push_back(char16_t(unit));
    }

    return text;
}

Menu::DeviceContext& Menu::text_context(uint32_t handle) {
    auto found = contexts_.find(handle);
    if (found == contexts_.end()) {
        throw GuestFault("invalid text DC");
    }

    if (!fonts_.count(found->second.font)) {
        throw GuestFault("unqualified default GDI font");
    }

    return found->second;
}

void Menu::paint_run(Control& c, DeviceContext& dc, const TextRun& run, int32_t x, int32_t y) {
    if (dc.window) {
        throw GuestFault("unqualified text on a window DC");
    }

    const auto& dst = dc.target;
    if (dst.bits != 24 && dst.bits != 32) {
        throw GuestFault("unqualified text target format");
    }

    const uint8_t color[3] = {uint8_t(dc.text_color >> 16), uint8_t(dc.text_color >> 8), uint8_t(dc.text_color)};
    const unsigned bytes = dst.bits / 8;
    const bool opaque = dc.background_mode == 2;
    for (int32_t row = 0; row < run.height; ++row) {
        int32_t py = y + row;
        if (py < 0 || uint32_t(py) >= dst.height) {
            continue;
        }

        uint32_t line = dst.top_down ? uint32_t(py) : dst.height - 1 - uint32_t(py);
        for (int32_t column = 0; column < run.width; ++column) {
            int32_t px = x + column;
            uint8_t alpha = run.alpha[size_t(row) * run.width + column];
            if ((!alpha && !opaque) || px < 0 || uint32_t(px) >= dst.width) {
                continue;
            }

            uint32_t at = dst.data + line * dst.pitch + uint32_t(px) * bytes;
            uint8_t pixel[4];
            c.memory.read(at, pixel, bytes);
            if (opaque) {
                for (unsigned channel = 0; channel < 3; ++channel) {
                    pixel[channel] = uint8_t(dc.background_color >> (8 * (2 - channel)));
                }
            }

            for (unsigned channel = 0; channel < 3; ++channel) {
                pixel[channel] = uint8_t((color[2 - channel] * alpha + pixel[channel] * (255 - alpha) + 127) / 255);
            }

            c.memory.write(at, pixel, bytes);
        }
    }
}

Result Menu::draw_text(Control& c, uint32_t handle, const std::u16string& requested, uint32_t rect, uint32_t format) {
    constexpr uint32_t center = 1, right = 2, vcenter = 4, bottom = 8, single = 0x20, noclip = 0x100, calc = 0x400,
                       noprefix = 0x800;
    auto& dc = text_context(handle);
    std::u16string text = requested;
    if (format & single) {
        text.erase(
            std::remove_if(text.begin(), text.end(), [](char16_t value) { return value == u'\r' || value == u'\n'; }),
            text.end());
    }

    if ((format & ~(center | right | vcenter | bottom | single | noclip | calc | noprefix)) || (format & 3) == 3 ||
        (format & 12) == 12) {
        throw GuestFault("unqualified DrawText format " + std::to_string(format));
    }

    // Mnemonic prefix: "&x" underlines x (first one only), "&&" is a literal '&', a trailing '&' is dropped.
    size_t underline = std::u16string::npos;
    if (!(format & noprefix) && text.find(u'&') != std::u16string::npos) {
        std::u16string stripped;
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] != u'&') {
                stripped.push_back(text[i]);
                continue;
            }

            if (i + 1 < text.size() && text[i + 1] == u'&') {
                stripped.push_back(u'&');
                ++i;
            } else if (i + 1 < text.size() && underline == std::u16string::npos) {
                underline = stripped.size();
            }
        }

        text = stripped;
    }

    if (const auto control = text.find_first_of(u"\r\n\t"); control != std::u16string::npos) {
        throw GuestFault("unqualified multi-line DrawText char=" + std::to_string(text[control]) +
                         " length=" + std::to_string(text.size()) + " format=" + std::to_string(format));
    }

    TextRun run = fonts_.at(dc.font).native->render(text);
    if (underline != std::u16string::npos && !(format & calc) && underline < text.size() && run.height > 2) {
        // Approximation of the GDI mnemonic underline: one pixel row near the bottom under the character span.
        const auto& font = *fonts_.at(dc.font).native;
        const int32_t from = underline ? font.render(text.substr(0, underline)).width : 0;
        const int32_t to = std::min<int32_t>(font.render(text.substr(0, underline + 1)).width, run.width);
        for (int32_t column = from; column < to; ++column) {
            run.alpha[size_t(run.height - 2) * run.width + column] = 255;
        }
    }

    const int32_t left = int32_t(c.memory.load32(rect)), top = int32_t(c.memory.load32(rect + 4)),
                  far_right = int32_t(c.memory.load32(rect + 8)), far_bottom = int32_t(c.memory.load32(rect + 12));
    int32_t x = left, y = top;
    if (format & center) {
        x = left + (far_right - left - run.width) / 2;
    } else if (format & right) {
        x = far_right - run.width;
    }

    if (format & single) {
        if (format & vcenter) {
            y = top + (far_bottom - top - run.height) / 2;
        } else if (format & bottom) {
            y = far_bottom - run.height;
        }
    }

    if (format & calc) {
        c.memory.store32(rect + 8, uint32_t(left + run.width));
        c.memory.store32(rect + 12, uint32_t(top + run.height));
        return Result{uint32_t(run.height), {}};
    }

    paint_run(c, dc, run, x, y);
    return Result{uint32_t(run.height), {}};
}
}
