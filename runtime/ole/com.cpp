#include "com.h"
#include "../ddraw/ddraw.h"
#include "../../platform/window_host.h"
#include <algorithm>
#include <array>

namespace sacred {
namespace {
using Guid = std::array<uint8_t, 16>;
constexpr Guid guid(uint32_t a, uint16_t b, uint16_t c, std::array<uint8_t, 8> d) {
    return {uint8_t(a),
            uint8_t(a >> 8),
            uint8_t(a >> 16),
            uint8_t(a >> 24),
            uint8_t(b),
            uint8_t(b >> 8),
            uint8_t(c),
            uint8_t(c >> 8),
            d[0],
            d[1],
            d[2],
            d[3],
            d[4],
            d[5],
            d[6],
            d[7]};
}

constexpr std::array<uint8_t, 8> amovie{0x8f, 0xde, 0x00, 0xc0, 0x4f, 0xd9, 0x18, 0x9d};
constexpr std::array<uint8_t, 8> amovie_stream{0x8f, 0xdf, 0x00, 0xc0, 0x4f, 0xd9, 0x18, 0x9d};
// DirectShow multimedia streaming (amstream.h / mmstream.h / ddstream.h).
constexpr Guid clsid_am_multimedia_stream =
    guid(0x49c47ce5, 0x9ba4, 0x11d0, {0x82, 0x12, 0x00, 0xc0, 0x4f, 0xc3, 0x2c, 0x45});
constexpr Guid iid_am_multimedia_stream = guid(0xbebe595c, 0x9a6f, 0x11d0, amovie);
constexpr Guid iid_multimedia_stream = guid(0xb502d1bc, 0x9a57, 0x11d0, amovie);
constexpr Guid iid_media_stream = guid(0xb502d1bd, 0x9a57, 0x11d0, amovie);
constexpr Guid iid_stream_sample = guid(0xb502d1be, 0x9a57, 0x11d0, amovie);
constexpr Guid iid_dd_media_stream = guid(0xf4104fce, 0x9a70, 0x11d0, amovie);
constexpr Guid iid_dd_stream_sample = guid(0xf4104fcf, 0x9a70, 0x11d0, amovie);
constexpr Guid iid_unknown = guid(0, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46});
constexpr Guid mspid_primary_video = guid(0xa35ff56a, 0x9fda, 0x11d0, amovie_stream);
constexpr Guid mspid_primary_audio = guid(0xa35ff56b, 0x9fda, 0x11d0, amovie_stream);
constexpr uint32_t E_NOINTERFACE = 0x80004002u, CO_E_CLASSSTRING_UNREGISTERED = 0x80040154u,
                   CO_E_NOTINITIALIZED = 0x800401f0u, VFW_E_UNSUPPORTED_STREAM = 0x80040265u,
                   MS_S_ENDOFSTREAM = 0x00040003u;
// Hosted vtable tokens sit above DirectDraw's (it uses the first 0x7000 bytes of this window).
constexpr uint32_t com_tokens = layout::hle_end - 0x10000 + 0x8000;
struct Slot {
    unsigned slot;
    const char* name;
    unsigned arguments;
};
const std::vector<Slot>& slots(unsigned kind) {
    static const std::vector<Slot> stream{{0, "QueryInterface", 3},  {1, "AddRef", 1},   {2, "Release", 1},
                                          {4, "GetMediaStream", 3},  {7, "SetState", 2}, {12, "Initialize", 4},
                                          {15, "AddMediaStream", 5}, {16, "OpenFile", 3}};
    static const std::vector<Slot> media{
        {0, "QueryInterface", 3}, {1, "AddRef", 1}, {2, "Release", 1}, {13, "CreateSample", 5}};
    static const std::vector<Slot> sample{
        {0, "QueryInterface", 3}, {1, "AddRef", 1}, {2, "Release", 1}, {6, "Update", 5}, {8, "GetSurface", 3}};
    return kind == 0 ? stream : kind == 1 ? media : sample;
}

const char* const kind_names[] = {"IAMMultiMediaStream::", "IDirectDrawMediaStream::", "IDirectDrawStreamSample::"};
const unsigned slot_counts[] = {19, 15, 10};
Guid read_guid(Memory& memory, uint32_t at) {
    Guid value;
    memory.read(at, value.data(), value.size());
    return value;
}

std::string read_ansi(Memory& memory, uint32_t at) {
    std::string path;
    for (; path.size() < 260; ++at) {
        char value;
        try {
            memory.read(at, &value, 1);
        } catch (const GuestFault&) {
            return std::string();
        }

        if (!value) {
            break;
        }

        path.push_back(value);
    }

    return path;
}

std::string read_wide(Memory& memory, uint32_t at) {
    std::string path;
    for (; path.size() < 260; at += 2) {
        uint16_t value = uint16_t(memory.load32(at) & 0xffff);
        if (!value) {
            break;
        }

        if (value < 0x80) {
            path.push_back(char(value));
        } else if (value < 0x800) {
            path.push_back(char(0xc0 | (value >> 6)));
            path.push_back(char(0x80 | (value & 0x3f)));
        } else {
            path.push_back(char(0xe0 | (value >> 12)));
            path.push_back(char(0x80 | ((value >> 6) & 0x3f)));
            path.push_back(char(0x80 | (value & 0x3f)));
        }
    }

    return path;
}

bool is_movie(std::string text) {
    for (char& ch : text) {
        if (ch >= 'a' && ch <= 'z') {
            ch = char(ch - 'a' + 'A');
        }
    }

    return text.find(".WMV") != std::string::npos;
}

std::string base_name(const std::string& path) {
    size_t at = path.find_last_of("/\\");
    return at == std::string::npos ? path : path.substr(at + 1);
}
}

