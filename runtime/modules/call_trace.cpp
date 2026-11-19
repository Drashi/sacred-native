#include "call_trace.h"
#include "../process/control.h"
#include "../../platform/renderer.h"
#include "../../platform/window_host.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>

namespace sacred {
namespace {
constexpr unsigned initial_bits = 17, max_records = 400;
constexpr uint32_t checkpoint_frames = 1800, waiting_argb = 0xff20a040u, recording_argb = 0xffe02020u;

std::string quote(const std::string& text) {
    std::string out = "\"";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += char(c);
        } else if (c < 0x20) {
            char buffer[8];
            std::snprintf(buffer, sizeof buffer, "\\u%04x", c);
            out += buffer;
        } else {
            out += char(c);
        }
    }

    return out + '"';
}

std::string hex(uint32_t value) {
    char buffer[12];
    std::snprintf(buffer, sizeof buffer, "%08x", value);
    return buffer;
}

// F1..F12, a letter or a digit as a Windows virtual key; 0 for anything else.
int virtual_key(const std::string& name) {
    if (name.size() >= 2 && (name[0] == 'F' || name[0] == 'f')) {
        const int number = std::atoi(name.c_str() + 1);
        return number >= 1 && number <= 12 ? 111 + number : 0;
    }

    if (name.size() != 1) {
        return 0;
    }

    const char c = name[0];
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 'A';
    }

    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ? c : 0;
}
}

std::unique_ptr<CallTrace> CallTrace::from_environment(Snapshot snapshot, Log log) {
    const char* root = std::getenv("SACRED_QA_CALLTRACE");
    if (!root || !*root) {
        return nullptr;
    }

    Config config;
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &utc);
    config.directory = std::string(root) + "/" + stamp;
    if (const char* plan = std::getenv("SACRED_QA_CALLTRACE_PLAN"); plan && *plan) {
        config.plan_file = plan;
        std::ifstream in(plan);
        if (!in) {
            throw GuestFault(std::string("cannot read SACRED_QA_CALLTRACE_PLAN ") + plan);
        }

        for (std::string line; std::getline(in, line);) {
            const size_t first = line.find_first_not_of(" \t\r");
            if (first == std::string::npos || line[first] == '#') {
                continue;
            }

            config.plan.push_back(line.substr(first, line.find_last_not_of(" \t\r") - first + 1));
        }
    }

    if (const char* key = std::getenv("SACRED_QA_CALLTRACE_KEY"); key && *key) {
        config.mark_key = virtual_key(key);
        if (!config.mark_key) {
            throw GuestFault(std::string("SACRED_QA_CALLTRACE_KEY is not F1..F12, a letter or a digit: ") + key);
        }
    }

    return std::make_unique<CallTrace>(std::move(config), std::move(snapshot), std::move(log));
}

CallTrace::CallTrace(Config config, Snapshot snapshot, Log log)
    : config_(std::move(config)), snapshot_(std::move(snapshot)), log_(std::move(log)),
      started_(std::chrono::steady_clock::now()), table_(size_t(1) << initial_bits), shift_(64 - initial_bits) {
    std::filesystem::create_directories(config_.directory);
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    std::strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%SZ", &utc);
    std::ofstream session(config_.directory + "/session.json");
    session << "{\"format\":1,\"started\":\"" << stamp << "\",\"mark_key\":" << config_.mark_key
            << ",\"plan_file\":" << quote(config_.plan_file) << ",\"plan\":[";
    for (size_t i = 0; i < config_.plan.size(); ++i) {
        session << (i ? "," : "") << quote(config_.plan[i]);
    }

    session << "]}\n";
    if (!session.flush()) {
        throw GuestFault("cannot write the call trace to " + config_.directory);
    }

    segments_.open(config_.directory + "/segments.jsonl");
    std::ofstream(config_.directory + "/hosts.csv") << "token,name\n";
}

CallTrace::~CallTrace() {
    finish();
}

uint64_t CallTrace::host_ms() const {
    return uint64_t(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_).count());
}

