#include "mixer.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

#define MINIMP3_NO_STDIO
#define MINIMP3_IMPLEMENTATION
#include <minimp3_ex.h>

namespace sacred {
struct Mixer::Source {
    std::vector<uint8_t> bytes;
    mp3dec_ex_t decoder{};
    bool decoder_open = false, eof = false;
    int hz = 44100;
    int64_t total_frames = 0;
    std::vector<int16_t> pcm; // decoded stereo frames at `hz`, kept so looping needs no re-decode
    double cursor = 0;        // source frames
    uint32_t status = Mixer::Stopped, eos = 0;
    int32_t rate_hz = 0;           // 0: native rate
    int32_t loops = 1, played = 0; // 0 loops forever
    float volume = 1.0f, pan = 0.5f;
    ~Source() {
        if (decoder_open) {
            mp3dec_ex_close(&decoder);
        }
    }

    // Decodes until at least `frames` stereo frames exist (or the file ends).
    void fill(size_t frames) {
        std::vector<mp3d_sample_t> chunk(4608);
        while (!eof && pcm.size() / 2 < frames) {
            const size_t got = mp3dec_ex_read(&decoder, chunk.data(), chunk.size());
            if (!got) {
                eof = true;
                break;
            }

            const int channels = std::max(decoder.info.channels, 1);
            for (size_t i = 0; i + size_t(channels) <= got; i += size_t(channels)) {
                const int16_t left = chunk[i], right = channels > 1 ? chunk[i + 1] : chunk[i];
                pcm.push_back(left);
                pcm.push_back(right);
            }
        }
    }
};

Mixer::Mixer(Sink sink, Backlog backlog, const std::string& tee_path)
    : sink_(std::move(sink)), backlog_(std::move(backlog)) {
    if (!tee_path.empty()) {
        unsigned char header[44] = {};
        tee_ = std::fopen(tee_path.c_str(), "wb");
        if (tee_) {
            std::fwrite(header, 1, sizeof header, tee_);
        }

        const size_t dot = tee_path.rfind(".wav");
        tee_sfx_ = std::fopen(
            (dot == std::string::npos ? tee_path + ".sfx" : tee_path.substr(0, dot) + ".sfx.wav").c_str(), "wb");
        if (tee_sfx_) {
            std::fwrite(header, 1, sizeof header, tee_sfx_);
        }
    }

    thread_ = std::thread([this] { run(); });
}

Mixer::~Mixer() {
    stop_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }

    write_headers();
    for (std::FILE* file : {tee_, tee_sfx_}) {
        if (file) {
            std::fclose(file);
        }
    }
}

// Rewrites the 44-byte WAV header with the frames mixed so far; called periodically so a killed run still leaves valid
// files.
void Mixer::write_headers() {
    for (std::FILE* file : {tee_, tee_sfx_}) {
        if (!file) {
            continue;
        }

        const uint32_t data = uint32_t(mixed_ * 4), riff = data + 36, fmt = 16, byte_rate = rate * 4, srate = rate;
        const uint16_t pcmfmt = 1, channels = 2, align = 4, bits = 16;
        unsigned char h[44];
        std::memcpy(h, "RIFF", 4);
        std::memcpy(h + 4, &riff, 4);
        std::memcpy(h + 8, "WAVEfmt ", 8);
        std::memcpy(h + 16, &fmt, 4);
        std::memcpy(h + 20, &pcmfmt, 2);
        std::memcpy(h + 22, &channels, 2);
        std::memcpy(h + 24, &srate, 4);
        std::memcpy(h + 28, &byte_rate, 4);
        std::memcpy(h + 32, &align, 2);
        std::memcpy(h + 34, &bits, 2);
        std::memcpy(h + 36, "data", 4);
        std::memcpy(h + 40, &data, 4);
        std::fflush(file);
        std::fseek(file, 0, SEEK_SET);
        std::fwrite(h, 1, sizeof h, file);
        std::fflush(file);
        std::fseek(file, 0, SEEK_END);
    }
}

bool Mixer::is_mp3(const std::vector<uint8_t>& bytes) {
    return bytes.size() > 4 && mp3dec_detect_buf(bytes.data(), bytes.size()) == 0;
}

