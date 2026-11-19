#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sacred {
// Software mixer for the Miles adapter: 44.1 kHz stereo signed 16-bit output, rendered on its own thread and
// handed to a sink (the SDL device queue). Guest callbacks never run here; end-of-stream events are queued
// and delivered by the adapter on a guest thread.
class Mixer {
public:
    static constexpr uint32_t rate = 44100;
    // Miles status values.
    enum Status : uint32_t { Free = 1, Done = 2, Playing = 4, Stopped = 8 };
    struct Event {
        uint32_t handle, callback;
    };
    using Sink = std::function<void(const int16_t*, size_t frames)>;
    using Backlog = std::function<size_t()>;
    Mixer(Sink sink, Backlog backlog, const std::string& tee_path);
    ~Mixer();
    // Opens an MP3 stream from raw file bytes; returns 0 and sets error when the codec is not recognised.
    // Decodes a whole MP3 image to stereo 16-bit frames (the mssmp3.asi replacement for samples).
    // True when the bytes hold a real MPEG audio frame sequence (ID3 tags, zero padding or text headers are skipped).
    static bool is_mp3(const std::vector<uint8_t>& bytes);
    static bool decode_mp3(const std::vector<uint8_t>& bytes, std::vector<int16_t>& stereo, unsigned& hz);
    uint32_t open_mp3_stream(std::vector<uint8_t> bytes, std::string& error);
    bool close(uint32_t handle);
    bool valid(uint32_t handle);
    void set_status(uint32_t handle, Status status);
    void start(uint32_t handle);
    uint32_t create_sample();
    void init_sample(uint32_t handle);
    void set_sample_pcm(uint32_t handle, std::vector<int16_t> stereo, unsigned hz);
    void set_rate(uint32_t handle, int32_t hz);
    int32_t playback_rate(uint32_t handle);
    uint32_t status(uint32_t handle);
    // Seconds of the sample that have actually left the device: mixed position minus the queued backlog.
    double audible_seconds(uint32_t handle);
    void set_loops(uint32_t handle, int32_t loops);
    void set_volume_pan(uint32_t handle, float volume, float pan);
    void volume_pan(uint32_t handle, float& volume, float& pan);
    void position_ms(uint32_t handle, int32_t& total, int32_t& current);
    void seek_ms(uint32_t handle, int32_t ms);
    uint32_t set_eos(uint32_t handle, uint32_t callback);
    std::vector<Event> take_events();
    uint64_t frames_mixed() const { return mixed_; }
    uint64_t frames_nonsilent() const { return nonsilent_; }

private:
    struct Source;
    void run();
    void write_headers();
    void mix(int16_t* out, int16_t* sfx, size_t frames);
    Sink sink_;
    Backlog backlog_;
    std::mutex mutex_;
    std::map<uint32_t, std::shared_ptr<Source>> sources_;
    uint32_t next_ = 0xe8000001;
    std::vector<Event> events_;
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> mixed_{0}, nonsilent_{0};
    std::FILE* tee_ = nullptr;     // full mix
    std::FILE* tee_sfx_ = nullptr; // samples only (everything that is not a stream)
    std::thread thread_;
};
}
