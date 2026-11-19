#include "oracle.h"
#include "../process/control.h"
#include "../../platform/window_host.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace sacred {
namespace {
// Boot drivers for the checkpoints, in the tests/menu/qa script language. menu = main-menu idle (the 600-frame
// settle every menu scenario starts with). world = tests/menu/qa/dwarf-explore.script up to the loaded world with
// the Dwarf and every start dialog dismissed (captures removed).
const char* const menu_script = "frames 600\n";
const char* const world_script = "frames 600\nmove 510 288\nframes 100\ndown 1\nframes 3\nup 1\nframes 400\n"
                                 "move 530 430\nframes 100\ndown 1\nframes 3\nup 1\nframes 200\n"
                                 "move 590 714\nframes 100\ndown 1\nframes 3\nup 1\n"
                                 "waitmark treppe.bin 4000\nwaitmark wea.bin 1500\nframes 150\n"
                                 "dismiss 12 20\nframes 60\n";
constexpr uint32_t max_transfer = 4u << 20;
constexpr size_t max_trace = 1u << 20;
constexpr uint64_t screenshot_frames = 600;

struct OracleError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Json {
    enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
    bool boolean = false;
    std::string text;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> members;
    const Json* get(const std::string& key) const {
        for (const auto& member : members) {
            if (member.first == key) {
                return &member.second;
            }
        }

        return nullptr;
    }
};

class Parser {
public:
    explicit Parser(const std::string& source) : s_(source) {}
    Json document() {
        Json result = value(0);
        space();
        if (i_ != s_.size()) {
            fail("trailing characters");
        }

        return result;
    }

private:
    [[noreturn]] void fail(const char* what) const {
        throw OracleError(std::string("invalid JSON: ") + what + " at offset " + std::to_string(i_));
    }

