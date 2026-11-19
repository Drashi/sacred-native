#include "profiler.h"
#include "../process/control.h"
#include <cstring>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#if defined(__APPLE__) && defined(__aarch64__)
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <set>
#include <string>
#include <mach/mach.h>
#include <pthread.h>
#endif

namespace sacred {
#if defined(__APPLE__) && defined(__aarch64__)
namespace {
std::atomic<bool> requested{false};
void on_signal(int) {
    requested = true;
}

constexpr int depth = 24;
constexpr int guest_depth = 16;
struct Sample {
    uint64_t thread;
    int frames, guest_frames;
    uint64_t pc[depth];
    uint32_t guest[guest_depth];
};

// Guest stack: source_pc (start of the last block entered), then the words above ESP that look like return
// addresses (code of the main image right after a CALL). Frame-pointer-omitted code makes the EBP chain useless.
bool readable(uint32_t address) {
    return g_page_access && (g_page_access[address >> 12] & 1u);
}

bool after_call(uint32_t ret) {
    if (ret < 0x401000 || ret >= 0x2000000 || !readable(ret) || !readable(ret - 8)) {
        return false;
    }

    const uint8_t* p = g_mem + ret;
    if (p[-5] == 0xe8) {
        return true; // call rel32
    }

    if (p[-2] == 0xff && ((p[-1] >> 3) & 7) == 2 && (p[-1] >> 6) == 3) {
        return true; // call reg
    }

    if (p[-2] == 0xff && ((p[-1] >> 3) & 7) == 2 && (p[-1] >> 6) == 0 && (p[-1] & 7) != 4 && (p[-1] & 7) != 5) {
        return true; // call [reg]
    }

    if (p[-3] == 0xff && ((p[-2] >> 3) & 7) == 2 && (p[-2] >> 6) == 1) {
        return true; // call [reg+d8]
    }

    if (p[-6] == 0xff && ((p[-5] >> 3) & 7) == 2 && ((p[-5] >> 6) == 2 || (p[-5] & 0xc7) == 5)) {
        return true;
    }

    return false;
}

int walk_guest(const X86* cpu, uint32_t* out) {
    if (!cpu || !g_mem) {
        return 0;
    }

    int n = 0;
    out[n++] = cpu->source_pc;
    const uint32_t esp = cpu->r[R_ESP];
    if (esp < 0x10000 || esp & 3) {
        return n;
    }

    for (uint32_t a = esp; a < esp + 2048 && n < guest_depth; a += 4) {
        if (!readable(a)) {
            break;
        }

        uint32_t word;
        std::memcpy(&word, g_mem + a, 4);
        if (after_call(word)) {
            out[n++] = word;
        }
    }

    return n;
}

// Frame-pointer walk of a suspended thread. Only reads inside its stack: the chain must rise from sp.
int walk(const arm_thread_state64_t& state, uint64_t* out) {
    int n = 0;
    out[n++] = arm_thread_state64_get_pc(state);
    out[n++] = arm_thread_state64_get_lr(state);
    uint64_t fp = arm_thread_state64_get_fp(state), sp = arm_thread_state64_get_sp(state);
    while (n < depth && fp >= sp && fp < sp + (64u << 20) && !(fp & 7)) {
        const uint64_t* frame = reinterpret_cast<const uint64_t*>(fp);
        const uint64_t next = frame[0], ret = frame[1];
        if (!ret) {
            break;
        }

        out[n++] = ret;
        if (next <= fp) {
            break;
        }

        fp = next;
    }

    return n;
}

void run(std::string path, double seconds) {
    while (!requested) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    const intptr_t slide = _dyld_get_image_vmaddr_slide(0);
    const mach_port_t self = mach_thread_self();
    std::vector<Sample> samples;
    samples.reserve(size_t(seconds * 1000 * 16));
    Sample scratch[64];
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    unsigned ticks = 0;
    while (std::chrono::steady_clock::now() < end) {
        thread_act_array_t threads;
        mach_msg_type_number_t count = 0;
        if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) {
            break;
        }

        int taken = 0;
        for (mach_msg_type_number_t i = 0; i < count && taken < 64; ++i) {
            if (threads[i] == self) {
                continue;
            }

            thread_basic_info_data_t info;
            mach_msg_type_number_t info_count = THREAD_BASIC_INFO_COUNT;
            if (thread_info(threads[i], THREAD_BASIC_INFO, thread_info_t(&info), &info_count) != KERN_SUCCESS) {
                continue;
            }

            if (info.run_state != TH_STATE_RUNNING) {
                continue; // only threads on a CPU
            }

            thread_identifier_info_data_t id;
            mach_msg_type_number_t id_count = THREAD_IDENTIFIER_INFO_COUNT;
            thread_info(threads[i], THREAD_IDENTIFIER_INFO, thread_info_t(&id), &id_count);
            // Nothing that can take a lock (malloc) while the other thread is stopped.
            if (thread_suspend(threads[i]) != KERN_SUCCESS) {
                continue;
            }

            arm_thread_state64_t state;
            mach_msg_type_number_t state_count = ARM_THREAD_STATE64_COUNT;
            if (thread_get_state(threads[i], ARM_THREAD_STATE64, thread_state_t(&state), &state_count) ==
                KERN_SUCCESS) {
                Sample& s = scratch[taken++];
                s.thread = id.thread_id;
                s.frames = walk(state, s.pc);
                s.guest_frames = 0;
                for (const auto& slot : profiled_cpus) {
                    if (slot.thread.load(std::memory_order_relaxed) == id.thread_id) {
                        s.guest_frames = walk_guest(slot.cpu.load(std::memory_order_relaxed), s.guest);
                        break;
                    }
                }
            }

            thread_resume(threads[i]);
        }

        for (mach_msg_type_number_t i = 0; i < count; ++i) {
            mach_port_deallocate(mach_task_self(), threads[i]);
        }

        vm_deallocate(mach_task_self(), vm_address_t(threads), count * sizeof(thread_act_t));
        samples.insert(samples.end(), scratch, scratch + taken);
        ++ticks;
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }

