#pragma once
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <pthread.h>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif
#include <stdexcept>
#include <string>
#include <cstring>

namespace sacred {
// Host thread that runs translated guest code. Translated functions call each other natively, so guest call depth
// becomes host stack depth. std::thread inherits the platform default, which is RLIMIT_STACK (normally 8 MiB) on
// glibc but only 512 KiB for secondary threads on macOS; this always asks for an explicit, generous stack.
class HostThread {
public:
    static constexpr size_t stack_size = size_t{16} << 20;
    HostThread() = default;
    explicit HostThread(std::function<void()> body) {
        auto start = std::make_unique<std::function<void()>>(std::move(body));
        pthread_attr_t attr;
        if (int error = pthread_attr_init(&attr)) {
            throw std::runtime_error("pthread_attr_init: " + std::string(std::strerror(error)));
        }

        int error = pthread_attr_setstacksize(&attr, stack_size);
#ifdef __APPLE__
        // Guest threads are the game: at the default class macOS wakes them from a timed sleep about a millisecond
        // late (4 ms at worst), which the frame loop loses out of every 60 Hz interval.
        if (!error) {
            error = pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
        }
#endif
        if (!error) {
            error = pthread_create(&thread_, &attr, &HostThread::run, start.get());
        }

        pthread_attr_destroy(&attr);
        if (error) {
            throw std::runtime_error("cannot start host thread: " + std::string(std::strerror(error)));
        }

        start.release();
        joinable_ = true;
    }

    HostThread(const HostThread&) = delete;
    HostThread& operator=(const HostThread&) = delete;
    HostThread(HostThread&& other) noexcept : thread_(other.thread_), joinable_(other.joinable_) {
        other.joinable_ = false;
    }

    HostThread& operator=(HostThread&& other) noexcept {
        if (this != &other) {
            if (joinable_) {
                std::terminate(); // same contract as std::thread
            }

            thread_ = other.thread_;
            joinable_ = other.joinable_;
            other.joinable_ = false;
        }

        return *this;
    }

    ~HostThread() {
        if (joinable_) {
            std::terminate();
        }
    }

    bool joinable() const { return joinable_; }
    void join() {
        if (!joinable_) {
            throw std::logic_error("host thread is not joinable");
        }

        if (int error = pthread_join(thread_, nullptr)) {
            throw std::runtime_error("pthread_join: " + std::string(std::strerror(error)));
        }

        joinable_ = false;
    }

private:
    static void* run(void* argument) {
        std::unique_ptr<std::function<void()>> body(static_cast<std::function<void()>*>(argument));
        try {
            (*body)();
        } catch (...) {
            std::terminate();
        } // same contract as std::thread

        return nullptr;
    }

    pthread_t thread_{};
    bool joinable_ = false;
};
}
