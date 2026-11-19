#pragma once
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace sacred {
class Control;
class Memory;
struct HostEvent;

// QA call trace (SACRED_QA builds, SACRED_QA_CALLTRACE=<directory>): which guest functions run during which player
// action. From WinMain on every guest call goes through Control::dispatch (direct calls stay off) and is counted as
// an edge (call site, target). The run is cut into segments by a mark key: gap, action, gap, action, ... Each action
// takes its label from the next line of a plan file (SACRED_QA_CALLTRACE_PLAN). Shift+mark steps back: it discards
// the action being recorded, or while waiting undoes the last step, a recorded action (discarded) or a skip, as
// often as there are steps. Ctrl+mark while waiting skips a plan line. A badge in the top-left corner of the frame
// shows the state (green: waiting, red: recording) and the number of the plan line.
//
// Output, one directory per run (<directory>/<UTC time>/):
//   session.json       format, start time, mark key, plan
//   segments.jsonl     one "open" and one "close" record per segment ("discard" for a discarded earlier action,
//                      "skip"/"unskip" for a plan line): kind, label, presents, host milliseconds, player state,
//                      key and mouse presses and wheel turns with their present, notes (files opened)
//   segment-NNNN.csv   site,target,type,count,frames,first in order of first occurrence; type c: CALL (site is the
//                      instruction), h: host function, r: entered from the host (callback, thread entry; site 0),
//                      j: any other transfer through the driver (a return, a jump out of a region, a tail call;
//                      site is the start of the block it left). The runtime does not know where functions start,
//                      so a reader keeps the j edges that land on a known function entry. `frames` is the
//                      number of presents in which the edge ran, `first` the present of its first run in the
//                      segment (to line functions up with the input records).
//   hosts.csv          token,name of the host functions seen
// An open segment is also written every 30 s worth of presents, so a killed process loses little.
class CallTrace {
public:
    struct Config {
        std::string directory;         // of this run; created
        std::vector<std::string> plan; // labels of the action segments, in order
        std::string plan_file;
        int mark_key = 123; // Windows virtual key, F12
        // A press of the mark key counts once the key has been released and has been quiet this long. A held
        // Ctrl+F12 on macOS arrived as a stream of presses without the repeat flag and skipped the whole plan.
        uint64_t mark_quiet_ms = 600;
    };
    // A JSON value describing the game state at a segment boundary ("null" when there is none).
    using Snapshot = std::function<std::string()>;
    using Log = std::function<void(const std::string&)>;
    // Null unless SACRED_QA_CALLTRACE names a directory; also reads SACRED_QA_CALLTRACE_PLAN (one label per line,
    // '#' comments) and SACRED_QA_CALLTRACE_KEY (F1..F12, a letter or a digit).
    static std::unique_ptr<CallTrace> from_environment(Snapshot snapshot, Log log);
    CallTrace(Config config, Snapshot snapshot, Log log);
    ~CallTrace();
    CallTrace(const CallTrace&) = delete;
    CallTrace& operator=(const CallTrace&) = delete;
    // Guest thread, once: turns direct calls off and starts counting (segment 0, a gap). The control names the host
    // functions in hosts.csv (tests pass null).
    void start(Control* control);
    // Presenting guest thread (it holds the execution baton): applies marks, collects input, writes segments.
    void frame();
    // SDL thread, for every key, mouse-button and focus-loss event. True for the mark key, which must not reach the
    // guest.
    bool input(const HostEvent& event);
    // Any thread: a line for the record of the segment being recorded (file opens and the like).
    void note(const std::string& text);
    // Closes the open segment. Only once no guest thread runs any more.
    void finish();
    // One guest transfer; public for tests.
    void record(uint32_t site, uint32_t target, char type);
    // 'c' when the dword at esp is a return address 2..7 bytes past site (a CALL just pushed it), else 'j'. Reads
    // [esp] only when it is readable: a jump can land with ESP below the committed stack (a frame reserved with
    // SUB ESP and not touched yet, the guard page included), and a host read there faults where the game does not.
    static char transfer_type(const Memory& memory, uint32_t esp, uint32_t site);
    bool recording_action() const { return action_; }
    uint32_t segment() const { return segment_; }

private:
    struct Edge {
        uint32_t site = 0, target = 0, count = 0, frames = 0, last_frame = ~0u, first_frame = 0;
        char type = 0;
    };
    struct Pending {
        char type; // k: key, m: mouse button, w: wheel, n: note, M: mark, U: undo, S: skip
        int value = 0, x = 0, y = 0;
        uint64_t host_ms = 0;
        std::string text;
    };
    static void hook(Control& control, uint32_t target, uint8_t kinds, uint8_t kind);
    size_t slot(uint32_t site, uint32_t target) const;
    void grow();
    void open_segment(bool action);
    void close_segment(bool discarded);
    void write_edges();
    void mark();
    void undo();
    void skip();
    void badge();
    uint64_t host_ms() const;
    std::string label() const;
    std::string label_of(uint32_t line) const;

    static inline CallTrace* active_ = nullptr;
    Config config_;
    Snapshot snapshot_;
    Log log_;
    Control* control_ = nullptr;
    std::chrono::steady_clock::time_point started_;
    std::vector<Edge> table_;
    size_t used_ = 0;
    unsigned shift_ = 0;
    std::vector<uint32_t> touched_; // slots with a count in this segment, in order of first occurrence
    std::vector<uint32_t> named_hosts_;
    uint64_t calls_ = 0;
    uint32_t frame_ = 0, checkpoint_frame_ = 0;
    uint32_t segment_ = 0, plan_next_ = 0;
    std::vector<int64_t> steps_; // plan steps taken, for undo: the segment of a recorded action, -1 for a skip
    bool running_ = false, action_ = false, shift_held_ = false, control_held_ = false, mark_held_ = false;
    uint64_t mark_seen_ms_ = 0;
    bool mark_seen_ = false;
    std::string records_; // inputs and notes of the open segment, as JSON array elements
    unsigned record_count_ = 0, records_dropped_ = 0;
    std::ofstream segments_;
    std::mutex mutex_;
    std::vector<Pending> pending_;
};
}
