#include "install.h"
#include "native.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" int sacred_granny_run_original(X86* c) {
    return sacred::Control::of(c).run_call();
}

namespace sacred {
// Registers the native stand-ins of runtime/granny/native.c over the generated code of their guest functions.
// A stand-in is tied to the exact code it replaces: it is installed only where the mapped granny.dll has those bytes.
void install_granny_natives(Control& control) {
    const char* setting = std::getenv("SACRED_GRANNY_NATIVE");
    if (setting && !std::strcmp(setting, "0")) {
        return;
    }

    granny_native_mode = setting && !std::strcmp(setting, "verify") ? GRANNY_NATIVE_VERIFY : GRANNY_NATIVE_ON;
    granny_skin_template_fn = granny_skin_template;
    unsigned installed = 0;
    for (unsigned i = 0; i < granny_native_count; ++i) {
        const GrannyNative& native = granny_natives[i];
        uint32_t hash = 0x811c9dc5u;
        bool mapped = true;
        for (const uint32_t* range = native.code; mapped && range[0]; range += 2) {
            std::vector<uint8_t> code(range[1]);
            try {
                control.memory.read(range[0], code.data(), code.size());
            } catch (const GuestFault&) {
                mapped = false;
            }

            for (uint8_t byte : code) {
                hash = (hash ^ byte) * 0x01000193u;
            }
        }

        if (!mapped || hash != native.hash) {
            continue;
        }

        *native.original = control.replace_entry(native.address, native.native);
        ++installed;
    }

    std::fprintf(stderr, "GRANNY_NATIVE installed=%u of %u%s\n", installed, granny_native_count,
                 granny_native_mode == GRANNY_NATIVE_VERIFY ? " (verifying every call)" : "");
    if (granny_native_mode == GRANNY_NATIVE_VERIFY) {
        std::atexit(granny_native_report);
    }
}
}