    void space() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\r' || s_[i_] == '\n')) {
            ++i_;
        }
    }

    bool literal(const char* word) {
        const size_t length = std::strlen(word);
        if (s_.compare(i_, length, word) != 0) {
            return false;
        }

        i_ += length;
        return true;
    }

    Json value(int depth) {
        if (depth > 32) {
            fail("nesting too deep");
        }

        space();
        if (i_ >= s_.size()) {
            fail("unexpected end");
        }

        Json result;
        const char c = s_[i_];
        if (c == '{') {
            result.kind = Json::Object;
            ++i_;
            space();
            if (i_ < s_.size() && s_[i_] == '}') {
                ++i_;
                return result;
            }

            for (;;) {
                space();
                if (i_ >= s_.size() || s_[i_] != '"') {
                    fail("expected object key");
                }

                std::string key = string();
                space();
                if (i_ >= s_.size() || s_[i_] != ':') {
                    fail("expected ':'");
                }

                ++i_;
                result.members.emplace_back(std::move(key), value(depth + 1));
                space();
                if (i_ < s_.size() && s_[i_] == ',') {
                    ++i_;
                    continue;
                }

                if (i_ < s_.size() && s_[i_] == '}') {
                    ++i_;
                    return result;
                }

                fail("expected ',' or '}'");
            }
        }

        if (c == '[') {
            result.kind = Json::Array;
            ++i_;
            space();
            if (i_ < s_.size() && s_[i_] == ']') {
                ++i_;
                return result;
            }

            for (;;) {
                result.items.push_back(value(depth + 1));
                space();
                if (i_ < s_.size() && s_[i_] == ',') {
                    ++i_;
                    continue;
                }

                if (i_ < s_.size() && s_[i_] == ']') {
                    ++i_;
                    return result;
                }

                fail("expected ',' or ']'");
            }
        }

        if (c == '"') {
            result.kind = Json::String;
            result.text = string();
            return result;
        }

        if (literal("true")) {
            result.kind = Json::Bool;
            result.boolean = true;
            return result;
        }

        if (literal("false")) {
            result.kind = Json::Bool;
            return result;
        }

        if (literal("null")) {
            return result;
        }

        if (c == '-' || (c >= '0' && c <= '9')) {
            result.kind = Json::Number;
            result.text = number();
            return result;
        }

        fail("unexpected character");
    }

    std::string number() {
        const size_t begin = i_;
        auto digits = [&] {
            size_t start = i_;
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') {
                ++i_;
            }

            return i_ > start;
        };
        if (s_[i_] == '-') {
            ++i_;
        }

        if (i_ < s_.size() && s_[i_] == '0') {
            ++i_;
        } else if (!digits()) {
            fail("malformed number");
        }

        if (i_ < s_.size() && s_[i_] == '.') {
            ++i_;
            if (!digits()) {
                fail("malformed number");
            }
        }

        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) {
                ++i_;
            }

            if (!digits()) {
                fail("malformed number");
            }
        }

        return s_.substr(begin, i_ - begin);
    }

    unsigned hex4() {
        if (i_ + 4 > s_.size()) {
            fail("short \\u escape");
        }

        unsigned value = 0;
        for (int k = 0; k < 4; ++k) {
            const char h = s_[i_++];
            value <<= 4;
            if (h >= '0' && h <= '9') {
                value |= unsigned(h - '0');
            } else if (h >= 'a' && h <= 'f') {
                value |= unsigned(h - 'a' + 10);
            } else if (h >= 'A' && h <= 'F') {
                value |= unsigned(h - 'A' + 10);
            } else {
                fail("bad \\u escape");
            }
        }

        return value;
    }

    std::string string() {
        ++i_;
        std::string out;
        for (;;) {
            if (i_ >= s_.size()) {
                fail("unterminated string");
            }

            const char c = s_[i_++];
            if (c == '"') {
                return out;
            }

            if (static_cast<unsigned char>(c) < 0x20) {
                fail("control character in string");
            }

            if (c != '\\') {
                out += c;
                continue;
            }

            if (i_ >= s_.size()) {
                fail("unterminated escape");
            }

            const char e = s_[i_++];
            switch (e) {
            case '"':
            case '\\':
            case '/':
                out += e;
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case 'u': {
                unsigned code = hex4();
                if (code >= 0xd800 && code < 0xdc00 && s_.compare(i_, 2, "\\u") == 0) {
                    i_ += 2;
                    const unsigned low = hex4();
                    if (low < 0xdc00 || low >= 0xe000) {
                        fail("bad surrogate pair");
                    }

                    code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                }

                if (code < 0x80) {
                    out += char(code);
                } else if (code < 0x800) {
                    out += char(0xc0 | (code >> 6));
                    out += char(0x80 | (code & 0x3f));
                } else if (code < 0x10000) {
                    out += char(0xe0 | (code >> 12));
                    out += char(0x80 | ((code >> 6) & 0x3f));
                    out += char(0x80 | (code & 0x3f));
                } else {
                    out += char(0xf0 | (code >> 18));
                    out += char(0x80 | ((code >> 12) & 0x3f));
                    out += char(0x80 | ((code >> 6) & 0x3f));
                    out += char(0x80 | (code & 0x3f));
                }

                break;
            }
            default:
                fail("unknown escape");
            }
        }
    }

    const std::string& s_;
    size_t i_ = 0;
};

std::string quote(const std::string& value) {
    std::string out = "\"";
    for (const char c : value) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '"') {
            out += "\\\"";
        } else if (c == '\\') {
            out += "\\\\";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else if (u < 0x20) {
            char buffer[8];
            std::snprintf(buffer, sizeof buffer, "\\u%04x", u);
            out += buffer;
        } else {
            out += c;
        }
    }

    return out + "\"";
}

std::string hex(uint64_t value, int digits) {
    char buffer[24];
    std::snprintf(buffer, sizeof buffer, "\"0x%0*llx\"", digits, static_cast<unsigned long long>(value));
    return buffer;
}