    if (FILE* out = std::fopen(path.c_str(), "w")) {
        std::fprintf(out, "ticks %u seconds %.3f\n", ticks, seconds);
        // Addresses outside the executable (system libraries) are named here; dladdr sees their exported symbols.
        const void* main_image = _dyld_get_image_header(0);
        std::set<uint64_t> seen;
        for (const Sample& s : samples) {
            for (int i = 0; i < s.frames; ++i) {
                Dl_info info{};
                if (s.pc[i] && seen.insert(s.pc[i]).second && dladdr(reinterpret_cast<void*>(s.pc[i]), &info) &&
                    info.dli_fbase != main_image) {
                    const char* lib = info.dli_fname ? std::strrchr(info.dli_fname, '/') : nullptr;
                    std::fprintf(out, "sym %llx %s:%s\n", (unsigned long long)(s.pc[i] - slide), lib ? lib + 1 : "?",
                                 info.dli_sname ? info.dli_sname : "?");
                }
            }
        }

        for (const Sample& s : samples) {
            std::fprintf(out, "%llu", (unsigned long long)s.thread);
            for (int i = 0; i < s.frames; ++i) {
                std::fprintf(out, " %llx", (unsigned long long)(s.pc[i] - slide));
            }

            if (s.guest_frames) {
                std::fputs(" |", out);
            }

            for (int i = 0; i < s.guest_frames; ++i) {
                std::fprintf(out, " %x", s.guest[i]);
            }

            std::fputc('\n', out);
        }

        std::fclose(out);
    }

    std::fprintf(stderr, "PROFILE_DONE %s ticks=%u samples=%zu\n", path.c_str(), ticks, samples.size());
}
}

void start_profiler() {
    const char* path = std::getenv("SACRED_PROFILE");
    if (!path || !*path) {
        return;
    }

    const char* secs = std::getenv("SACRED_PROFILE_SECONDS");
    std::signal(SIGUSR1, on_signal);
    std::thread(run, std::string(path), secs ? std::atof(secs) : 8.0).detach();
}
#else
void start_profiler() {
}
#endif
}
