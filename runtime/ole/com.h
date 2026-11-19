#pragma once
#include "../process/control.h"
#include "../ddraw/surface_pixels.h"
#include "movie_playback.h"
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace sacred {
class DirectDraw;
class WindowHost;
// OLE activation. The only class the game creates is DirectShow's CLSID_AMMultiMediaStream; with the SDL host and
// DirectDraw adapter present it is served natively (FFmpeg decode, SDL audio, frames written into the game's own
// DirectDraw surface). Without them it keeps the documented MOVIE_SKIPPED path (docs/notes/movie-playback.md).
class Com {
public:
    using Log = std::function<void(const std::string&)>;
    using Resolver = std::function<std::filesystem::path(const std::string&)>;
    Com(Control& control, const Loader& loader, Log log = {}, DirectDraw* draw = nullptr, WindowHost* host = nullptr,
        Resolver resolver = {}, std::string audio_tee = {}, MoviePlayback::MixerSource mixer = {})
        : control_(control), loader_(loader), log_(std::move(log)), draw_(draw), host_(host),
          resolver_(std::move(resolver)), audio_tee_(std::move(audio_tee)), mixer_(std::move(mixer)) {}
    ~Com();
    void bind();

private:
    enum class Kind : unsigned { Stream, Media, Sample };
    struct Object {
        Kind kind;
        uint32_t references = 1, parent = 0;
        // Stream
        std::unique_ptr<MoviePlayback> playback;
        bool video = false, audio = false, opened = false, running = false;
        uint32_t media = 0;
        std::string label;
        // Sample
        uint32_t surface = 0;
        PixelRect area{};
        bool ended = false;
    };
    uint32_t table(Kind kind);
    uint32_t create(Kind kind);
    Object& object(uint32_t pointer, Kind kind);
    uint32_t release(uint32_t pointer);
    Result method(Kind kind, unsigned slot, const std::vector<uint64_t>& arguments);
    Result stream_method(Object& object, unsigned slot, const std::vector<uint64_t>& arguments);
    Result media_method(uint32_t pointer, Object& object, unsigned slot, const std::vector<uint64_t>& arguments);
    Result sample_method(Object& object, unsigned slot, const std::vector<uint64_t>& arguments);
    Result skip(Control& c);
    Control& control_;
    const Loader& loader_;
    Log log_;
    DirectDraw* draw_;
    WindowHost* host_;
    Resolver resolver_;
    std::string audio_tee_;
    MoviePlayback::MixerSource mixer_;
    uint32_t tables_[3]{};
    std::map<uint32_t, uint32_t> apartments_;
    std::map<uint32_t, Object> objects_;
};
}