uint64_t integer(const Json* value, const char* name, uint64_t limit, bool allow_negative = false) {
    if (!value) {
        throw OracleError(std::string("missing ") + name);
    }

    std::string text = value->text;
    if (value->kind != Json::Number && value->kind != Json::String) {
        throw OracleError(std::string(name) + " must be a number or a hex string");
    }

    bool negative = false;
    int base = 10;
    if (value->kind == Json::String && text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text = text.substr(2);
    } else if (!text.empty() && text[0] == '-') {
        negative = true;
        text = text.substr(1);
    }

    if (text.empty() || text.size() > 20 || !std::all_of(text.begin(), text.end(), [base](char c) {
            return base == 16 ? std::isxdigit(static_cast<unsigned char>(c)) != 0 : (c >= '0' && c <= '9');
        })) {
        throw OracleError(std::string("invalid ") + name + ": " + value->text);
    }

    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, base);
    if (errno == ERANGE || *end) {
        throw OracleError(std::string("invalid ") + name + ": " + value->text);
    }

    if (negative) {
        if (!allow_negative || parsed == 0 || parsed > (limit >> 1) + 1) {
            throw OracleError(std::string(name) + " out of range: " + value->text);
        }

        return (limit - parsed + 1) & limit;
    }

    if (parsed > limit) {
        throw OracleError(std::string(name) + " out of range: " + value->text);
    }

    return parsed;
}

bool flag(const Json* value, const char* name, bool fallback) {
    if (!value) {
        return fallback;
    }

    if (value->kind != Json::Bool) {
        throw OracleError(std::string(name) + " must be a boolean");
    }

    return value->boolean;
}

std::string text_field(const Json* value, const char* name) {
    if (!value || value->kind != Json::String) {
        throw OracleError(std::string(name) + " must be a string");
    }

    return value->text;
}

void write_all(int fd, const std::string& data) {
    size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0 && errno == EINTR) {
            continue;
        }

        if (n <= 0) {
            return;
        }

        done += size_t(n);
    }
}

struct Call {
    uint32_t target = 0;
    Signature signature;
    std::vector<uint64_t> arguments;
    std::string ret;
};
Call parse_call(const Json& request) {
    Call call;
    call.target = uint32_t(integer(request.get("va"), "va", 0xffffffffu));
    const Json* conv = request.get("conv");
    const std::string convention = conv ? text_field(conv, "conv") : "cdecl";
    if (convention == "cdecl") {
        call.signature.convention = Convention::Cdecl;
    } else if (convention == "stdcall") {
        call.signature.convention = Convention::Stdcall;
    } else if (convention == "thiscall") {
        call.signature.convention = Convention::Thiscall;
    } else if (convention == "fastcall") {
        call.signature.convention = Convention::Fastcall;
    } else {
        throw OracleError("unknown conv: " + convention);
    }

    const Json* self = request.get("this");
    if (call.signature.convention == Convention::Thiscall) {
        call.arguments.push_back(integer(self, "this", 0xffffffffu));
        call.signature.arguments.push_back({4});
    } else if (self) {
        throw OracleError("this is only valid with conv thiscall");
    }

    if (const Json* args = request.get("args")) {
        if (args->kind != Json::Array) {
            throw OracleError("args must be an array");
        }

        for (const Json& item : args->items) {
            if (item.kind == Json::Object) {
                call.arguments.push_back(integer(item.get("q"), "args[].q", ~uint64_t(0), true));
                call.signature.arguments.push_back({8});
            } else {
                call.arguments.push_back(integer(&item, "args[]", 0xffffffffu, true));
                call.signature.arguments.push_back({4});
            }
        }
    }

    const Json* ret = request.get("ret");
    call.ret = ret ? text_field(ret, "ret") : "eax";
    if (call.ret == "eax") {
        call.signature.result = ReturnChannel::Eax;
    } else if (call.ret == "edxeax") {
        call.signature.result = ReturnChannel::EdxEax;
    } else if (call.ret == "st0") {
        call.signature.result = ReturnChannel::St0;
    } else if (call.ret == "void") {
        call.signature.result = ReturnChannel::Void;
    } else {
        throw OracleError("unknown ret: " + call.ret);
    }

    call.signature.stack_bytes();
    return call;
}

