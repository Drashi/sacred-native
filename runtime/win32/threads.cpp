#include "startup.h"
#include "../../platform/host_thread.h"
#include <algorithm>
#include <chrono>

namespace sacred {
namespace {
struct ThreadExit {
    uint32_t code;
};
constexpr uint32_t still_active = 259, wait_timeout = 258, create_suspended = 4;
}

struct Startup::GuestThread {
    GuestThread(Memory& memory, uint32_t id, const std::vector<TlsTemplate>& templates, uint32_t stack)
        : thread(memory, id, templates, stack), tid(id + 1) {}
    Thread thread;
    uint32_t tid, entry = 0, parameter = 0, suspend = 0, exit_code = still_active;
    bool started = false, exited = false;
    HostThread host;
};

bool Startup::wait_for(const std::function<bool()>& ready, uint32_t milliseconds) {
    if (milliseconds != UINT32_MAX && environment_.time_scale > 1) {
        milliseconds /= environment_.time_scale;
    }

    return control_.scheduler().wait(ready, milliseconds == UINT32_MAX,
                                     std::chrono::steady_clock::now() +
                                         std::chrono::milliseconds(milliseconds == UINT32_MAX ? 0 : milliseconds));
}

void Startup::start_thread(const std::shared_ptr<GuestThread>& guest) {
    guest->started = true;
    guest->host = HostThread([this, guest] { run_thread(guest); });
}

void Startup::run_thread(std::shared_ptr<GuestThread> guest) {
    auto& scheduler = control_.scheduler();
    try {
        scheduler.acquire();
        {
            Control worker(control_, guest->thread);
            worker.cpu.fpu_cw = 0x027f; // Observed Windows thread-start profile.
            x86_set_eflags(&worker.cpu, 0x246);
            try {
                auto result = worker.invoke(guest->entry, {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
                                            {guest->parameter});
                guest->exit_code = uint32_t(result.integer);
            } catch (const ThreadExit& exit) {
                guest->exit_code = exit.code;
            }

            guest->exited = true;
        }

        scheduler.changed();
    } catch (const ThreadKilled&) {
    } catch (const ProcessExit&) {
    } catch (const ProcessFault&) {
    } catch (const std::exception& error) {
        scheduler.stop("guest thread " + std::to_string(guest->tid) + ": " + error.what());
    }

    if (scheduler.owns()) {
        scheduler.release();
    }
}

// Joins and frees threads that have exited and whose handle the guest closed, returning their stack/TEB/TLS slot.
// Only called by the baton holder, so an exited thread has already released the baton and the join is brief.
void Startup::reap_threads() {
    for (auto it = threads_.begin(); it != threads_.end();) {
        const auto& guest = *it;
        const bool referenced = std::any_of(thread_handles_.begin(), thread_handles_.end(),
                                            [&](const auto& handle) { return handle.second == guest; });
        if (!guest->exited || referenced) {
            ++it;
            continue;
        }

        if (guest->host.joinable()) {
            guest->host.join();
        }

        free_thread_ids_.push_back(guest->tid - 1);
        it = threads_.erase(it);
    }
}

void Startup::shutdown_threads() {
    if (threads_.empty()) {
        return;
    }

    auto& scheduler = control_.scheduler();
    scheduler.stop();
    if (scheduler.owns()) {
        scheduler.release();
    }

    for (auto& guest : threads_) {
        if (guest->host.joinable()) {
            guest->host.join();
        }
    }
}

void Startup::bind_threads() {
    auto imported = [this](const char* symbol) {
        return std::any_of(loader_.hle_slots().begin(), loader_.hle_slots().end(),
                           [&](const HleSlot& slot) { return slot.dll == "kernel32.dll" && slot.symbol == symbol; });
    };
    auto bind = [&](const char* symbol, Signature signature, HostFunction function) {
        if (imported(symbol)) {
            control_.host(loader_.resolve("kernel32.dll", symbol), std::move(signature), std::move(function),
                          std::string("kernel32.dll!") + symbol);
        }
    };
    auto find = [this](uint32_t handle) -> std::shared_ptr<GuestThread> {
        auto found = thread_handles_.find(handle);
        return found == thread_handles_.end() ? nullptr : found->second;
    };
    bind("CreateThread", {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}, {4}, {4}, {4}, {4}}, ""},
         [this](Control& current, const std::vector<uint64_t>& a) {
             if (a[0]) {
                 throw GuestFault("unqualified thread security attributes");
             }

             if (a[4] & ~uint64_t(create_suspended)) {
                 throw GuestFault("unqualified thread creation flags");
             }

             reap_threads();
             const uint32_t id = free_thread_ids_.empty() ? next_thread_++ : free_thread_ids_.back();
             auto guest = std::make_shared<GuestThread>(current.memory, id, loader_.tls_templates(), uint32_t(a[1]));
             if (!free_thread_ids_.empty() && free_thread_ids_.back() == id) {
                 free_thread_ids_.pop_back();
             }

             guest->entry = uint32_t(a[2]);
             guest->parameter = uint32_t(a[3]);
             guest->suspend = (a[4] & create_suspended) ? 1 : 0;
             uint32_t handle = next_handle_++;
             thread_handles_.emplace(handle, guest);
             threads_.push_back(guest);
             if (a[5]) {
                 current.memory.store32(uint32_t(a[5]), guest->tid);
             }

             if (!guest->suspend) {
                 start_thread(guest);
             }

             return Result{handle, {}};
         });
    bind("ResumeThread", {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
         [this, find](Control&, const std::vector<uint64_t>& a) {
             auto guest = find(uint32_t(a[0]));
             if (!guest) {
                 return Result{error(6, UINT32_MAX), {}};
             }

             uint32_t previous = guest->suspend;
             if (previous && !--guest->suspend && !guest->started) {
                 start_thread(guest);
             }

             control_.scheduler().changed();
             return Result{previous, {}};
         });
    bind("GetExitCodeThread", {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}}, ""},
         [this, find](Control& current, const std::vector<uint64_t>& a) {
             auto guest = find(uint32_t(a[0]));
             if (!guest) {
                 return Result{error(6), {}};
             }

             current.memory.store32(uint32_t(a[1]), guest->exited ? guest->exit_code : still_active);
             return Result{1, {}};
         });
    bind("InterlockedIncrement", {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
         [](Control& current, const std::vector<uint64_t>& a) {
             uint32_t value = current.memory.load32(uint32_t(a[0])) + 1;
             current.memory.store32(uint32_t(a[0]), value);
             return Result{value, {}};
         });
    bind("InterlockedDecrement", {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
         [](Control& current, const std::vector<uint64_t>& a) {
             uint32_t value = current.memory.load32(uint32_t(a[0])) - 1;
             current.memory.store32(uint32_t(a[0]), value);
             return Result{value, {}};
         });
    bind("InterlockedExchangeAdd", {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}}, ""},
         [](Control& current, const std::vector<uint64_t>& a) {
             uint32_t previous = current.memory.load32(uint32_t(a[0]));
             current.memory.store32(uint32_t(a[0]), previous + uint32_t(a[1]));
             return Result{previous, {}};
         });
    bind("InterlockedCompareExchange", {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}, {4}}, ""},
         [](Control& current, const std::vector<uint64_t>& a) {
             uint32_t previous = current.memory.load32(uint32_t(a[0]));
             if (previous == uint32_t(a[2])) {
                 current.memory.store32(uint32_t(a[0]), uint32_t(a[1]));
             }

             return Result{previous, {}};
         });
    bind("DeleteCriticalSection", {Convention::Stdcall, ReturnChannel::Void, {{4}}, ""},
         [this](Control&, const std::vector<uint64_t>& a) {
             if (!critical_.erase(uint32_t(a[0]))) {
                 throw GuestFault("deleting an uninitialized critical section");
             }

             control_.scheduler().changed();
             return Result{};
         });
    bind("ExitProcess", {Convention::Stdcall, ReturnChannel::Void, {{4}}, ""},
         [this](Control&, const std::vector<uint64_t>& a) -> Result {
             control_.scheduler().exit_process(uint32_t(a[0]));
             throw ProcessExit{uint32_t(a[0])};
         });
    bind("ExitThread", {Convention::Stdcall, ReturnChannel::Void, {{4}}, ""},
         [this](Control&, const std::vector<uint64_t>& a) -> Result {
             uint32_t tid = thread_id();
             if (std::none_of(threads_.begin(), threads_.end(),
                              [tid](const auto& guest) { return guest->tid == tid; })) {
                 throw GuestFault("ExitThread on the process main thread is unqualified");
             }

             throw ThreadExit{uint32_t(a[0])};
         });
}

bool Startup::wait_thread(uint32_t handle, uint32_t milliseconds, uint32_t& result) {
    auto found = thread_handles_.find(handle);
    if (found == thread_handles_.end()) {
        return false;
    }

    auto guest = found->second;
    result = wait_for([&] { return guest->exited; }, milliseconds) ? 0 : wait_timeout;
    return true;
}
}
