#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sacred {
class WindowHost;

// Replays a recorded input script through the SDL event queue, the same entry point native backends use.
class QaScript {
public:
    QaScript(WindowHost& host, const std::string& path, std::string evidence, std::function<uint32_t()> window,
             const std::atomic<uint64_t>& frames, std::function<void(const std::string&)> log);
    ~QaScript();
    // Feeds a runtime log line to waitmark; safe from any thread.
    void note(const std::string& line);
    // Presents still to capture for `record N` (each one to <evidence>/rec/<present number>.bmp, drawable only).
    std::atomic<uint32_t> record{0};
    struct Button {
        int x, y;
    };

private:
    std::vector<Button> scan_dialog_buttons(uint32_t window);
    std::mutex notes_mutex_;
    std::vector<std::string> notes_;
    size_t consumed_ = 0;
    struct Command {
        std::string name, argument;
        int x = 0, y = 0;
    };
    void run();
    WindowHost& host_;
    std::string evidence_;
    std::function<uint32_t()> window_;
    const std::atomic<uint64_t>& frames_;
    std::function<void(const std::string&)> log_;
    std::vector<Command> commands_;
    std::atomic<bool> stop_{false};
    std::thread worker_;
};
}
