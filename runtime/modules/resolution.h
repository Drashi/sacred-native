#pragma once
#include <cstdint>
#include "resolution_table.h"
#include <string>
#include <vector>

namespace sacred {
class Memory;
// Opt-in screen size other than the original 1024x768: Settings.cfg keys HD_WIDTH and HD_HEIGHT (both required,
// at least 1024x720). Without them the mode is off and every resolution cell keeps its original value.
struct ResolutionConfig {
    bool enabled = false;
    int32_t width = 1024, height = 768;
};
constexpr int32_t resolution_min_width = 1024, resolution_min_height = 720;
// Reads the keys from a Settings.cfg text ("KEY : value" lines). Values below the minimum are raised to it.
ResolutionConfig parse_resolution_settings(const std::string& text);
ResolutionConfig read_resolution_settings(const std::string& settings_path);
// The mode for a whole host display (fullscreen without explicit keys): its size raised to the minimum, off when that
// is the original 1024x768.
ResolutionConfig display_resolution(int width, int height);
// Opt-in HD rendering options, all off by default (Settings.cfg keys, then the environment, which wins):
//   RENDER_SCALE : k   / SACRED_RENDER_SCALE=k    internal render scale, integer 1..4 (others are ignored with a
//                                                 note; the previous value, Settings.cfg's or 1, stays)
//   TEXTURE_PACK : dir / SACRED_TEXTURE_PACK=dir  replacement textures by content hash (runtime/ddraw/texture_pack.h)
//   SACRED_TEXTURE_DUMP=dir                       writes every distinct texture content once as PNG
//   MENU_BACKGROUNDS : dir / SACRED_MENU_BACKGROUNDS=dir   wide images for the 1024x768 screens in HD mode
//                                                 (runtime/ddraw/menu_backdrops.h)
struct HdOptions {
    uint32_t render_scale = 1;
    uint32_t texture_upscale = 0; // TEXTURE_UPSCALE / SACRED_TEXTURE_UPSCALE: 2..4 (0 or 1: off)
    std::string texture_pack, texture_dump, menu_backgrounds;
    std::vector<std::string> notes; // one line per ignored value, for the log
};
// The directories (TEXTURE_PACK, MENU_BACKGROUNDS, from Settings.cfg or the environment) go through option_directory.
HdOptions parse_hd_options(const std::string& settings_text, const char* env_scale, const char* env_pack,
                           const char* env_dump, const char* env_backgrounds = nullptr, const char* home = nullptr);
// A directory value as written by hand: surrounding blanks trimmed, one pair of surrounding quotes ("..." or '...')
// removed, and a leading "~/" (or a lone "~") replaced by `home` ($HOME) when that is set.
std::string option_directory(const std::string& value, const char* home);
HdOptions read_hd_options(const std::string& settings_path);
// SACRED_TEXTURE_CACHE_MB: decoded replacements kept in memory, integer 1..65536 MB; unset gives 128, anything
// else gives 128 and sets *note.
size_t parse_texture_cache_mb(const char* value, std::string* note);
// Frame rate limit (Settings.cfg FPS_LIMIT, then SACRED_FPS_LIMIT, which wins): an integer 30..240 or `auto` (the
// display's refresh rate, clamped to that range). Unset, 60 or an invalid value (with a note) keep the original 60.
// The game's own limiter, the frame-counted timeouts of the generated table (F) and the vertical-blank clock follow it.
constexpr int32_t fps_limit_default = 60, fps_limit_min = 30, fps_limit_max = 240;
struct FpsLimit {
    int32_t fps = fps_limit_default;
    bool automatic = false;         // `auto`: fps is filled in from the display (resolve_fps_limit)
    std::vector<std::string> notes; // one line per ignored value, for the log
};
FpsLimit parse_fps_limit(const std::string& settings_text, const char* env_limit);
FpsLimit read_fps_limit(const std::string& settings_path);
// True when Settings.cfg names WAITRETRACE. Without it the game waits for one vertical blank per flip (its
// default 1); the port's default is 0 (see DirectDraw::frame_vblank_default_off).
bool has_wait_retrace(const std::string& settings_path);
// The value of a Settings.cfg key (its last line, trimmed), "" when absent or when the file cannot be read.
std::string read_setting(const std::string& settings_path, const std::string& key);
// The limit for a display refreshing at `refresh_hz` (0: unknown, then 60) when automatic, clamped to 30..240.
int32_t resolve_fps_limit(const FpsLimit& limit, int refresh_hz);
// Writes the generated table into the mapped image: code cells always (original values when off), data sites only
// when the mode is on. Returns the number of guest writes.
unsigned apply_resolution(Memory& memory, const ResolutionConfig& config, int32_t fps = fps_limit_default);
// Rewrites only the cells that depend on the frame rate limit (safe while the game runs). Returns the writes.
unsigned apply_fps_limit(Memory& memory, const ResolutionConfig& config, int32_t fps);
// Settings.cfg switches of the generated table (e.g. LEGACY_UI : 1, the original Sacred UI skin, menu art and music
// instead of the Underworld ones): any value but 0 turns one on. Returns the settings that are on.
std::vector<std::string> parse_option_settings(const std::string& text);
// Writes every switch's guest word (1 when on, 0 otherwise); returns the settings that are on.
std::vector<std::string> apply_options(Memory& memory, const std::string& settings_path);
// Guest word that hooks raise while a screen designed for 1024x768 draws (0 when the table has none). The present
// that finds it raised shows that screen centred and clears it; see DirectDraw::inset_word.
uint32_t resolution_inset_word();
// The generated table the functions above read (the game module's, or a test fixture); set before any of them runs.
void use_resolution_table(const struct ::ResolutionTable& table);
}
