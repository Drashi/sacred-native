#include "qa_script.h"
#ifdef SACRED_PGO_GENERATE
extern "C" int __llvm_profile_write_file(void);
#endif
#include "../../platform/window_host.h"
#include "../process/memory.h"
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>

namespace sacred {

namespace {
struct Cell {
    int count = 0, x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
};
}

// Green button text (OK, Skip Tutorial) inside the dialog band y 90..689, left to right. The HUD below is excluded.
std::vector<QaScript::Button> QaScript::scan_dialog_buttons(uint32_t window) {
    const std::string path = evidence_ + "/.qa-scan.bmp";
    std::remove(path.c_str());
    host_.capture_next(window, path);
    std::vector<unsigned char> data;
    for (int attempt = 0; attempt < 2000 && !stop_; ++attempt) {
        std::ifstream in(path, std::ios::binary);
        if (in) {
            data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            if (data.size() > 54) {
                uint32_t offset;
                int32_t w, h;
                uint16_t bpp;
                std::memcpy(&offset, &data[10], 4);
                std::memcpy(&w, &data[18], 4);
                std::memcpy(&h, &data[22], 4);
                std::memcpy(&bpp, &data[28], 2);
                if ((bpp == 32 || bpp == 24) && w > 0 && h != 0 &&
                    data.size() >= offset + size_t(w) * size_t(std::abs(h)) * (bpp / 8)) {
                    break;
                }
            }

            data.clear();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::remove(path.c_str());
    if (data.empty()) {
        return {};
    }

    uint32_t offset;
    int32_t w, h;
    uint16_t bpp;
    std::memcpy(&offset, &data[10], 4);
    std::memcpy(&w, &data[18], 4);
    std::memcpy(&h, &data[22], 4);
    std::memcpy(&bpp, &data[28], 2);
    const int step = bpp / 8, rows = std::abs(h);
    std::map<std::pair<int, int>, Cell> cells;
    for (int y = 90; y < 690 && y < rows; ++y) {
        const int row = h > 0 ? rows - 1 - y : y;
        const unsigned char* line = &data[offset + size_t(row) * size_t(w) * step];
        for (int x = 0; x < w; ++x) {
            const int b = line[x * step], g = line[x * step + 1], r = line[x * step + 2];
            if (g >= 150 && r <= 90 && b <= 90 && g - r >= 90) {
                Cell& cell = cells[{y / 10, x / 10}];
                ++cell.count;
                cell.x0 = std::min(cell.x0, x);
                cell.y0 = std::min(cell.y0, y);
                cell.x1 = std::max(cell.x1, x);
                cell.y1 = std::max(cell.y1, y);
            }
        }
    }

    std::vector<Button> buttons;
    std::set<std::pair<int, int>> seen;
    for (const auto& seed : cells) {
        if (seen.count(seed.first)) {
            continue;
        }

        Cell total;
        std::vector<std::pair<int, int>> stack{seed.first};
        seen.insert(seed.first);
        while (!stack.empty()) {
            const auto key = stack.back();
            stack.pop_back();
            const Cell& cell = cells[key];
            total.count += cell.count;
            total.x0 = std::min(total.x0, cell.x0);
            total.y0 = std::min(total.y0, cell.y0);
            total.x1 = std::max(total.x1, cell.x1);
            total.y1 = std::max(total.y1, cell.y1);
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -3; dx <= 3; ++dx) {
                    const std::pair<int, int> next{key.first + dy, key.second + dx};
                    if (cells.count(next) && seen.insert(next).second) {
                        stack.push_back(next);
                    }
                }
            }
        }

        if (total.count >= 30) {
            buttons.push_back({(total.x0 + total.x1) / 2, (total.y0 + total.y1) / 2});
        }
    }

    std::sort(buttons.begin(), buttons.end(), [](const Button& a, const Button& b) { return a.x < b.x; });
    return buttons;
}

QaScript::QaScript(WindowHost& host, const std::string& path, std::string evidence, std::function<uint32_t()> window,
                   const std::atomic<uint64_t>& frames, std::function<void(const std::string&)> log)
    : host_(host), evidence_(std::move(evidence)), window_(std::move(window)), frames_(frames), log_(std::move(log)) {
    std::ifstream input(path);
    if (!input) {
        throw GuestFault("cannot open QA input script");
    }

    host_.script_pointer();
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        Command command;
        if (!(fields >> command.name) || command.name[0] == '#') {
            continue;
        }

        if (command.name == "record") { // record N [LABEL]: the label goes to the log line only
            fields >> command.x;
            if (!fields.fail() && !(fields >> command.argument)) {
                fields.clear();
            }
        } else if (command.name == "frames" || command.name == "down" || command.name == "up" ||
                   command.name == "sleep" || command.name == "wheel") {
            fields >> command.x;
        } else if (command.name == "move" || command.name == "dismiss") {
            fields >> command.x >> command.y;
        } else if (command.name == "key") {
            std::string state;
            fields >> command.argument >> state;
            command.x = state == "down";
            if (state != "down" && state != "up") {
                throw GuestFault("invalid QA key state");
            }

            std::replace(command.argument.begin(), command.argument.end(), '_', ' ');
        } // Left_Alt is SDL's "Left Alt"
        else if (command.name == "capture" || command.name == "type") {
            fields >> command.argument;
        } else if (command.name == "waitmark") {
            fields >> command.argument >> command.x;
        } else if (command.name == "drive") {
            fields >> command.argument >> command.x >> command.y;
        } else {
            throw GuestFault("unknown QA command: " + command.name);
        }

        if (fields.fail()) {
            throw GuestFault("malformed QA command: " + line);
        }

        commands_.push_back(command);
    }

