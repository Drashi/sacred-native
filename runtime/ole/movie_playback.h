#pragma once
#include "../../platform/movie_decoder.h"
#include "../miles/mixer.h"
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sacred {
class WindowHost;
// One playing movie: FFmpeg decoder + SDL audio queue + A/V pacing. The audio device position is the master clock
// (wall clock when no audio device exists); video frames are held until due and dropped when stale.
class MoviePlayback {
public:
    using Log = std::function<void(const std::string&)>;
    enum class Poll { Frame, End };
    using MixerSource = std::function<Mixer*()>;
    // Audio goes through the game's shared mixer when it exists (SOUND 1); otherwise through a private SDL device.
    MoviePlayback(WindowHost* host, MixerSource mixer, Log log, std::string tee_path)
        : host_(host), mixer_source_(std::move(mixer)), log_(std::move(log)), tee_(std::move(tee_path)) {}
    ~MoviePlayback();
    // Opens the file and decodes its audio track; false (with `error`) leaves the caller on the documented skip path.
    bool open(const std::string& host_path, const std::string& label, std::string& error);
    const MovieInfo& info() const { return decoder_->info(); }
    void run();  // DirectShow STREAMSTATE_RUN: open the audio device and start the clock
    void stop(); // STREAMSTATE_STOP (also normal end, skip and release): silence audio, log the summary once
    // Blocks until the next video frame is due, scaling it to width x height. End once the last frame and the
    // audio have both finished.
    Poll next(VideoFrame& frame, int width, int height);

private:
    double clock();
    void top_up();
    bool audio_finished();
    bool fetch(VideoFrame& into);
    WindowHost* host_;
    MixerSource mixer_source_;
    Mixer* mixer_ = nullptr;
    uint32_t mixer_handle_ = 0;
    uint64_t mixer_nonsilent_start_ = 0;
    Log log_;
    std::string tee_, label_;
    std::unique_ptr<MovieDecoder> decoder_;
    std::vector<int16_t> audio_;
    uint64_t queued_frames_ = 0, nonsilent_ = 0;
    double energy_ = 0;
    uint32_t device_ = 0;
    bool running_ = false, primed_ = false, has_current_ = false, has_next_ = false, ended_ = false, drained_ = false;
    int width_ = 0, height_ = 0;
    VideoFrame current_, next_;
    std::chrono::steady_clock::time_point started_, drain_time_;
    uint64_t shown_ = 0, dropped_ = 0;
    double max_error_ = 0, last_pts_ = 0;
    std::vector<double> drop_pts_;
    std::FILE* tee_file_ = nullptr;
};
}
