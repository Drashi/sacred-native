#pragma once
#include <cstdlib>

namespace sacred {
// QA-only environment switches. Compiled out (always false) unless the build defines SACRED_QA, so a production
// build exposes no hidden environment knobs.
inline bool qa_env(const char* name) {
#ifdef SACRED_QA
    return std::getenv(name) != nullptr;
#else
    (void)name;
    return false;
#endif
}
}