Com::~Com() {
    objects_.clear();
    for (uint32_t pointer : tables_) {
        if (pointer) {
            control_.heap.free(pointer);
        }
    }
}

uint32_t Com::table(Kind kind) {
    unsigned index = static_cast<unsigned>(kind);
    auto& pointer = tables_[index];
    if (!pointer) {
        pointer = control_.heap.allocate(slot_counts[index] * 4);
        for (unsigned slot = 0; slot < slot_counts[index]; ++slot) {
            control_.memory.store32(pointer + slot * 4, com_tokens + index * 0x1000 + slot * 16);
        }
    }

    return pointer;
}

uint32_t Com::create(Kind kind) {
    uint32_t pointer = control_.heap.allocate(4);
    control_.memory.store32(pointer, table(kind));
    Object value;
    value.kind = kind;
    objects_.emplace(pointer, std::move(value));
    return pointer;
}

Com::Object& Com::object(uint32_t pointer, Kind kind) {
    auto found = objects_.find(pointer);
    if (found == objects_.end() || found->second.kind != kind) {
        throw GuestFault("invalid or released DirectShow COM pointer");
    }

    return found->second;
}

uint32_t Com::release(uint32_t pointer) {
    auto found = objects_.find(pointer);
    if (found == objects_.end()) {
        throw GuestFault("invalid or released DirectShow COM pointer");
    }

    uint32_t references = --found->second.references;
    if (references) {
        return references;
    }

    Object dead = std::move(found->second);
    objects_.erase(found);
    control_.heap.free(pointer);
    if (dead.kind == Kind::Sample) {
        draw_->movie_surface_release(dead.surface);
        if (objects_.count(dead.parent)) {
            release(dead.parent);
        }
    } else if (dead.kind == Kind::Stream && dead.media && objects_.count(dead.media)) {
        release(dead.media);
    }

    return 0;
}

Result Com::skip(Control& c) {
    // The movie routine holds the ANSI path in EDI (it passes it to MultiByteToWideChar for the stream); its global
    // path buffer (0x009dfdcc) is the fallback when EDI is not a .WMV path.
    if (log_) {
        std::string path = read_ansi(c.memory, c.cpu.r[7]);
        if (!is_movie(path)) {
            path = read_ansi(c.memory, 0x9dfdcc);
        }

        log_("MOVIE_SKIPPED file=" + path +
             " cause=CLSID_MultiMediaStream unregistered (observed original path); game continues as if the movie "
             "ended");
    }

    return Result{CO_E_CLASSSTRING_UNREGISTERED, {}};
}

Result Com::method(Kind kind, unsigned slot, const std::vector<uint64_t>& a) {
    uint32_t pointer = uint32_t(a[0]);
    auto& self = object(pointer, kind);
    if (slot == 1) {
        return Result{++self.references, {}};
    }

    if (slot == 2) {
        return Result{release(pointer), {}};
    }

    if (slot == 0) {
        Guid iid = read_guid(control_.memory, uint32_t(a[1]));
        control_.memory.check(uint32_t(a[2]), 4, Write);
        bool known = iid == iid_unknown;
        if (kind == Kind::Stream) {
            known |= iid == iid_am_multimedia_stream || iid == iid_multimedia_stream;
        }

        if (kind == Kind::Media) {
            known |= iid == iid_media_stream || iid == iid_dd_media_stream;
        }

        if (kind == Kind::Sample) {
            known |= iid == iid_stream_sample || iid == iid_dd_stream_sample;
        }

        control_.memory.store32(uint32_t(a[2]), known ? pointer : 0);
        if (known) {
            ++self.references;
        }

        return Result{known ? 0u : E_NOINTERFACE, {}};
    }

    if (kind == Kind::Stream) {
        return stream_method(self, slot, a);
    }

    if (kind == Kind::Media) {
        return media_method(pointer, self, slot, a);
    }

    return sample_method(self, slot, a);
}