bool Mixer::decode_mp3(const std::vector<uint8_t>& bytes, std::vector<int16_t>& stereo, unsigned& hz) {
    mp3dec_t decoder;
    mp3dec_file_info_t info{};
    mp3dec_init(&decoder);
    if (mp3dec_load_buf(&decoder, bytes.data(), bytes.size(), &info, nullptr, nullptr) || !info.buffer ||
        info.channels < 1 || info.channels > 2) {
        std::free(info.buffer);
        return false;
    }

    stereo.clear();
    stereo.reserve(info.samples / size_t(info.channels) * 2);
    for (size_t i = 0; i + size_t(info.channels) <= info.samples; i += size_t(info.channels)) {
        stereo.push_back(info.buffer[i]);
        stereo.push_back(info.channels > 1 ? info.buffer[i + 1] : info.buffer[i]);
    }

    hz = unsigned(info.hz);
    std::free(info.buffer);
    return true;
}

uint32_t Mixer::open_mp3_stream(std::vector<uint8_t> bytes, std::string& error) {
    auto source = std::make_shared<Source>();
    source->bytes = std::move(bytes);
    if (mp3dec_ex_open_buf(&source->decoder, source->bytes.data(), source->bytes.size(), MP3D_SEEK_TO_SAMPLE)) {
        error = "not a decodable MP3 stream";
        return 0;
    }

    source->decoder_open = true;
    source->hz = source->decoder.info.hz;
    source->total_frames = int64_t(source->decoder.samples) / std::max(source->decoder.info.channels, 1);
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t handle = next_++;
    sources_.emplace(handle, std::move(source));
    return handle;
}

bool Mixer::close(uint32_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    return sources_.erase(handle) != 0;
}

bool Mixer::valid(uint32_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    return sources_.count(handle) != 0;
}
#define WITH(handle, body)                                                                                             \
    std::lock_guard<std::mutex> lock(mutex_);                                                                          \
    auto found = sources_.find(handle);                                                                                \
    if (found == sources_.end())                                                                                       \
        return;                                                                                                        \
    Source& s = *found->second;                                                                                        \
    body
void Mixer::set_status(uint32_t handle, Status status) {
    WITH(handle, s.status = status;)
}

void Mixer::start(uint32_t handle) {
    WITH(
        handle, if (s.status == Free) return; if (s.status == Done) {
            s.cursor = 0;
            s.played = 0;
        } s.status = Playing;)
}

uint32_t Mixer::status(uint32_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto f = sources_.find(handle);
    return f == sources_.end() ? Free : f->second->status;
}

double Mixer::audible_seconds(uint32_t handle) {
    const double queued = double(backlog_ ? backlog_() : 0);
    std::lock_guard<std::mutex> lock(mutex_);
    auto f = sources_.find(handle);
    if (f == sources_.end()) {
        return 0;
    }

    return std::max(0.0, f->second->cursor - queued) / double(f->second->hz);
}

void Mixer::set_loops(uint32_t handle, int32_t loops) {
    WITH(handle, s.loops = loops; s.played = 0;)
}

void Mixer::set_volume_pan(uint32_t handle, float volume, float pan) {
    WITH(handle, s.volume = volume; s.pan = pan;)
}

void Mixer::volume_pan(uint32_t handle, float& volume, float& pan) {
    WITH(handle, volume = s.volume; pan = s.pan;)
}

uint32_t Mixer::set_eos(uint32_t handle, uint32_t callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = sources_.find(handle);
    if (found == sources_.end()) {
        return 0;
    }

    const uint32_t previous = found->second->eos;
    found->second->eos = callback;
    return previous;
}

uint32_t Mixer::create_sample() {
    auto source = std::make_shared<Source>();
    source->eof = true;
    source->status = Done; // an allocated sample reports SMP_DONE until a file is set and it is started
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t handle = next_++;
    sources_.emplace(handle, std::move(source));
    return handle;
}

void Mixer::init_sample(uint32_t handle) {
    WITH(handle, s.pcm.clear(); s.cursor = 0; s.status = Done; s.loops = 1; s.played = 0; s.rate_hz = 0;
         s.volume = 1.0f; s.pan = 0.5f;)
}

