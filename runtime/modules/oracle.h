#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sacred {
class WindowHost;

// Headless differential-testing oracle (QA builds only, --oracle). Serves newline-delimited JSON requests read from
// stdin at guest frame boundaries (the DirectDraw present path) and writes one JSON response line per request to the
// protocol descriptor.
class Oracle {
public:
    // Embedded QA script that drives the boot to a checkpoint; nullptr for an unknown checkpoint name.
    static const char* checkpoint_script(const std::string& checkpoint);
    Oracle(WindowHost& host, std::string checkpoint, int protocol_fd, std::filesystem::path assets, std::string scratch,
           std::function<void(const std::string&)> log);
    ~Oracle();
    Oracle(const Oracle&) = delete;
    Oracle& operator=(const Oracle&) = delete;
    // Runtime log line (any thread): detects checkpoint completion and boot failure.
    void note(const std::string& line);
    // Guest frame boundary: called on the presenting guest thread, which holds the execution baton.
    void frame();
    struct Queue;

private:
    struct Pending {
        std::string id, bmp, png;
        uint64_t frames = 0;
    };
    void serve(const std::string& line);
    void respond(const std::string& id, const std::string& fields);
    bool finish_screenshot();
    WindowHost& host_;
    std::string checkpoint_;
    int protocol_;
    std::filesystem::path assets_;
    std::string scratch_;
    std::function<void(const std::string&)> log_;
    std::shared_ptr<Queue> queue_;
    std::atomic<bool> ready_{false}, busy_{false};
    bool announced_ = false, trace_next_ = false;
    std::optional<uint16_t> control_word_;
    std::optional<Pending> screenshot_;
    uint64_t screenshots_ = 0;
};
}
