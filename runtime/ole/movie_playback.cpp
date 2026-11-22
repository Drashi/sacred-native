#include "movie_playback.h"
#include "../../platform/movie_pacer.h"
#include "../../platform/window_host.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>

namespace sacred {
namespace {
using Clock = std::chrono::steady_clock;
std::atomic<int> running_movies{0};
constexpr uint64_t lead_frames = MovieDecoder::audio_rate; // keep one second of audio queued ahead
constexpr uint64_t chunk_frames = MovieDecoder::audio_rate / 10;
void put32(std::FILE* file, uint32_t value) {
    std::fwrite(&value, 4, 1, file);
}

void put16(std::FILE* file, uint16_t value) {
    std::fwrite(&value, 2, 1, file);
}
}

bool movie_playing() {
    return running_movies.load() > 0;
}

MoviePlayback::~MoviePlayback() {
    stop();
}

bool MoviePlayback::open(const std::string& host_path, const std::string& label, std::string& error) {
    decoder_ = MovieDecoder::open(host_path, error);
    if (!decoder_) {
        return false;
    }

    label_ = label;
    if (decoder_->info().has_audio && !decoder_->decode_audio(audio_)) {
        error = "audio track could not be decoded";
        decoder_.reset();
        return false;
    }

    if (decoder_->decode_errors()) {
        error = "decoder reported " + std::to_string(decoder_->decode_errors()) + " errors";
        decoder_.reset();
        return false;
    }

    return true;
}

void MoviePlayback::run() {
    if (running_ || !decoder_) {
        return;
    }

    running_ = true;
    ++running_movies;
    started_ = Clock::now();
    const auto& info = decoder_->info();
    energy_ = 0;
    for (int16_t sample : audio_) {
        energy_ += double(sample) * sample;
    }

    mixer_ = mixer_source_ ? mixer_source_() : nullptr;
    if (mixer_ && !audio_.empty()) {
        mixer_handle_ = mixer_->create_sample();
        mixer_->set_sample_pcm(mixer_handle_, audio_, MovieDecoder::audio_rate);
        mixer_nonsilent_start_ = mixer_->frames_nonsilent();
        mixer_->start(mixer_handle_);
    } else if (host_ && !audio_.empty()) {
        std::string failure;
        device_ = host_->open_audio(MovieDecoder::audio_rate, failure);
        if (!device_) {
            host_->start_audio();
            failure.clear();
            device_ = host_->open_audio(MovieDecoder::audio_rate, failure);
        }

        if (!device_) {
            log_("MOVIE_AUDIO_UNAVAILABLE file=" + label_ + " cause=" + failure +
                 " (video continues on the wall clock)");
        }
    }

    if (!mixer_handle_ && !tee_.empty() &&
        (tee_file_ = std::fopen((tee_ + ".movie-" + label_ + ".wav").c_str(), "wb"))) {
        std::fwrite("RIFF", 1, 4, tee_file_);
        put32(tee_file_, 0);
        std::fwrite("WAVEfmt ", 1, 8, tee_file_);
        put32(tee_file_, 16);
        put16(tee_file_, 1);
        put16(tee_file_, 2);
        put32(tee_file_, MovieDecoder::audio_rate);
        put32(tee_file_, MovieDecoder::audio_rate * 4);
        put16(tee_file_, 4);
        put16(tee_file_, 16);
        std::fwrite("data", 1, 4, tee_file_);
        put32(tee_file_, 0);
    }

    started_ = Clock::now();
    top_up();
    log_("MOVIE_PLAY file=" + label_ + " video=" + info.video_codec + " " + std::to_string(info.width) + "x" +
         std::to_string(info.height) + " fps=" + std::to_string(info.fps) + " audio=" + info.audio_codec +
         " duration=" + std::to_string(info.duration) + " audio_output=" +
         (mixer_handle_ ? "game-mixer"
          : device_     ? "sdl-device"
                        : "none"));
}

void MoviePlayback::stop() {
    if (!running_) {
        return;
    }

    running_ = false;
    --running_movies;
    uint64_t mixer_loud = 0;
    if (mixer_handle_) {
        mixer_loud = mixer_->frames_nonsilent() - mixer_nonsilent_start_;
        mixer_->set_status(mixer_handle_, Mixer::Stopped);
        mixer_->close(mixer_handle_);
    }

    if (device_) {
        host_->clear_audio(device_);
        host_->close_audio(device_);
        device_ = 0;
    }

    if (tee_file_) {
        uint32_t bytes = uint32_t(queued_frames_ * 4);
        std::fseek(tee_file_, 4, SEEK_SET);
        put32(tee_file_, 36 + bytes);
        std::fseek(tee_file_, 40, SEEK_SET);
        put32(tee_file_, bytes);
        std::fclose(tee_file_);
        tee_file_ = nullptr;
    }

    const uint64_t track = audio_.size() / 2;
    double rms = track ? std::sqrt(energy_ / double(track * 2)) : 0;
    std::string drops;
    for (double pts : drop_pts_) {
        drops += (drops.empty() ? "" : ",") + std::to_string(pts).substr(0, std::to_string(pts).find('.') + 3);
    }

    log_("MOVIE_END file=" + label_ + " reason=" + (ended_ ? "completed" : "stopped") + " frames_shown=" +
         std::to_string(shown_) + " frames_dropped=" + std::to_string(dropped_) + " first_drop_pts=[" + drops + "]" +
         " last_pts=" + std::to_string(last_pts_) + " max_av_error_ms=" + std::to_string(max_error_ * 1000) +
         " audio_frames_queued=" + std::to_string(mixer_handle_ ? track : queued_frames_) + " audio_nonsilent_frames=" +
         std::to_string(mixer_handle_ ? mixer_loud : nonsilent_) + " audio_rms=" + std::to_string(rms));
}

double MoviePlayback::clock() {
    if (!running_) {
        return 0;
    }

    double total = double(audio_.size() / 2) / MovieDecoder::audio_rate;
    if (mixer_handle_) {
        if (mixer_->status(mixer_handle_) == Mixer::Done) {
            if (!drained_) {
                drained_ = true;
                drain_time_ = Clock::now();
            }

            return total + std::chrono::duration<double>(Clock::now() - drain_time_).count();
        }

        return mixer_->audible_seconds(mixer_handle_);
    }

    if (!device_) {
        return std::chrono::duration<double>(Clock::now() - started_).count();
    }

    uint64_t backlog = host_->queued_audio(device_) / 4;
    if (queued_frames_ * 2 >= audio_.size() && backlog == 0) {
        if (!drained_) {
            drained_ = true;
            drain_time_ = Clock::now();
        }

        return total + std::chrono::duration<double>(Clock::now() - drain_time_).count();
    }

    return double(queued_frames_ - std::min<uint64_t>(backlog, queued_frames_)) / MovieDecoder::audio_rate;
}

void MoviePlayback::top_up() {
    if (!device_) {
        return;
    }

    uint64_t total = audio_.size() / 2;
    while (queued_frames_ < total && host_->queued_audio(device_) / 4 < lead_frames) {
        uint64_t count = std::min(chunk_frames, total - queued_frames_);
        const int16_t* data = audio_.data() + queued_frames_ * 2;
        host_->queue_audio(device_, data, uint32_t(count * 4));
        if (tee_file_) {
            std::fwrite(data, 4, count, tee_file_);
        }

        for (uint64_t i = 0; i < count * 2; ++i) {
            energy_ += double(data[i]) * data[i];
            if (std::abs(data[i]) > 64) {
                ++nonsilent_;
            }
        }

        queued_frames_ += count;
    }
}

bool MoviePlayback::audio_finished() {
    if (mixer_handle_) {
        return mixer_->status(mixer_handle_) == Mixer::Done;
    }

    if (!device_) {
        return clock() >= double(audio_.size() / 2) / MovieDecoder::audio_rate;
    }

    return queued_frames_ * 2 >= audio_.size() && host_->queued_audio(device_) == 0;
}

bool MoviePlayback::fetch(VideoFrame& into) {
    return decoder_->next_video(into, width_, height_);
}

MoviePlayback::Poll MoviePlayback::next(VideoFrame& frame, int width, int height) {
    if (!primed_) {
        width_ = width;
        height_ = height;
        has_current_ = fetch(current_);
        has_next_ = has_current_ && fetch(next_);
        primed_ = true;
    }

    for (;;) {
        top_up();
        if (!has_current_) {
            // Video is exhausted; the stream ends when the audio has played out (bounded by the clock).
            double limit = std::max(double(audio_.size() / 2) / MovieDecoder::audio_rate, last_pts_) + 1.0;
            if (audio_finished() || clock() > limit) {
                ended_ = true;
                return Poll::End;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        double now = clock();
        auto decision = decide_frame(current_.pts, has_next_, next_.pts, now);
        if (decision.kind == PacerDecision::Wait) {
            std::this_thread::sleep_for(std::chrono::duration<double>(std::min(decision.wait_seconds, 0.004)));
            continue;
        }

        if (decision.kind == PacerDecision::Drop) {
            ++dropped_;
            if (drop_pts_.size() < 12) {
                drop_pts_.push_back(current_.pts);
            }

            current_ = std::move(next_);
            has_next_ = fetch(next_);
            continue;
        }

        frame = std::move(current_);
        max_error_ = std::max(max_error_, std::fabs(now - frame.pts));
        last_pts_ = frame.pts;
        ++shown_;
        current_ = std::move(next_);
        has_current_ = has_next_;
        has_next_ = has_current_ && fetch(next_);
        return Poll::Frame;
    }
}
}