std::string CallTrace::label() const {
    if (!action_) {
        return segment_ ? "" : "startup";
    }

    return plan_next_ < config_.plan.size() ? config_.plan[plan_next_] : "action-" + std::to_string(plan_next_ + 1);
}

std::string CallTrace::label_of(uint32_t line) const {
    return line < config_.plan.size() ? config_.plan[line] : std::string("-");
}

void CallTrace::badge() {
    PresentBadge& shown = present_badge();
    shown.number = plan_next_ + 1;
    shown.argb = running_ ? (action_ ? recording_argb : waiting_argb) : 0;
}

void CallTrace::start(Control* control) {
    if (running_) {
        return;
    }

    control_ = control;
    running_ = true;
    active_ = this;
    Control::enable_direct_calls(false); // every call has to pass dispatch()
    Control::set_trace(&CallTrace::hook);
    open_segment(false);
    log_("CALLTRACE started directory=" + config_.directory + " plan=" + std::to_string(config_.plan.size()));
}

void CallTrace::finish() {
    if (!running_) {
        return;
    }

    Control::set_trace(nullptr);
    close_segment(false);
    running_ = false;
    active_ = nullptr;
    badge();
    log_("CALLTRACE finished segments=" + std::to_string(segment_ + 1));
}

void CallTrace::hook(Control& control, uint32_t target, uint8_t kinds, uint8_t kind) {
    uint32_t site = control.source_pc;
    char type;
    if (!kinds) {
        type = 'h';
    } else if (kind == CallEntry) {
        type = 'r';
        site = 0;
    } else {
        // A CALL has just pushed the address after itself; the generated code set source_pc to the CALL. Anything
        // else is a return or a jump, with source_pc at the start of the block it left. Every compiled address is
        // registered as both a call and a block entry, so a tail call cannot be told from the rest here.
        type = transfer_type(control.memory, control.cpu.r[R_ESP], site);
    }

    active_->record(site, target, type);
}

char CallTrace::transfer_type(const Memory& memory, uint32_t esp, uint32_t site) {
    // A CALL that pushed its return address made [esp] committed, so an unreadable [esp] is never a call.
    if (esp < 0x10000 || esp > 0xfffffffcu || !(memory.permissions(esp) & Read) ||
        !(memory.permissions(esp + 3) & Read)) {
        return 'j';
    }

    const uint32_t back = memory.load32(esp) - site;
    return back >= 2 && back <= 7 ? 'c' : 'j';
}

size_t CallTrace::slot(uint32_t site, uint32_t target) const {
    const uint64_t key = (uint64_t(site) << 32) | target;
    size_t index = size_t((key * 0x9e3779b97f4a7c15ull) >> shift_);
    for (;; index = (index + 1) & (table_.size() - 1)) {
        const Edge& edge = table_[index];
        if (!edge.target || (edge.target == target && edge.site == site)) {
            return index;
        }
    }
}

void CallTrace::grow() {
    std::vector<Edge> old(table_.size() * 2);
    old.swap(table_);
    --shift_;
    std::vector<std::pair<uint32_t, uint32_t>> order;
    order.reserve(touched_.size());
    for (uint32_t index : touched_) {
        order.push_back({old[index].site, old[index].target});
    }

    for (const Edge& edge : old) {
        if (edge.target) {
            table_[slot(edge.site, edge.target)] = edge;
        }
    }

    touched_.clear();
    for (const auto& key : order) {
        touched_.push_back(uint32_t(slot(key.first, key.second)));
    }
}

void CallTrace::record(uint32_t site, uint32_t target, char type) {
    ++calls_;
    size_t index = slot(site, target);
    if (!table_[index].target) {
        if ((used_ + 1) * 2 > table_.size()) {
            grow();
            index = slot(site, target);
        }

        Edge& fresh = table_[index];
        fresh.site = site;
        fresh.target = target;
        fresh.type = type;
        ++used_;
    }

    Edge& edge = table_[index];
    if (!edge.count) {
        touched_.push_back(uint32_t(index));
        edge.first_frame = frame_;
    }

    ++edge.count;
    if (edge.last_frame != frame_) {
        edge.last_frame = frame_;
        ++edge.frames;
    }
}

