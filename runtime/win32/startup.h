#pragma once
#include "../process/control.h"
#include "../../platform/file_metadata.h"
#include "host_errors.h"
#include <set>
#include <filesystem>
#include <memory>

namespace sacred {
enum class StartupApi {
#define API(id, dll, symbol, count, result, in, out) id,
#include "startup.def"
#undef API
};
struct StartupEnvironment {
    std::string assets;
    std::string command_line;
    std::vector<std::string> environment;
    // Optional deterministic oracle inputs; otherwise use native clocks.
    std::function<uint64_t()> file_time;
    std::function<uint32_t()> tick_count;
    // Zero selects the actual host fd type; nonzero replays an oracle launch.
    std::array<uint32_t, 3> standard_types{};
    std::string profile;
    // QA-only (--qa-fast-clock N): scale runtime time sources and wait timeouts by N. 1 = real time.
    uint32_t time_scale = 1;
    // QA-only (set with --qa-script): runtime progress markers such as QA_FILE_OPEN; no game-logic effect.
    std::function<void(const std::string&)> qa_marker;
    // QA (SACRED_QA_FRAME_DT_US): a virtual counter in nanoseconds replacing the host clock, and a Sleep observer.
    std::function<uint64_t()> counter_ns;
    std::function<void(uint32_t)> on_sleep;
};
// Process-owned startup objects. Execution remains serialized by Control.
class Startup {
public:
    Startup(Control& control, const Loader& loader, StartupEnvironment environment);
    ~Startup();
    void bind();
    static Signature signature(StartupApi api);
    static const char* symbol(StartupApi api);
    Result call(StartupApi api, const std::vector<uint64_t>& arguments);
    std::filesystem::path read_path(const std::string& name) { return file_path(name, false, 3); }
    std::filesystem::path write_path(const std::string& name) { return file_path(name, true, 2); }
    // The installation's own file, never the profile overlay's replacement.
    std::filesystem::path install_path(const std::string& name) { return file_path(name, false, 3, false); }
    bool release_crt(uint32_t pointer) { return release(0, pointer) != 0; }
    // Stops the scheduler and joins every guest thread; call before any HLE subsystem is destroyed.
    void shutdown_threads();

private:
    Control& control_;
    const Loader& loader_;
    StartupEnvironment environment_;
    uint32_t command_line_ = 0, next_handle_ = 0xe0000000;
    struct Block {
        uint32_t heap, size;
    };
    struct Handle {
        int fd;
        uint32_t type;
        std::string name;
    };
    struct Mutex {
        uint32_t owner = 0, recursion = 0;
    };
    struct Semaphore {
        uint32_t count = 0, maximum = 0;
    };
    std::map<uint32_t, std::shared_ptr<Semaphore>> semaphores_;
    struct Event {
        bool manual = false, signaled = false;
    };
    std::map<uint32_t, std::shared_ptr<Event>> events_;
    std::map<uint32_t, uint32_t> virtual_blocks_;
    struct Critical {
        uint32_t owner = 0, recursion = 0, spin = 0;
    };
    std::map<uint32_t, Block> blocks_;
    std::map<uint32_t, uint32_t> heaps_;
    std::map<uint32_t, Handle> handles_;
    std::map<uint32_t, uint64_t> mappings_;
    std::map<uint32_t, uint32_t> views_;
    std::map<uint32_t, std::shared_ptr<Mutex>> mutexes_;
    struct FindEntry {
        std::string name;
        FileMetadata metadata;
    };
    struct Find {
        std::vector<FindEntry> entries;
        size_t next = 1;
    };
    std::map<uint32_t, Find> finds_;
    void find_data(const FindEntry& entry, uint32_t destination);
    std::map<uint32_t, Critical> critical_;
    struct GuestThread;
    std::map<uint32_t, std::shared_ptr<GuestThread>> thread_handles_;
    std::vector<std::shared_ptr<GuestThread>> threads_;
    uint32_t next_thread_ = 2;
    std::vector<uint32_t> free_thread_ids_;
    void reap_threads();
    void bind_threads();
    void bind_strings();
    void bind_virtual();
    void bind_locale();
    void bind_system();
    bool imported(const char* dll, const char* symbol) const;
    std::map<uint32_t, uint32_t> page_protection_;
    void start_thread(const std::shared_ptr<GuestThread>& guest);
    void run_thread(std::shared_ptr<GuestThread> guest);
    bool wait_for(const std::function<bool()>& ready, uint32_t milliseconds);
    bool wait_thread(uint32_t handle, uint32_t milliseconds, uint32_t& result);
    std::vector<uint32_t> fls_callbacks_;
    uint32_t tls_count_ = 0;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> fls_values_;
    std::set<uint32_t> environments_;
    std::map<uint32_t, uint32_t> timer_periods_;
    std::string ansi(uint32_t pointer, int32_t length = -1) const;
    std::vector<uint16_t> wide(uint32_t pointer, int32_t length) const;
    void put_ansi(uint32_t pointer, const std::string& value);
    uint32_t error(uint32_t code, uint32_t result = 0);
    uint32_t allocate(uint32_t heap, uint32_t flags, uint32_t size);
    uint32_t release(uint32_t heap, uint32_t pointer);
    uint32_t thread_id() const;
    void publish_critical(uint32_t address, const Critical& state);
    std::filesystem::path file_path(const std::string& name, bool writing, uint32_t disposition, bool overlay = true,
                                    bool want_file = false);
    uint64_t file_time() const;
    uint64_t counter() const;
};
}