void Mixer::set_sample_pcm(uint32_t handle, std::vector<int16_t> stereo, unsigned hz) {
    WITH(handle, s.pcm = std::move(stereo); s.hz = int(hz); s.total_frames = int64_t(s.pcm.size() / 2); s.cursor = 0;
         s.played = 0; s.status = Stopped;)
}

void Mixer::set_rate(uint32_t handle, int32_t hz) {
    WITH(handle, s.rate_hz = hz;)
}

int32_t Mixer::playback_rate(uint32_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto f = sources_.find(handle);
    return f == sources_.end() ? 0 : (f->second->rate_hz ? f->second->rate_hz : f->second->hz);
}

void Mixer::position_ms(uint32_t handle, int32_t& total, int32_t& current) {
    WITH(handle, total = int32_t(s.total_frames * 1000 / s.hz); current = int32_t(int64_t(s.cursor) * 1000 / s.hz);)
}

void Mixer::seek_ms(uint32_t handle, int32_t ms) {
    WITH(handle, const int64_t frame = std::clamp<int64_t>(int64_t(ms) * s.hz / 1000, 0, s.total_frames);
         s.fill(size_t(frame) + 1); s.cursor = double(std::min<int64_t>(frame, int64_t(s.pcm.size() / 2)));)
}

std::vector<Mixer::Event> Mixer::take_events() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Event> taken;
    taken.swap(events_);
    return taken;
}

void Mixer::mix(int16_t* out, int16_t* sfx, size_t frames) {
    std::vector<int32_t> acc(frames * 2, 0), samples(frames * 2, 0);
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : sources_) {
        Source& s = *entry.second;
        if (s.status != Playing) {
            continue;
        }

        const float left_gain = s.volume * (s.pan <= 0.5f ? 1.0f : 2.0f * (1.0f - s.pan));
        const float right_gain = s.volume * (s.pan >= 0.5f ? 1.0f : 2.0f * s.pan);
        const double step = double(s.rate_hz ? s.rate_hz : s.hz) / double(rate);
        for (size_t i = 0; i < frames && s.status == Playing; ++i) {
            const size_t index = size_t(s.cursor);
            s.fill(index + 2);
            if (index + 1 >= s.pcm.size() / 2) {
                // End of the decoded data: loop or finish.
                if (s.loops == 0 || ++s.played < s.loops) {
                    s.cursor = 0;
                    continue;
                }

                s.status = Done;
                if (s.eos) {
                    events_.push_back({entry.first, s.eos});
                }

                break;
            }

            const double frac = s.cursor - double(index);
            const int16_t* a = &s.pcm[index * 2];
            const int16_t* b = &s.pcm[(index + 1) * 2];
            const double l = a[0] + (b[0] - a[0]) * frac, r = a[1] + (b[1] - a[1]) * frac;
            const int32_t lv = int32_t(l * left_gain), rv = int32_t(r * right_gain);
            acc[i * 2] += lv;
            acc[i * 2 + 1] += rv;
            if (!s.decoder_open) {
                samples[i * 2] += lv;
                samples[i * 2 + 1] += rv;
            }

            s.cursor += step;
        }
    }

    for (size_t i = 0; i < frames * 2; ++i) {
        out[i] = int16_t(std::clamp(acc[i], -32768, 32767));
        sfx[i] = int16_t(std::clamp(samples[i], -32768, 32767));
    }
}

void Mixer::run() {
    constexpr size_t block = 1024, target = 4096;
    std::vector<int16_t> buffer(block * 2), sfx(block * 2);
    while (!stop_) {
        if (backlog_() >= target) {
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
            continue;
        }

        mix(buffer.data(), sfx.data(), block);
        bool loud = false;
        for (int16_t v : buffer) {
            if (v > 64 || v < -64) {
                loud = true;
                break;
            }
        }

        if (loud) {
            nonsilent_ += block;
        }

        mixed_ += block;
        if ((mixed_ / block) % 43 == 0) {
            write_headers();
        }

        if (tee_) {
            std::fwrite(buffer.data(), sizeof(int16_t), buffer.size(), tee_);
        }

        if (tee_sfx_) {
            std::fwrite(sfx.data(), sizeof(int16_t), sfx.size(), tee_sfx_);
        }

        sink_(buffer.data(), block);
    }
}
}
