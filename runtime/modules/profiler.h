#pragma once

namespace sacred {
// Opt-in sampling profiler (macOS ARM64): with SACRED_PROFILE=<file> set, SIGUSR1 starts SACRED_PROFILE_SECONDS
// (default 8) of ~2 kHz sampling of every running thread (pc + frame-pointer chain, image-relative addresses) and
// writes them to <file>. tools/perf/profile_report.py folds the samples with the executable's symbols.
void start_profiler();
}