Result Com::stream_method(Object& self, unsigned slot, const std::vector<uint64_t>& a) {
    auto& memory = control_.memory;
    if (slot == 12) { // Initialize(StreamType, Flags, FilterGraph)
        if (a[1] || a[2] || a[3]) {
            throw GuestFault("unqualified multimedia stream type, flags or filter graph");
        }

        return Result{0, {}};
    }

    if (slot == 15) { // AddMediaStream(StreamObject, PurposeId, Flags, NewStream)
        Guid purpose = read_guid(memory, uint32_t(a[2]));
        if (a[4]) {
            throw GuestFault("unqualified AddMediaStream output");
        }

        if (purpose == mspid_primary_video && !a[3] && a[1]) {
            self.video = true;
            if (!self.media) {
                self.media = create(Kind::Media);
                objects_.at(self.media).parent = uint32_t(a[0]);
            }

            return Result{0, {}};
        }

        if (purpose == mspid_primary_audio && a[3] == 1 && !a[1]) { // AMMSF_ADDDEFAULTRENDERER
            self.audio = true;
            return Result{0, {}};
        }

        throw GuestFault("unqualified AddMediaStream purpose, flags or stream object");
    }

    if (slot == 16) { // OpenFile(wide name, Flags)
        if (a[2] || !self.video || self.opened) {
            throw GuestFault("unqualified multimedia stream OpenFile");
        }

        std::string name = read_wide(memory, uint32_t(a[1]));
        self.label = base_name(name);
        std::string error;
        self.playback = std::make_unique<MoviePlayback>(host_, mixer_, log_, audio_tee_);
        std::filesystem::path path = resolver_ ? resolver_(name) : std::filesystem::path(name);
        if (!self.playback->open(path.string(), self.label, error)) {
            self.playback.reset();
            if (log_) {
                log_("MOVIE_SKIPPED file=" + name + " cause=" + error +
                     " (documented skip path; game continues as if the movie ended)");
            }

            return Result{VFW_E_UNSUPPORTED_STREAM, {}};
        }

        self.opened = true;
        return Result{0, {}};
    }

    if (slot == 4) { // GetMediaStream(PurposeId, MediaStream**)
        if (read_guid(memory, uint32_t(a[1])) != mspid_primary_video || !self.media) {
            throw GuestFault("unqualified GetMediaStream purpose");
        }

        memory.check(uint32_t(a[2]), 4, Write);
        ++objects_.at(self.media).references;
        memory.store32(uint32_t(a[2]), self.media);
        return Result{0, {}};
    }

    if (slot == 7) { // SetState(STREAMSTATE_STOP = 0 / STREAMSTATE_RUN = 1)
        if (!self.opened) {
            throw GuestFault("SetState before OpenFile");
        }

        if (a[1] == 1) {
            self.playback->run();
            self.running = true;
        } else if (a[1] == 0) {
            self.playback->stop();
            self.running = false;
        } else {
            throw GuestFault("unqualified multimedia stream state");
        }

        return Result{0, {}};
    }

    throw GuestFault("unimplemented IAMMultiMediaStream method");
}

Result Com::media_method(uint32_t, Object& self, unsigned slot, const std::vector<uint64_t>& a) {
    auto& memory = control_.memory;
    if (slot != 13) {
        throw GuestFault("unimplemented IDirectDrawMediaStream method");
    }

    // CreateSample(Surface, Rect, Flags, Sample**). The game either passes its own texture surface (flags 0) or no
    // surface (DDSFF_PROGRESSIVERENDER): the stream then owns a frame surface of the requested size.
    if (a[3] > 1 || (a[1] && a[3]) || !objects_.count(self.parent)) {
        throw GuestFault("unqualified CreateSample flags");
    }

    memory.check(uint32_t(a[4]), 4, Write);
    auto& stream = objects_.at(self.parent);
    PixelRect area{0, 0, stream.playback->info().width, stream.playback->info().height};
    if (a[2]) {
        memory.check(uint32_t(a[2]), 16, Read);
        area = {int32_t(memory.load32(uint32_t(a[2]))), int32_t(memory.load32(uint32_t(a[2]) + 4)),
                int32_t(memory.load32(uint32_t(a[2]) + 8)), int32_t(memory.load32(uint32_t(a[2]) + 12))};
    }

    uint32_t surface = uint32_t(a[1]);
    if (surface) {
        draw_->movie_surface_retain(surface);
    } else {
        surface = draw_->movie_surface_create(uint32_t(area.right), uint32_t(area.bottom));
    }

    uint32_t pointer = create(Kind::Sample);
    auto& sample = objects_.at(pointer);
    sample.parent = self.parent;
    sample.surface = surface;
    sample.area = area;
    ++objects_.at(self.parent).references;
    memory.store32(uint32_t(a[4]), pointer);
    return Result{0, {}};
}