// Runs one guest call on the active control and returns the response fields. Every exception (guest fault, strict
// trap, host refusal in an isolated child) becomes ok:false; Control::invoke has already restored the CPU state.
std::string run_call(const Call& call, std::optional<uint16_t> control_word, bool trace) {
    Control& control = active_control();
    const uint16_t live_word = control.cpu.fpu_cw;
    auto previous = control.observe_entry;
    std::vector<uint32_t> visited;
    bool truncated = false;
    const bool direct = Control::direct_calls();
    if (trace) {
        Control::enable_direct_calls(false); // the trace must see every call
    }

    if (trace) {
        control.observe_entry = [&](uint32_t address) {
            if (visited.size() < max_trace) {
                visited.push_back(address);
            } else {
                truncated = true;
            }

            if (previous) {
                previous(address);
            }
        };
    }

    if (control_word) {
        control.cpu.fpu_cw = *control_word;
    }

    std::string fields;
    try {
        const Result result = control.invoke(call.target, call.signature, call.arguments);
        fields = "\"ok\":true";
        if (call.ret == "eax") {
            fields += ",\"ret\":" + hex(uint32_t(result.integer), 8);
        } else if (call.ret == "edxeax") {
            fields += ",\"ret\":" + hex(result.integer, 16);
        } else if (call.ret == "st0") {
            char buffer[40];
            std::snprintf(buffer, sizeof buffer, "%.17g", ::x87_to_double(result.floating));
            fields += ",\"ret\":\"0x" + hex(result.floating.signExp, 4).substr(3, 4) +
                      hex(result.floating.signif, 16).substr(3, 16) + "\",\"f64\":" + quote(buffer);
        } else {
            fields += ",\"ret\":null";
        }
    } catch (const std::exception& error) {
        fields = "\"ok\":false,\"error\":" + quote(error.what());
    } catch (const ProcessExit& exit) {
        fields = "\"ok\":false,\"error\":" + quote("guest requested process exit code=" + std::to_string(exit.code));
    } catch (...) {
        fields = "\"ok\":false,\"error\":\"non-standard exception during guest call\"";
    }

    control.cpu.fpu_cw = live_word;
    control.observe_entry = previous;
    Control::enable_direct_calls(direct);
    if (trace) {
        fields += ",\"trace\":[";
        for (size_t i = 0; i < visited.size(); ++i) {
            fields += (i ? "," : "") + hex(visited[i], 8);
        }

        fields += std::string("],\"trace_truncated\":") + (truncated ? "true" : "false");
    }

    return fields;
}

