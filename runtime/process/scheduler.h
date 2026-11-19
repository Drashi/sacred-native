#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace sacred {
// Thrown into a guest thread when the process is shutting down; never a guest fault.
struct ThreadKilled : std::exception {
    const char* what() const noexcept override { return "guest thread killed"; }
};
// Thrown by a guest thread fault into every other thread (strict trap, process-wide).
struct ProcessFault : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct ProcessExit {
    uint32_t code;
};

// Serialized guest execution. Exactly one host thread holds the baton and may
// touch guest memory or process state. Blocking operations release it; a FIFO
// ticket order makes handoff deterministic for threads that are already queued.
class Scheduler {
public:
    void acquire() {
        std::unique_lock<std::mutex> lock(baton_);
        const uint64_t ticket = next_++;
        baton_cv_.wait(lock, [&] { return serving_ == ticket; });
        owner_ = std::this_thread::get_id();
        if (!stopping_.load()) {
            return;
        }

        owner_ = std::thread::id();
        ++serving_;
        baton_cv_.notify_all();
        lock.unlock();
        check_stop();
    }

    void release() {
        std::lock_guard<std::mutex> lock(baton_);
        owner_ = std::thread::id();
        ++serving_;
        baton_cv_.notify_all();
    }

    bool owns() const {
        std::lock_guard<std::mutex> lock(baton_);
        return owner_ == std::this_thread::get_id();
    }

    // Hand the baton to threads already queued behind this one, if any.
    void yield() {
        if (stopping_.load() && owns()) {
            check_stop();
        }

        {
            std::lock_guard<std::mutex> lock(baton_);
            if (owner_ != std::this_thread::get_id() || next_ == serving_ + 1) {
                return;
            }
        }

        release();
        acquire();
    }

    // Bump the epoch after a state change another thread may be waiting for.
    void changed() {
        std::lock_guard<std::mutex> lock(change_);
        ++epoch_;
        change_cv_.notify_all();
    }

    // Wait until predicate() holds (evaluated with the baton) or the deadline passes.
    // The baton is released while blocked. Returns the final predicate value.
    template <class Predicate>
    bool wait(Predicate predicate, bool infinite, std::chrono::steady_clock::time_point deadline) {
        for (;;) {
            check_stop();
            if (predicate()) {
                return true;
            }

            if (!infinite && std::chrono::steady_clock::now() >= deadline) {
                return false;
            }

            uint64_t observed;
            {
                std::lock_guard<std::mutex> lock(change_);
                observed = epoch_;
            }

            release();
            {
                std::unique_lock<std::mutex> lock(change_);
                auto ready = [&] {
                    return epoch_ != observed || stopping_.load();
                };
                if (infinite) {
                    change_cv_.wait(lock, ready);
                } else {
                    change_cv_.wait_until(lock, deadline, ready);
                }
            }

            acquire();
        }
    }

    // Process-wide stop (clean teardown, or the first guest-thread fault).
    void stop(const std::string& fault = std::string()) {
        {
            std::lock_guard<std::mutex> lock(change_);
            if (!fault.empty() && fault_.empty()) {
                fault_ = fault;
            }

            stopping_ = true;
            ++epoch_;
        }

        change_cv_.notify_all();
    }

    void exit_process(uint32_t code) {
        {
            std::lock_guard<std::mutex> lock(change_);
            if (!exiting_) {
                exiting_ = true;
                exit_code_ = code;
            }

            stopping_ = true;
            ++epoch_;
        }

        change_cv_.notify_all();
    }

    bool stopping() const { return stopping_.load(); }

private:
    void check_stop() {
        if (!stopping_.load()) {
            return;
        }

        std::lock_guard<std::mutex> lock(change_);
        if (!fault_.empty()) {
            throw ProcessFault(fault_);
        }

        if (exiting_) {
            throw ProcessExit{exit_code_};
        }

        throw ThreadKilled();
    }

    mutable std::mutex baton_;
    std::condition_variable baton_cv_;
    uint64_t next_ = 0, serving_ = 0;
    std::thread::id owner_;
    std::mutex change_;
    std::condition_variable change_cv_;
    uint64_t epoch_ = 0;
    std::atomic<bool> stopping_{false};
    std::string fault_;
    bool exiting_ = false;
    uint32_t exit_code_ = 0;
};
}
