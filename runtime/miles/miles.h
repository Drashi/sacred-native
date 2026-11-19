#pragma once
#include "../process/control.h"
#include "mixer.h"
#include <filesystem>
#include <functional>
#include <memory>

namespace sacred {
class WindowHost;
class Miles {
public:
    using Resolver = std::function<std::filesystem::path(const std::string&)>;
    using Log = std::function<void(const std::string&)>;
    // `resolver` maps a guest path to a host file (case-insensitive virtual filesystem); `tee` is the QA WAV tee.
    Miles(Control& control, const Loader& loader, WindowHost* host, Resolver resolver, Log log, std::string tee)
        : control_(control), loader_(loader), host_(host), resolver_(std::move(resolver)), log_(std::move(log)),
          tee_(std::move(tee)) {}
    ~Miles();
    void bind();
    // The shared software mixer once the game has opened its digital driver; null before that (and with SOUND 0).
    Mixer* mixer() { return mixer_.get(); }

private:
    using Handler = std::function<Result(Control&, const std::vector<uint64_t>&)>;
    // Registers one mss32 export; queued end-of-stream callbacks are delivered on this guest thread first.
    void reg(const char* symbol, Signature signature, Handler handler);
    void deliver_events(Control& control);
    std::string guest_string(Control& control, uint32_t address);
    Control& control_;
    const Loader& loader_;
    WindowHost* host_;
    Resolver resolver_;
    Log log_;
    std::string tee_;
    std::unique_ptr<Mixer> mixer_;
    uint32_t mixer_device_ = 0;
    bool initialized_ = false;
    std::string error_;
    std::map<uint32_t, uint32_t> drivers_;
    uint32_t next_driver_ = 0xe6000001;
};
}