// fork()ed child: guest-memory-only execution on a copy-on-write image of the process. The child holds the only
// thread, so it never yields the guest baton and refuses host calls (Control::isolated); it reports through a pipe
// and leaves with _exit. SIGALRM bounds the child even if the parent dies first.
std::string run_isolated(const Call& call, std::optional<uint16_t> control_word, bool trace, uint32_t timeout_ms) {
    int channel[2];
    if (::pipe(channel) != 0) {
        throw OracleError(std::string("pipe failed: ") + std::strerror(errno));
    }

    const pid_t child = ::fork();
    if (child < 0) {
        ::close(channel[0]);
        ::close(channel[1]);
        throw OracleError(std::string("fork failed: ") + std::strerror(errno));
    }

    if (child == 0) {
        ::close(channel[0]);
        sigset_t alarm_only;
        sigemptyset(&alarm_only);
        sigaddset(&alarm_only, SIGALRM);
        pthread_sigmask(SIG_UNBLOCK, &alarm_only, nullptr);
        std::signal(SIGALRM, SIG_DFL);
        ::alarm(timeout_ms / 1000 + 2);
        active_control().isolated = true;
        write_all(channel[1], run_call(call, control_word, trace));
        ::_exit(0);
    }

    ::close(channel[1]);
    std::string output;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    bool timed_out = false;
    for (;;) {
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) {
            timed_out = true;
            break;
        }

        pollfd wait{channel[0], POLLIN, 0};
        const int ready = ::poll(&wait, 1, int(std::min<long long>(left, 1000)));
        if (ready < 0 && errno == EINTR) {
            continue;
        }

        if (ready < 0) {
            break;
        }

        if (ready == 0) {
            continue;
        }

        char buffer[65536];
        const ssize_t n = ::read(channel[0], buffer, sizeof buffer);
        if (n < 0 && errno == EINTR) {
            continue;
        }

        if (n <= 0) {
            break;
        }

        output.append(buffer, size_t(n));
    }

    ::close(channel[0]);
    if (timed_out) {
        ::kill(child, SIGKILL);
    }

    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }

    if (timed_out) {
        return "\"ok\":false,\"error\":" + quote("timeout after " + std::to_string(timeout_ms) + " ms") +
               ",\"isolated\":true";
    }

    if (output.empty() || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        const std::string why = WIFSIGNALED(status) ? "signal " + std::to_string(WTERMSIG(status))
                                                    : "exit status " + std::to_string(WEXITSTATUS(status));
        return "\"ok\":false,\"error\":" + quote("isolated child ended without a result: " + why) +
               ",\"isolated\":true";
    }

    return output + ",\"isolated\":true";
}

uint32_t crc32(const uint8_t* data, size_t size, uint32_t crc = 0) {
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }

    return ~crc;
}

void be32(std::string& out, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out += char((value >> shift) & 0xff);
    }
}

void chunk(std::string& out, const char* type, const std::string& body) {
    be32(out, uint32_t(body.size()));
    std::string tagged = std::string(type, 4) + body;
    out += tagged;
    be32(out, crc32(reinterpret_cast<const uint8_t*>(tagged.data()), tagged.size()));
}

// 24/32-bit BMP (as written by the renderer capture) to an RGB PNG with stored (uncompressed) deflate blocks.
void bmp_to_png(const std::vector<unsigned char>& bmp, const std::string& path) {
    uint32_t offset;
    int32_t width, height;
    uint16_t bpp;
    std::memcpy(&offset, &bmp[10], 4);
    std::memcpy(&width, &bmp[18], 4);
    std::memcpy(&height, &bmp[22], 4);
    std::memcpy(&bpp, &bmp[28], 2);
    const int step = bpp / 8, rows = std::abs(height);
    const size_t stride = (size_t(width) * step + 3) / 4 * 4;
    std::string raw;
    raw.reserve(size_t(rows) * (size_t(width) * 3 + 1));
    for (int y = 0; y < rows; ++y) {
        const unsigned char* line = &bmp[offset + size_t(height > 0 ? rows - 1 - y : y) * stride];
        raw += '\0';
        for (int x = 0; x < width; ++x) {
            raw += char(line[x * step + 2]);
            raw += char(line[x * step + 1]);
            raw += char(line[x * step]);
        }
    }

    std::string zlib = "\x78\x01";
    uint32_t a = 1, b = 0;
    for (const char c : raw) {
        a = (a + static_cast<unsigned char>(c)) % 65521;
        b = (b + a) % 65521;
    }

    for (size_t at = 0; at < raw.size() || at == 0; at += 65535) {
        const size_t length = std::min<size_t>(65535, raw.size() - at);
        zlib += char(at + length >= raw.size() ? 1 : 0);
        zlib += char(length & 0xff);
        zlib += char(length >> 8);
        zlib += char(~length & 0xff);
        zlib += char((~length >> 8) & 0xff);
        zlib.append(raw, at, length);
        if (raw.empty()) {
            break;
        }
    }

    be32(zlib, (b << 16) | a);
    std::string png = "\x89PNG\r\n\x1a\n";
    std::string header;
    be32(header, uint32_t(width));
    be32(header, uint32_t(rows));
    header += std::string("\x08\x02\x00\x00\x00", 5);
    chunk(png, "IHDR", header);
    chunk(png, "IDAT", zlib);
    chunk(png, "IEND", "");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!(out << png) || !out.flush()) {
        throw OracleError("cannot write " + path);
    }
}
}