Result Com::sample_method(Object& self, unsigned slot, const std::vector<uint64_t>& a) {
    auto& memory = control_.memory;
    if (slot == 8) { // GetSurface(Surface**, Rect*)
        memory.check(uint32_t(a[1]), 4, Write);
        draw_->movie_surface_retain(self.surface);
        memory.store32(uint32_t(a[1]), self.surface);
        if (a[2]) {
            const uint32_t rect[] = {uint32_t(self.area.left), uint32_t(self.area.top), uint32_t(self.area.right),
                                     uint32_t(self.area.bottom)};
            memory.write(uint32_t(a[2]), rect, sizeof rect);
        }

        return Result{0, {}};
    }

    if (slot != 6) {
        throw GuestFault("unimplemented IDirectDrawStreamSample method");
    }

    // Update(Flags, Event, APC, APCData): synchronous (flags 0); returns when the next frame is due.
    if (a[1] || a[2] || a[3] || a[4]) {
        throw GuestFault("unqualified asynchronous stream sample update");
    }

    auto& stream = object(self.parent, Kind::Stream);
    if (!stream.running) {
        throw GuestFault("stream sample updated while the movie is not running");
    }

    if (self.ended) {
        return Result{MS_S_ENDOFSTREAM, {}};
    }

    VideoFrame frame;
    int width = self.area.right - self.area.left, height = self.area.bottom - self.area.top;
    if (stream.playback->next(frame, width, height) == MoviePlayback::Poll::End) {
        self.ended = true;
        return Result{MS_S_ENDOFSTREAM, {}};
    }

    draw_->movie_surface_write(self.surface, self.area, frame.bgra.data(), size_t(width) * 4);
    return Result{0, {}};
}

void Com::bind() {
    for (unsigned index = 0; index < 3; ++index) {
        for (const Slot& entry : slots(index)) {
            control_.host(
                com_tokens + index * 0x1000 + entry.slot * 16,
                {Convention::Stdcall, ReturnChannel::Eax, std::vector<Argument>(entry.arguments), ""},
                [this, kind = static_cast<Kind>(index), slot = entry.slot](Control&, const std::vector<uint64_t>& a) {
                    return this->method(kind, slot, a);
                },
                std::string(kind_names[index]) + entry.name);
        }
    }

    control_.host(
        loader_.resolve("ole32.dll", "CoCreateInstance"),
        {Convention::Stdcall,
         ReturnChannel::Eax,
         {{4, PointerDirection::In}, {4}, {4}, {4, PointerDirection::In}, {4, PointerDirection::Out}},
         ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            if (read_guid(c.memory, uint32_t(a[0])) != clsid_am_multimedia_stream ||
                read_guid(c.memory, uint32_t(a[3])) != iid_am_multimedia_stream || a[1] || a[2] != 1) {
                throw GuestFault("unqualified COM class, interface, context or aggregation");
            }

            c.memory.store32(uint32_t(a[4]), 0);
            uint32_t thread = c.memory.load32(c.cpu.fs_base + 0x24);
            if (!apartments_.count(thread)) {
                return Result{CO_E_NOTINITIALIZED, {}};
            }

            if (!draw_ || !host_) {
                return skip(c);
            }

            // DirectShow is in-process; the stream is a host object whose vtable entries are hosted tokens.
            uint32_t pointer = create(Kind::Stream);
            c.memory.store32(uint32_t(a[4]), pointer);
            return Result{0, {}};
        },
        "ole32.dll!CoCreateInstance");
    control_.host(
        loader_.resolve("ole32.dll", "CoInitialize"), {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            if (a[0]) {
                throw GuestFault("unqualified CoInitialize reserved pointer");
            }

            uint32_t thread = c.memory.load32(c.cpu.fs_base + 0x24);
            auto& count = apartments_[thread];
            uint32_t previous = count++;
            return Result{previous ? 1u : 0u, {}};
        },
        "ole32.dll!CoInitialize");
}
}
