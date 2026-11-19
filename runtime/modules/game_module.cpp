#include "generated.h"
#include <stdexcept>
#ifdef SACRED_GAME_MODULE_DYNAMIC
#include <dlfcn.h>
#endif

namespace sacred {
const SacredGameModule& load_game_module(const std::string& path, const std::string& sacred_sha256) {
    const SacredGameModule* module = nullptr;
#ifdef SACRED_GAME_MODULE_DYNAMIC
    if (path.empty()) {
        throw std::runtime_error("no game module given (--game-module PATH; the launcher builds it)");
    }

    // RTLD_LOCAL: the module's own symbols stay private; it resolves the runtime's helpers against this executable.
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        throw std::runtime_error("cannot load the game module " + path + ": " + dlerror());
    }

    using Entry = const SacredGameModule* (*)();
    auto entry = reinterpret_cast<Entry>(dlsym(handle, "sacred_game_module"));
    if (!entry) {
        throw std::runtime_error(path + " is not a game module (no sacred_game_module)");
    }

    module = entry();
#else
    if (!path.empty()) {
        throw std::runtime_error("this build has its game module linked in; --game-module is not used");
    }

    module = sacred_game_module();
#endif
#ifdef SACRED_QA
    const uint32_t qa = 1;
#else
    const uint32_t qa = 0;
#endif
    if (!module || module->abi != SACRED_GAME_MODULE_ABI) {
        throw std::runtime_error("the game module was built for another runtime version: rebuild it");
    }

    if (module->qa != qa) {
        throw std::runtime_error("the game module's QA setting differs from the runtime's: rebuild it");
    }

    if (!sacred_sha256.empty() && sacred_sha256 != module->sacred_sha256) {
        throw std::runtime_error("the game module was generated from another Sacred.exe: rebuild it");
    }

    return *module;
}

void register_generated(Control& control, const SacredGameModule& module) {
    for (unsigned t = 0; t < module.table_count; ++t) {
        const SacredGeneratedTable& table = module.tables[t];
        for (unsigned i = 0; i < *table.count; ++i) {
            control.entry(table.entries[i].address, table.entries[i].function, 3, table.tag);
        }
    }
}
}
