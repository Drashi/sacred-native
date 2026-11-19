#include "platform/window_host.h"
#include "platform/frame_clock.h"
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <mutex>
#include <exception>
#include <thread>

int main() {
    using namespace std::chrono;
    sacred::FrameClock::Clock::time_point epoch{};
    sacred::FrameClock clock(epoch);
    if (clock.deadline(epoch) != epoch + nanoseconds(16666667) ||
        clock.deadline(epoch + nanoseconds(16666666)) != epoch + nanoseconds(16666667) ||
        clock.deadline(epoch + nanoseconds(16666667)) != epoch + nanoseconds(33333334) ||
        clock.deadline(epoch + seconds(1) - nanoseconds(1)) != epoch + seconds(1) ||
        clock.deadline(epoch + seconds(1000)) != epoch + seconds(1000) + nanoseconds(16666667)) {
        return 1;
    }

    sacred::WindowHost host;
    std::exception_ptr failure;
    bool first = true, second = true;
    bool enabled = false, restored = true;
    std::pair<int, int> size;
    std::thread guest([&] {
        try {
            first = host.screen_saver();
            second = host.screen_saver();
            host.screen_saver(true);
            enabled = host.screen_saver();
            host.screen_saver(false);
            restored = host.screen_saver();
            size = host.display_size();
        } catch (...) {
            failure = std::current_exception();
        }

        host.finish();
    });
    host.loop();
    guest.join();
    if (failure) {
        try {
            std::rethrow_exception(failure);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s\n", error.what());
        }

        return 1;
    }

    if (first || second || !enabled || restored || size.first <= 0 || size.second <= 0) {
        return 1;
    }

    {
        // Host-loop exit: a guest blocked in a host command and a guest waiting for input must both be released.
        // No sleeps: whichever side runs first, shutdown() fails the queued command or submit() sees the stopped host.
        sacred::WindowHost stopped_host;
        std::mutex mutex;
        std::condition_variable changed;
        bool notified = false;
        stopped_host.set_event_notifier([&] {
            {
                std::lock_guard<std::mutex> lock(mutex);
                notified = true;
            }

            changed.notify_all();
        });
        std::promise<bool> submit_stopped, input_released;
        auto submit_result = submit_stopped.get_future(), input_result = input_released.get_future();
        std::thread submitter([&] {
            try {
                stopped_host.screen_saver();
                submit_stopped.set_value(false);
            } catch (const sacred::HostStopped&) {
                submit_stopped.set_value(true);
            } catch (...) {
                submit_stopped.set_value(false);
            }
        });
        std::thread input_waiter([&] {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [&] { return notified && stopped_host.stopped(); });
            input_released.set_value(true);
        });
        stopped_host.shutdown();
        const bool bounded = submit_result.wait_for(seconds(20)) == std::future_status::ready &&
                             input_result.wait_for(seconds(20)) == std::future_status::ready;
        if (!bounded) {
            std::fputs("guest still blocked after host shutdown\n", stderr);
            std::_Exit(1);
        }

        submitter.join();
        input_waiter.join();
        bool later_stopped = false;
        try {
            stopped_host.display_size();
        } catch (const sacred::HostStopped&) {
            later_stopped = true;
        }

        if (!submit_result.get() || !input_result.get() || !later_stopped) {
            return 1;
        }
    }

    std::puts("HOST_QUEUE_PASS seven_completed_commands=1 worker_joined=1 shutdown_releases_guests=1");
    return 0;
}