void CallTrace::write_edges() {
    char name[32];
    std::snprintf(name, sizeof name, "/segment-%04u.csv", segment_);
    const std::string path = config_.directory + name;
    std::string text = "site,target,type,count,frames,first\n";
    std::string hosts;
    for (uint32_t index : touched_) {
        const Edge& edge = table_[index];
        text += hex(edge.site) + ',' + hex(edge.target) + ',' + edge.type + ',' + std::to_string(edge.count) + ',' +
                std::to_string(edge.frames) + ',' + std::to_string(edge.first_frame) + '\n';
        if (edge.type != 'h' || std::binary_search(named_hosts_.begin(), named_hosts_.end(), edge.target)) {
            continue;
        }

        named_hosts_.insert(std::upper_bound(named_hosts_.begin(), named_hosts_.end(), edge.target), edge.target);
        const std::string* host = control_ ? control_->host_name(edge.target) : nullptr;
        hosts += hex(edge.target) + ',' + (host ? *host : std::string("?")) + '\n';
    }

    {
        std::ofstream out(path + ".tmp", std::ios::trunc);
        out << text;
    }

    std::filesystem::rename(path + ".tmp", path);
    if (!hosts.empty()) {
        std::ofstream(config_.directory + "/hosts.csv", std::ios::app) << hosts;
    }
}

void CallTrace::open_segment(bool action) {
    action_ = action;
    checkpoint_frame_ = frame_;
    segments_ << "{\"event\":\"open\",\"index\":" << segment_ << ",\"kind\":\"" << (action ? "action" : "gap")
              << "\",\"label\":" << quote(label());
    if (action) {
        segments_ << ",\"number\":" << plan_next_ + 1;
    }

    segments_ << ",\"present\":" << frame_ << ",\"host_ms\":" << host_ms() << ",\"state\":" << snapshot_() << "}"
              << std::endl;
    badge();
    if (action) {
        log_("CALLTRACE segment=" + std::to_string(segment_) + " recording number=" + std::to_string(plan_next_ + 1) +
             " label=" + label());
    } else {
        log_("CALLTRACE segment=" + std::to_string(segment_) + " waiting next=" + std::to_string(plan_next_ + 1) +
             " label=" + label_of(plan_next_));
    }
}

void CallTrace::close_segment(bool discarded) {
    write_edges();
    segments_ << "{\"event\":\"close\",\"index\":" << segment_ << ",\"present\":" << frame_
              << ",\"host_ms\":" << host_ms() << ",\"calls\":" << calls_ << ",\"edges\":" << touched_.size()
              << ",\"discarded\":" << (discarded ? "true" : "false") << ",\"state\":" << snapshot_() << ",\"records\":["
              << records_ << "],\"records_dropped\":" << records_dropped_ << "}" << std::endl;
    for (uint32_t index : touched_) {
        Edge& edge = table_[index];
        edge.count = 0;
        edge.frames = 0;
        edge.last_frame = ~0u;
    }

    touched_.clear();
    calls_ = 0;
    records_.clear();
    record_count_ = records_dropped_ = 0;
}

void CallTrace::mark() {
    const bool was_action = action_;
    close_segment(false);
    if (was_action) {
        steps_.push_back(segment_);
        ++plan_next_;
    }

    ++segment_;
    open_segment(!was_action);
}

void CallTrace::undo() {
    if (action_) {
        close_segment(true);
        log_("CALLTRACE segment=" + std::to_string(segment_) + " discarded");
        ++segment_;
        open_segment(false);
    } else if (!steps_.empty()) {
        const int64_t step = steps_.back();
        steps_.pop_back();
        --plan_next_;
        if (step < 0) {
            segments_ << "{\"event\":\"unskip\",\"number\":" << plan_next_ + 1 << "}" << std::endl;
            log_("CALLTRACE skip undone, next=" + std::to_string(plan_next_ + 1) + " label=" + label_of(plan_next_));
        } else {
            segments_ << "{\"event\":\"discard\",\"index\":" << step << "}" << std::endl;
            log_("CALLTRACE segment=" + std::to_string(step) + " discarded, next=" + std::to_string(plan_next_ + 1) +
                 " label=" + label_of(plan_next_));
        }

        badge();
    }
}