    worker_ = std::thread([this] { run(); });
}

void QaScript::note(const std::string& line) {
    std::lock_guard<std::mutex> lock(notes_mutex_);
    notes_.push_back(line);
}

QaScript::~QaScript() {
    stop_ = true;
    if (worker_.joinable()) {
        worker_.join();
    }
}

void QaScript::run() {
    auto pause = [] {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    };
    while (!stop_ && !window_()) {
        pause();
    }

    int x = 0, y = 0;
    for (const auto& command : commands_) {
        if (stop_) {
            return;
        }

        const uint32_t window = window_();
        if (command.name == "frames") {
            const uint64_t target = frames_ + uint64_t(command.x);
            while (!stop_ && frames_ < target) {
                pause();
            }
        } else if (command.name == "sleep") {
            // Wall-clock hold (milliseconds) for input that must last while the guest presents no frames.
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(command.x);
            while (!stop_ && std::chrono::steady_clock::now() < until) {
                pause();
            }
        } else if (command.name == "move") {
            x = command.x;
            y = command.y;
            host_.inject_mouse(window, x, y);
        } else if (command.name == "down" || command.name == "up") {
            host_.inject_button(window, x, y, command.x, command.name == "down");
        } else if (command.name == "drive") {
            // Continued pointer input for argument milliseconds around client point (x, y): a right-click and the
            // game's SetCursorPos sequence (current position, then a 31,23 displacement) every 60 steps. After every
            // step the guest-visible position must equal an independent model: previous position plus the host delta,
            // clamped.
            const auto begin = std::chrono::steady_clock::now();
            const auto end = begin + std::chrono::milliseconds(std::stoi(command.argument));
            const HostWindowState area = host_.window_state(window);
            auto clamp = [](int value, int extent) {
                return std::min(std::max(value, 0), extent - 1);
            };
            auto settle = [&](int want_x, int want_y) {
                const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                for (;;) {
                    const HostInputState state = host_.input_state();
                    if (state.x == want_x && state.y == want_y) {
                        return true;
                    }

                    if (stop_ || std::chrono::steady_clock::now() > limit) {
                        return false;
                    }

                    pause();
                }
            };
            host_.inject_mouse(window, x, y);
            int guest_x = x, guest_y = y;
            long steps = 0, warps = 0, bad = 0;
            while (!stop_ && std::chrono::steady_clock::now() < end) {
                const double angle = double(steps) * 0.05;
                const int next_x = command.x + int(150 * std::cos(angle)),
                          next_y = command.y + int(110 * std::sin(angle));
                const HostInputState before = host_.input_state();
                guest_x = clamp(before.x + next_x - x, area.width);
                guest_y = clamp(before.y + next_y - y, area.height);
                x = next_x;
                y = next_y;
                host_.inject_mouse(window, x, y);
                if (!settle(guest_x, guest_y)) {
                    ++bad;
                }

                if (++steps % 60 == 0) {
                    host_.inject_button(window, x, y, 3, true);
                    pause();
                    host_.inject_button(window, x, y, 3, false);
                    const HostInputState now = host_.input_state();
                    guest_x = clamp(now.x + 31, area.width);
                    guest_y = clamp(now.y + 23, area.height);
                    host_.set_cursor_pos(area.x + now.x, area.y + now.y);
                    host_.set_cursor_pos(area.x + guest_x, area.y + guest_y);
                    ++warps;
                    if (!settle(guest_x, guest_y)) {
                        ++bad;
                    }
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(8));
            }

            log_("CURSOR_DRIVE steps=" + std::to_string(steps) + " warps=" + std::to_string(warps) +
                 " diverged=" + std::to_string(bad) + " elapsed_ms=" +
                 std::to_string(
                     std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin)
                         .count()));
            if (bad) {
                log_("RUN_FAILURE QA guest cursor diverged from host motion");
                return;
            }
        } else if (command.name == "wheel") {
            // Wheel steps at the pointer: positive away from the user (zoom in), negative towards (zoom out).
            host_.inject_wheel(window, command.x);
        } else if (command.name == "key") {
            host_.inject_key(window, command.argument, command.x != 0);
        } else if (command.name == "waitmark") {
            // Waits for the next unconsumed runtime log line containing the text, or gives up after x presented frames.
            const uint64_t limit = frames_ + uint64_t(command.x);
            bool found = false;
            while (!stop_ && !found && frames_ < limit) {
                {
                    std::lock_guard<std::mutex> lock(notes_mutex_);
                    for (; consumed_ < notes_.size() && !found; ++consumed_) {
                        found = notes_[consumed_].find(command.argument) != std::string::npos;
                    }
                }

                if (!found) {
                    pause();
                }
            }

            if (!found && !stop_) {
                log_("RUN_FAILURE QA waitmark timeout: " + command.argument);
                return;
            }
        } else if (command.name == "dismiss") {
            // Clears modal dialogs: scan the presented frame for green button text, click the right-most button
            // (Skip Tutorial, otherwise the sole OK) and repeat until two consecutive scans are clear.
            auto wait = [&](uint64_t count) {
                const uint64_t target = frames_ + count;
                while (!stop_ && frames_ < target) {
                    pause();
                }
            };
            int id = 0, clear = 0;
            bool open = false;
            int signature = 0;
            bool done = false;
            for (int round = 0; round < command.x && !stop_ && !done; ++round) {
                const auto buttons = scan_dialog_buttons(window);
                if (buttons.empty()) {
                    if (open) {
                        log_("QA_DIALOG_CLOSE id=" + std::to_string(id));
                        open = false;
                    }

                    if (++clear >= 2) {
                        done = true;
                    } else {
                        wait(10);
                    }

                    continue;
                }

                clear = 0;
                const Button& target = buttons.back();
                const int sig = target.x * 1000 + target.y;
                if (open && sig != signature) {
                    log_("QA_DIALOG_CLOSE id=" + std::to_string(id));
                    open = false;
                }

                if (!open) {
                    open = true;
                    signature = sig;
                    ++id;
                    log_("QA_DIALOG_OPEN id=" + std::to_string(id) + " buttons=" + std::to_string(buttons.size()) +
                         " click=" + std::to_string(target.x) + "," + std::to_string(target.y));
                }

                host_.inject_mouse(window, target.x, target.y);
                wait(5);
                host_.inject_button(window, target.x, target.y, 1, true);
                wait(3);
                host_.inject_button(window, target.x, target.y, 1, false);
                wait(uint64_t(command.y));
            }

            if (open && !stop_) {
                log_("RUN_FAILURE QA dialog could not be dismissed: id=" + std::to_string(id));
                return;
            }
        } else if (command.name == "type") {
            host_.inject_text(window, command.argument);
        } else if (command.name == "record") {
            // Every present from here on, for x presents: the frames of a transition one by one.
            record = uint32_t(std::max(command.x, 0));
        } else if (command.name == "capture") {
            host_.capture_next(window, evidence_ + "/" + command.argument + ".bmp");
        }

        log_("QA " + command.name + " " + command.argument + " " + std::to_string(command.x) + " " +
             std::to_string(command.y));
    }
#ifdef SACRED_PGO_GENERATE
    __llvm_profile_write_file(); // the run is killed at script_complete: the counters first
#endif
    log_("QA script_complete");
}
}
