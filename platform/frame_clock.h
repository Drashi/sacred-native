#pragma once
#include <chrono>
#include <cstdint>

namespace sacred {
// The advertised virtual adapter owns a 60 Hz scanout clock. Presentation
// remains independent of physical-window scaling and host compositor timing.
// With a frame rate limit other than 60 (Settings.cfg FPS_LIMIT) the vertical
// blank the game waits on follows the limit; the advertised mode stays 60 Hz.
class FrameClock {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr uint32_t rate = 60;
    explicit FrameClock(Clock::time_point epoch = Clock::now()) : epoch_(epoch) {}
    void scanout_rate(uint32_t hertz) { scanout_ = hertz ? hertz : rate; }
    uint32_t scanout_rate() const { return scanout_; }
    Clock::time_point deadline(Clock::time_point now) const {
        using namespace std::chrono;
        const int64_t rate = scanout_;
        int64_t elapsed = duration_cast<nanoseconds>(now - epoch_).count();
        int64_t frame = (elapsed / 1000000000) * rate + (elapsed % 1000000000) * rate / 1000000000 + 1;
        int64_t fraction = ((frame % rate) * 1000000000 + rate - 1) / rate;
        return epoch_ + seconds(frame / rate) + nanoseconds(fraction);
    }

private:
    Clock::time_point epoch_;
    uint32_t scanout_ = rate;
};
}