struct Oracle::Queue {
    std::mutex mutex;
    std::deque<std::string> lines;
};

const char* Oracle::checkpoint_script(const std::string& checkpoint) {
    if (checkpoint == "menu") {
        return menu_script;
    }

    if (checkpoint == "world") {
        return world_script;
    }

    return nullptr;
}

Oracle::Oracle(WindowHost& host, std::string checkpoint, int protocol_fd, std::filesystem::path assets,
               std::string scratch, std::function<void(const std::string&)> log)
    : host_(host), checkpoint_(std::move(checkpoint)), protocol_(protocol_fd), assets_(std::move(assets)),
      scratch_(std::move(scratch)), log_(std::move(log)), queue_(std::make_shared<Queue>()) {
    // Detached: a blocking read cannot be interrupted portably. The queue outlives the Oracle through shared_ptr.
    std::thread([queue = queue_] {
        std::string pending;
        char buffer[4096];
        for (;;) {
            const ssize_t n = ::read(STDIN_FILENO, buffer, sizeof buffer);
            if (n < 0 && errno == EINTR) {
                continue;
            }

            if (n <= 0) {
                break;
            }

            pending.append(buffer, size_t(n));
            for (size_t end; (end = pending.find('\n')) != std::string::npos; pending.erase(0, end + 1)) {
                std::lock_guard<std::mutex> lock(queue->mutex);
                queue->lines.push_back(pending.substr(0, end));
            }
        }

        std::lock_guard<std::mutex> lock(queue->mutex);
        if (!pending.empty()) {
            queue->lines.push_back(pending);
        }

        queue->lines.push_back("{\"op\":\"quit\",\"id\":\"stdin-eof\"}");
    }).detach();
}

Oracle::~Oracle() = default;

void Oracle::note(const std::string& line) {
    if (ready_.load()) {
        return;
    }

    if (line.find("QA script_complete") != std::string::npos) {
        ready_ = true;
        return;
    }

    if (line.find("RUN_FAILURE") != std::string::npos) {
        write_all(protocol_, "ORACLE_FAILED " + checkpoint_ + " " + line + "\n");
        std::_Exit(1);
    }
}

void Oracle::respond(const std::string& id, const std::string& fields) {
    write_all(protocol_, "{" + (id.empty() ? std::string() : "\"id\":" + id + ",") + fields + "}\n");
}

bool Oracle::finish_screenshot() {
    Pending& shot = *screenshot_;
    std::vector<unsigned char> data;
    if (std::ifstream in{shot.bmp, std::ios::binary}) {
        data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        bool complete = false;
        if (data.size() > 54) {
            uint32_t offset;
            int32_t w, h;
            uint16_t bpp;
            std::memcpy(&offset, &data[10], 4);
            std::memcpy(&w, &data[18], 4);
            std::memcpy(&h, &data[22], 4);
            std::memcpy(&bpp, &data[28], 2);
            complete = (bpp == 32 || bpp == 24) && w > 0 && h != 0 &&
                       data.size() >= offset + (size_t(w) * (bpp / 8) + 3) / 4 * 4 * size_t(std::abs(h));
        }

        if (!complete) {
            data.clear();
        }
    }

    if (data.empty()) {
        if (++shot.frames < screenshot_frames) {
            return false;
        }

        respond(shot.id, "\"ok\":false,\"error\":\"screenshot capture did not complete\"");
    } else {
        try {
            bmp_to_png(data, shot.png);
            int32_t w, h;
            std::memcpy(&w, &data[18], 4);
            std::memcpy(&h, &data[22], 4);
            respond(shot.id, "\"ok\":true,\"path\":" + quote(shot.png) + ",\"width\":" + std::to_string(w) +
                                 ",\"height\":" + std::to_string(std::abs(h)));
        } catch (const std::exception& error) {
            respond(shot.id, "\"ok\":false,\"error\":" + quote(error.what()));
        }
    }

    std::remove(shot.bmp.c_str());
    screenshot_.reset();
    return true;
}

