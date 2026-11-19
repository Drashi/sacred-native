#pragma once
#include "../process/control.h"
#include "game_module.h"
#include <string>
namespace sacred {
// The game module (game_module.h): the one linked into this binary, or with SACRED_GAME_MODULE_DYNAMIC the shared
// library at `path` (the launcher's build). Throws when it is missing, of another ABI or QA setting, or generated
// from another Sacred.exe than `sacred_sha256`.
const SacredGameModule& load_game_module(const std::string& path, const std::string& sacred_sha256);
// Enters every generated region in the dispatch table.
void register_generated(Control& control, const SacredGameModule& module);
}