void CallTrace::skip() {
    if (action_ || (!config_.plan.empty() && plan_next_ >= config_.plan.size())) {
        return; // nothing left to skip
    }

    segments_ << "{\"event\":\"skip\",\"number\":" << plan_next_ + 1
              << ",\"label\":" << quote(plan_next_ < config_.plan.size() ? config_.plan[plan_next_] : std::string())
              << "}" << std::endl;
    steps_.push_back(-1);
    ++plan_next_;
    badge();
    log_("CALLTRACE skipped, next=" + std::to_string(plan_next_ + 1) + " label=" + label_of(plan_next_));
}

void CallTrace::frame() {
    if (!running_) {
        return;
    }

    ++frame_;
    std::vector<Pending> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending.swap(pending_);
    }

    for (const Pending& item : pending) {
        if (item.type == 'M') {
            mark();
            continue;
        }

        if (item.type == 'U') {
            undo();
            continue;
        }

        if (item.type == 'S') {
            skip();
            continue;
        }

        if (record_count_ >= max_records) {
            ++records_dropped_;
            continue;
        }

        if (record_count_++) {
            records_ += ',';
        }

        records_ += "{\"present\":" + std::to_string(frame_) + ",\"host_ms\":" + std::to_string(item.host_ms);
        if (item.type == 'k') {
            records_ += ",\"type\":\"key\",\"vk\":" + std::to_string(item.value);
        } else if (item.type == 'm') {
            records_ += ",\"type\":\"mouse\",\"button\":" + std::to_string(item.value) +
                        ",\"x\":" + std::to_string(item.x) + ",\"y\":" + std::to_string(item.y);
        } else if (item.type == 'w') {
            records_ += ",\"type\":\"wheel\",\"y\":" + std::to_string(item.value);
        } else {
            records_ += ",\"type\":\"note\",\"text\":" + quote(item.text);
        }

        records_ += '}';
    }

    if (frame_ - checkpoint_frame_ >= checkpoint_frames) {
        checkpoint_frame_ = frame_;
        write_edges();
    }
}

bool CallTrace::input(const HostEvent& event) {
    if (event.kind == HostEventKind::FocusLost) { // the releases go elsewhere
        shift_held_ = control_held_ = mark_held_ = false;
        return false;
    }

    const bool down = event.kind == HostEventKind::KeyDown;
    if (down || event.kind == HostEventKind::KeyUp) {
        if (event.value == 16) {
            shift_held_ = down;
        }

        if (event.value == 17) {
            control_held_ = down;
        }

        if (event.value == config_.mark_key) {
            const uint64_t now = host_ms();
            const bool quiet = !mark_seen_ || now - mark_seen_ms_ >= config_.mark_quiet_ms;
            if (down && !event.y && !mark_held_ && quiet) { // y: key repeat
                std::lock_guard<std::mutex> lock(mutex_);
                pending_.push_back({shift_held_ ? 'U' : control_held_ ? 'S' : 'M', 0, 0, 0, now, {}});
            }

            mark_held_ = down;
            mark_seen_ = true;
            mark_seen_ms_ = now;
            return true;
        }

        if (down && !event.y) {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_.push_back({'k', event.value, 0, 0, host_ms(), {}});
        }
    } else if (event.kind == HostEventKind::MouseDown) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.push_back({'m', event.value, event.x, event.y, host_ms(), {}});
    } else if (event.kind == HostEventKind::Wheel) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.push_back({'w', event.y, 0, 0, host_ms(), {}});
    }

    return false;
}

void CallTrace::note(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_.size() < 4096) {
        pending_.push_back({'n', 0, 0, 0, host_ms(), text});
    }
}
}