void Oracle::frame() {
    if (busy_.exchange(true)) {
        return;
    }

    struct Idle {
        std::atomic<bool>& busy;
        ~Idle() { busy = false; }
    } idle{busy_};
    if (!announced_) {
        if (!ready_.load()) {
            return;
        }

        announced_ = true;
        log_("ORACLE_READY " + checkpoint_);
        write_all(protocol_, "ORACLE_READY " + checkpoint_ + "\n");
    }

    if (screenshot_ && !finish_screenshot()) {
        return;
    }

    for (;;) {
        std::string line;
        {
            std::lock_guard<std::mutex> lock(queue_->mutex);
            if (queue_->lines.empty()) {
                return;
            }

            line = std::move(queue_->lines.front());
            queue_->lines.pop_front();
        }

        serve(line);
        if (screenshot_) {
            return;
        }
    }
}

void Oracle::serve(const std::string& line) {
    if (line.find_first_not_of(" \t\r") == std::string::npos) {
        return;
    }

    std::string id;
    try {
        const Json request = Parser(line).document();
        if (request.kind != Json::Object) {
            throw OracleError("request must be a JSON object");
        }

        if (const Json* value = request.get("id")) {
            if (value->kind == Json::String) {
                id = quote(value->text);
            } else if (value->kind == Json::Number) {
                id = value->text;
            } else {
                throw OracleError("id must be a string or a number");
            }
        }

        const std::string op = text_field(request.get("op"), "op");
        Control& control = active_control();
        if (op == "invoke") {
            const Call call = parse_call(request);
            const bool trace = trace_next_;
            trace_next_ = false;
            if (flag(request.get("isolate"), "isolate", false)) {
                const Json* timeout = request.get("timeout_ms");
                const uint32_t limit = timeout ? uint32_t(integer(timeout, "timeout_ms", 3600000)) : 10000;
                if (!limit) {
                    throw OracleError("timeout_ms must be positive");
                }

                respond(id, run_isolated(call, control_word_, trace, limit));
            } else {
                if (request.get("timeout_ms")) {
                    throw OracleError("timeout_ms requires isolate:true");
                }

                respond(id, run_call(call, control_word_, trace));
            }
        } else if (op == "read") {
            const uint32_t va = uint32_t(integer(request.get("va"), "va", 0xffffffffu));
            const uint32_t length = uint32_t(integer(request.get("len"), "len", max_transfer));
            std::vector<uint8_t> bytes(length);
            if (length) {
                control.memory.read(va, bytes.data(), length);
            }

            std::string text;
            text.reserve(size_t(length) * 2);
            static const char digits[] = "0123456789abcdef";
            for (const uint8_t byte : bytes) {
                text += digits[byte >> 4];
                text += digits[byte & 15];
            }

            respond(id, "\"ok\":true,\"hex\":\"" + text + "\"");
        } else if (op == "write") {
            const uint32_t va = uint32_t(integer(request.get("va"), "va", 0xffffffffu));
            const std::string text = text_field(request.get("hex"), "hex");
            if (text.size() % 2 || text.size() / 2 > max_transfer || !std::all_of(text.begin(), text.end(), [](char c) {
                    return std::isxdigit(static_cast<unsigned char>(c)) != 0;
                })) {
                throw OracleError("hex must be an even-length hex string of at most 4 MiB");
            }

            std::vector<uint8_t> bytes(text.size() / 2);
            for (size_t i = 0; i < bytes.size(); ++i) {
                bytes[i] = uint8_t(std::stoul(text.substr(i * 2, 2), nullptr, 16));
            }

            if (!bytes.empty()) {
                control.memory.write(va, bytes.data(), bytes.size());
            }

            respond(id, "\"ok\":true,\"len\":" + std::to_string(bytes.size()));
        } else if (op == "alloc") {
            const uint32_t length = uint32_t(integer(request.get("len"), "len", max_transfer));
            if (!length) {
                throw OracleError("len must be positive");
            }

            const uint32_t va = control.heap.allocate(length);
            const std::vector<uint8_t> zero(length, 0);
            control.memory.write(va, zero.data(), length);
            respond(id, "\"ok\":true,\"va\":" + hex(va, 8));
        } else if (op == "x87cw") {
            if (const Json* set = request.get("set")) {
                if (set->kind == Json::Null) {
                    control_word_.reset();
                } else {
                    control_word_ = uint16_t(integer(set, "set", 0xffff));
                }
            }

            respond(id, "\"ok\":true,\"cw\":" + hex(control_word_.value_or(control.cpu.fpu_cw), 4) +
                            ",\"live\":" + hex(control.cpu.fpu_cw, 4) +
                            ",\"override\":" + (control_word_ ? hex(*control_word_, 4) : std::string("null")));
        } else if (op == "trace") {
            trace_next_ = flag(request.get("on"), "on", true);
            respond(id, std::string("\"ok\":true,\"on\":") + (trace_next_ ? "true" : "false"));
        } else if (op == "screenshot") {
            const std::string path = text_field(request.get("path"), "path");
            const auto target = std::filesystem::weakly_canonical(std::filesystem::absolute(path));
            const auto relative = target.lexically_relative(assets_);
            if (path.empty() || target.filename().empty() || (!relative.empty() && *relative.begin() != "..")) {
                throw OracleError("screenshot path must be a file outside the installation");
            }

            const uint32_t window = host_.first_window();
            if (!window) {
                throw OracleError("no game window");
            }

            Pending shot;
            shot.id = id;
            shot.png = target.string();
            shot.bmp = scratch_ + "/.oracle-screenshot-" + std::to_string(++screenshots_) + ".bmp";
            std::remove(shot.bmp.c_str());
            host_.capture_next(window, shot.bmp);
            screenshot_ = shot;
        } else if (op == "input") {
            const Json* events = request.get("events");
            if (!events || events->kind != Json::Array) {
                throw OracleError("events must be an array");
            }

            const uint32_t window = host_.first_window();
            if (!window) {
                throw OracleError("no game window");
            }

            const HostInputState state = host_.input_state();
            int x = state.x, y = state.y;
            for (const Json& event : events->items) {
                if (event.kind != Json::Object) {
                    throw OracleError("each event must be an object");
                }

                const std::string type = text_field(event.get("type"), "type");
                if (event.get("x")) {
                    x = int(integer(event.get("x"), "x", 0xffff));
                }

                if (event.get("y")) {
                    y = int(integer(event.get("y"), "y", 0xffff));
                }

                if (type == "move") {
                    host_.inject_mouse(window, x, y);
                } else if (type == "down" || type == "up") {
                    host_.inject_button(window, x, y, int(integer(event.get("button"), "button", 5)), type == "down");
                } else if (type == "key") {
                    host_.inject_key(window, text_field(event.get("key"), "key"),
                                     flag(event.get("down"), "down", true));
                } else if (type == "text") {
                    host_.inject_text(window, text_field(event.get("text"), "text"));
                } else {
                    throw OracleError("unknown input event type: " + type);
                }
            }

            respond(id, "\"ok\":true,\"events\":" + std::to_string(events->items.size()));
        } else if (op == "quit") {
            respond(id, "\"ok\":true");
            log_("ORACLE_QUIT");
            std::_Exit(0);
        } else {
            throw OracleError("unknown op: " + op);
        }
    } catch (const std::exception& error) {
        respond(id, "\"ok\":false,\"error\":" + quote(error.what()));
    }
}
}
