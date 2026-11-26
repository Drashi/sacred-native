// Opt-in screen size: Settings.cfg parsing and the guest writes of a resolution table. The table here is a fixture
// with the shape tools/lift/resolution.py generates.
#include "runtime/modules/resolution.h"
#include "runtime/modules/resolution_table.h"
#include "runtime/process/memory.h"
#include "platform/display_choice.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

extern "C" {
const char resolution_table_sha256[] = "fixture";
const unsigned resolution_write_count = 5u;
const uint32_t resolution_inset_address = 0u;
const struct ResolutionWrite resolution_writes[] = {
    {0x00500000u, 4u, 1u, 0u}, // cell: W
    {0x00500008u, 4u, 1u, 0u}, // cell: f32 H/2
    {0x00500010u, 4u, 1u, 0u}, // cell: hd(0x2ef0f4, 0x970f4)
    {0x00500020u, 4u, 1u, 1u}, // cell: 100*F/60
    {0x00501000u, 2u, 0u, 0u}, // data: W - 1024 + 0x2a8, read-only page
};
const unsigned resolution_blob_count = 0u;
const struct ResolutionBlob resolution_blobs[] = {{0u, 0u, ""}};
const unsigned resolution_option_write_count = 0u;
const struct ResolutionOptionWrite resolution_option_writes[] = {{"", 0u, 0u, ""}};
const unsigned resolution_option_count = 1u;
const struct ResolutionOption resolution_options[] = {
    {"LEGACY_UI", 0x00500018u}, // switch word tested by a generated option hook
};
uint64_t resolution_value(unsigned index, int32_t w, int32_t h, int32_t hd, int32_t f) {
    switch (index) {
    case 0u:
        return uint64_t(w);
    case 1u: {
        float f = float(h / 2);
        uint32_t u;
        std::memcpy(&u, &f, 4);
        return u;
    }
    case 2u:
        return hd ? 0x2ef0f4u : 0x970f4u;
    case 3u:
        return uint64_t(100 * int64_t(f) / 60);
    case 4u:
        return uint64_t(int64_t(w) - 1024 + 0x2a8);
    default:
        return 0u;
    }
}

const struct ResolutionTable resolution_table = {resolution_table_sha256,
                                                 resolution_write_count,
                                                 resolution_inset_address,
                                                 resolution_writes,
                                                 resolution_blob_count,
                                                 resolution_blobs,
                                                 resolution_option_write_count,
                                                 resolution_option_writes,
                                                 resolution_option_count,
                                                 resolution_options,
                                                 resolution_value};
}

namespace {
int failures = 0;
void expect(bool condition, const char* what, int line) {
    if (!condition) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++failures;
    }
}
#define EXPECT(condition) expect((condition), #condition, __LINE__)
float f32(sacred::Memory& m, uint32_t address) {
    uint32_t bits = m.load32(address);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

uint16_t u16(sacred::Memory& m, uint32_t address) {
    uint16_t v;
    std::memcpy(&v, reinterpret_cast<const void*>(m.host_address(address)), 2);
    return v;
}
}

int main() {
    sacred::use_resolution_table(resolution_table);
    using sacred::parse_resolution_settings;
    // Off without both keys; the original lines around them are ignored.
    auto config = parse_resolution_settings("FULLSCREEN : 1\r\nWINDOW_WIDTH : 1366\r\n");
    EXPECT(!config.enabled && config.width == 1024 && config.height == 768);
    config = parse_resolution_settings("HD_WIDTH : 1366\r\n");
    EXPECT(!config.enabled);
    config = parse_resolution_settings("SOUND : 1\r\nHD_WIDTH : 1366\r\nHD_HEIGHT : 768\r\nVIOLENCE : 1\r\n");
    EXPECT(config.enabled && config.width == 1366 && config.height == 768);
    config = parse_resolution_settings("HD_WIDTH:1920\nHD_HEIGHT:1080");
    EXPECT(config.enabled && config.width == 1920 && config.height == 1080);
    // Below the minimum: raised to 1024x720.
    config = parse_resolution_settings("HD_WIDTH : 800\nHD_HEIGHT : 600\n");
    EXPECT(config.enabled && config.width == 1024 && config.height == 720);
    // Fullscreen without keys takes the display's size; a 1024x768 display keeps the original mode off.
    config = sacred::display_resolution(1920, 1080);
    EXPECT(config.enabled && config.width == 1920 && config.height == 1080);
    config = sacred::display_resolution(1280, 600);
    EXPECT(config.enabled && config.width == 1280 && config.height == 720);
    config = sacred::display_resolution(1024, 768);
    EXPECT(!config.enabled && config.width == 1024 && config.height == 768);
    // Not a number: treated as absent.
    config = parse_resolution_settings("HD_WIDTH : wide\nHD_HEIGHT : 768\n");
    EXPECT(!config.enabled);
    config = parse_resolution_settings("HD_WIDTH : -1366\nHD_HEIGHT : 768\n");
    EXPECT(!config.enabled);

    // HD rendering options: off by default; RENDER_SCALE 1..4 only; the environment wins over Settings.cfg.
    using sacred::parse_hd_options;
    auto hd = parse_hd_options("HD_WIDTH : 1366\r\n", nullptr, nullptr, nullptr);
    EXPECT(hd.render_scale == 1 && hd.texture_pack.empty() && hd.texture_dump.empty());
    hd = parse_hd_options("RENDER_SCALE : 2\r\nTEXTURE_PACK : /packs/hd \r\n", nullptr, nullptr, nullptr);
    EXPECT(hd.render_scale == 2 && hd.texture_pack == "/packs/hd" && hd.menu_backgrounds.empty());
    // Wide screen images: a directory from Settings.cfg, the environment wins.
    hd = parse_hd_options("MENU_BACKGROUNDS : /art/menus \r\n", nullptr, nullptr, nullptr);
    EXPECT(hd.menu_backgrounds == "/art/menus" && hd.notes.empty());
    EXPECT(parse_hd_options("MENU_BACKGROUNDS : /art/menus\n", nullptr, nullptr, nullptr, "/other").menu_backgrounds ==
           "/other");
    // Directories as written by hand: one pair of surrounding quotes goes, a leading ~/ is $HOME (file and
    // environment); anything else stays as written.
    using sacred::option_directory;
    EXPECT(option_directory(" \"/Users/me/Sacred Packs/bg\" \r", "/Users/me") == "/Users/me/Sacred Packs/bg");
    EXPECT(option_directory("'/art/menus'", nullptr) == "/art/menus");
    EXPECT(option_directory("~/packs/bg", "/Users/me") == "/Users/me/packs/bg");
    EXPECT(option_directory("\"~/packs/bg\"", "/Users/me/") == "/Users/me/packs/bg");
    EXPECT(option_directory("~", "/Users/me") == "/Users/me");
    EXPECT(option_directory("~/packs", nullptr) == "~/packs" && option_directory("~/packs", "") == "~/packs");
    EXPECT(option_directory("~other/packs", "/Users/me") == "~other/packs");
    EXPECT(option_directory("\"/art", nullptr) == "\"/art" && option_directory("\"/art'", nullptr) == "\"/art'");
    EXPECT(option_directory("\"", nullptr) == "\"" &&
           option_directory("build/menu-hd/backgrounds", "/h") == "build/menu-hd/backgrounds");
    hd = parse_hd_options("MENU_BACKGROUNDS : \"~/Sacred/bg\"\r\nTEXTURE_PACK : '~/Sacred/pack'\r\n", nullptr, nullptr,
                          nullptr, nullptr, "/Users/me");
    EXPECT(hd.menu_backgrounds == "/Users/me/Sacred/bg" && hd.texture_pack == "/Users/me/Sacred/pack");
    hd = parse_hd_options("", nullptr, " ~/pack ", nullptr, "\"~/bg\"", "/Users/me");
    EXPECT(hd.menu_backgrounds == "/Users/me/bg" && hd.texture_pack == "/Users/me/pack");
    for (const char* bad : {"RENDER_SCALE : 0\n", "RENDER_SCALE : 5\n", "RENDER_SCALE : 1.5\n", "RENDER_SCALE : x\n",
                            "RENDER_SCALE : -1\n", "RENDER_SCALE : 999999\n", "RENDER_SCALE : 99999999999\n",
                            "RENDER_SCALE : 2.0\n", "RENDER_SCALE : +2\n", "RENDER_SCALE : 0x2\n", "RENDER_SCALE :\n",
                            "RENDER_SCALE : 2 2\n", "RENDER_SCALE : 18446744073709551618\n"}) {
        hd = parse_hd_options(bad, nullptr, nullptr, nullptr);
        EXPECT(hd.render_scale == 1 && hd.notes.size() == 1);
    }

    // The environment: surrounding blanks are fine; an invalid value is noted and leaves Settings.cfg's scale.
    EXPECT(parse_hd_options("", " 2 ", nullptr, nullptr).render_scale == 2);
    EXPECT(parse_hd_options("RENDER_SCALE : 4\n", nullptr, nullptr, nullptr).render_scale == 4);
    for (const char* bad : {"abc", "0", "-1", "5", "999999", "99999999999", "1.5", "2.0", "\t", "2abc"}) {
        hd = parse_hd_options("", bad, nullptr, nullptr);
        EXPECT(hd.render_scale == 1 && hd.notes.size() == 1);
        hd = parse_hd_options("RENDER_SCALE : 3\n", bad, nullptr, nullptr);
        EXPECT(hd.render_scale == 3 && hd.notes.size() == 1);
    }

    // Valid values carry no note; the scale needs no pack.
    hd = parse_hd_options("RENDER_SCALE : 2\n", "4", nullptr, nullptr);
    EXPECT(hd.render_scale == 4 && hd.notes.empty() && hd.texture_pack.empty());
    // SACRED_TEXTURE_CACHE_MB: 1..65536, anything else 128 with a note.
    std::string note;
    EXPECT(sacred::parse_texture_cache_mb(nullptr, &note) == 128 && note.empty());
    EXPECT(sacred::parse_texture_cache_mb("", &note) == 128 && note.empty());
    EXPECT(sacred::parse_texture_cache_mb(" 1024 ", &note) == 1024 && note.empty());
    EXPECT(sacred::parse_texture_cache_mb("1", &note) == 1 && note.empty());
    EXPECT(sacred::parse_texture_cache_mb("65536", &note) == 65536 && note.empty());
    for (const char* bad : {"0", "-5", "65537", "99999999999", "abc", "1.5", "512MB", "18446744073709551617"}) {
        note.clear();
        EXPECT(sacred::parse_texture_cache_mb(bad, &note) == 128 && !note.empty());
    }

    hd = parse_hd_options("RENDER_SCALE : 2\nTEXTURE_PACK : a\n", "3", "b", "c");
    EXPECT(hd.render_scale == 3 && hd.texture_pack == "b" && hd.texture_dump == "c");
    hd = parse_hd_options("RENDER_SCALE : 2\n", "", "", "");
    EXPECT(hd.render_scale == 2 && hd.texture_pack.empty() && hd.texture_dump.empty());

    // HIDPI (macOS drawable in pixels): on by default, but only a render scale of 2 or more asks for it; HIDPI : 0
    // or SACRED_HIDPI=0 keeps the window in points, and the environment wins over Settings.cfg.
    using sacred::hidpi_drawable;
    hd = parse_hd_options("", nullptr, nullptr, nullptr);
    EXPECT(hd.hidpi && !hidpi_drawable(hd));
    hd = parse_hd_options("RENDER_SCALE : 2\n", nullptr, nullptr, nullptr);
    EXPECT(hd.hidpi && hidpi_drawable(hd) && hd.notes.empty());
    EXPECT(hidpi_drawable(parse_hd_options("", "4", nullptr, nullptr)));
    EXPECT(!hidpi_drawable(parse_hd_options("RENDER_SCALE : 2\n", "1", nullptr, nullptr)));
    hd = parse_hd_options("RENDER_SCALE : 2\r\nHIDPI : 0\r\n", nullptr, nullptr, nullptr);
    EXPECT(!hd.hidpi && !hidpi_drawable(hd) && hd.notes.empty());
    hd = parse_hd_options("RENDER_SCALE : 2\nHIDPI : 1\n", nullptr, nullptr, nullptr);
    EXPECT(hd.hidpi && hidpi_drawable(hd));
    hd = parse_hd_options("RENDER_SCALE : 2\nHIDPI : 1\n", nullptr, nullptr, nullptr, nullptr, nullptr, "0");
    EXPECT(!hd.hidpi && !hidpi_drawable(hd) && hd.notes.empty());
    hd = parse_hd_options("RENDER_SCALE : 2\nHIDPI : 0\n", nullptr, nullptr, nullptr, nullptr, nullptr, " 1 ");
    EXPECT(hd.hidpi && hidpi_drawable(hd) && hd.notes.empty());
    EXPECT(parse_hd_options("HIDPI : 0\n", nullptr, nullptr, nullptr, nullptr, nullptr, "").hidpi == false);
    // Anything but 0 or 1 is noted and leaves the previous value (Settings.cfg's, or on).
    for (const char* bad : {"2", "-1", "off", "yes", "1.0", "01x", " "}) {
        hd = parse_hd_options(std::string("RENDER_SCALE : 2\nHIDPI : ") + bad + "\n", nullptr, nullptr, nullptr);
        EXPECT(hd.hidpi && hd.notes.size() == 1);
        hd = parse_hd_options("RENDER_SCALE : 2\nHIDPI : 0\n", nullptr, nullptr, nullptr, nullptr, nullptr, bad);
        EXPECT(!hd.hidpi && hd.notes.size() == 1);
    }

    sacred::Memory memory;
    memory.reserve(0x00500000u, 2 * sacred::Memory::page_size, "fixture");
    memory.commit(0x00500000u, sacred::Memory::page_size, sacred::Read | sacred::Write, "fixture");
    memory.commit(0x00501000u, sacred::Memory::page_size, sacred::Read, "fixture");
    const uint16_t original = 0x2a8;
    std::memcpy(reinterpret_cast<void*>(memory.host_address(0x00501000u)), &original, 2);

    // Off: cells get the original values, data stays.
    EXPECT(sacred::apply_resolution(memory, {}) == 4u);
    EXPECT(memory.load32(0x00500020u) == 100u);
    EXPECT(memory.load32(0x00500000u) == 1024u);
    EXPECT(memory.load32(0x00500010u) == 0x970f4u);
    EXPECT(f32(memory, 0x00500008u) == 384.0f);
    EXPECT(u16(memory, 0x00501000u) == 0x2a8);
    // On: cells and data (also in a read-only page) follow the configured size.
    EXPECT(sacred::apply_resolution(memory, {true, 1366, 768}) == 5u);
    EXPECT(memory.load32(0x00500000u) == 1366u);
    EXPECT(memory.load32(0x00500010u) == 0x2ef0f4u);
    EXPECT(f32(memory, 0x00500008u) == 384.0f);
    EXPECT(u16(memory, 0x00501000u) == 0x2a8 + 342);
    EXPECT(!(memory.permissions(0x00501000u) & sacred::Write));
    // The frame rate limit: every cell follows it at start; later only the F cells are rewritten.
    EXPECT(sacred::apply_resolution(memory, {}, 120) == 4u);
    EXPECT(memory.load32(0x00500020u) == 200u && memory.load32(0x00500000u) == 1024u);
    memory.store32(0x00500000u, 7u);
    EXPECT(sacred::apply_fps_limit(memory, {}, 60) == 1u);
    EXPECT(memory.load32(0x00500020u) == 100u && memory.load32(0x00500000u) == 7u);
    // FPS_LIMIT: unset 60, integers 30..240 or auto, the environment wins, invalid values are noted and ignored.
    using sacred::parse_fps_limit;
    auto fps = parse_fps_limit("SOUND : 1\r\n", nullptr);
    EXPECT(fps.fps == 60 && !fps.automatic && fps.notes.empty());
    fps = parse_fps_limit("FPS_LIMIT : 120\r\n", nullptr);
    EXPECT(fps.fps == 120 && !fps.automatic);
    fps = parse_fps_limit("FPS_LIMIT : auto\r\n", nullptr);
    EXPECT(fps.automatic && sacred::resolve_fps_limit(fps, 144) == 144 && sacred::resolve_fps_limit(fps, 0) == 60 &&
           sacred::resolve_fps_limit(fps, 360) == 240 && sacred::resolve_fps_limit(fps, 24) == 30);
    fps = parse_fps_limit("FPS_LIMIT : auto\n", "90");
    EXPECT(fps.fps == 90 && !fps.automatic && sacred::resolve_fps_limit(fps, 144) == 90);
    for (const char* bad : {"0", "29", "241", "1.5", "x", "-60", ""}) {
        fps = parse_fps_limit(std::string("FPS_LIMIT : ") + bad + "\n", nullptr);
        EXPECT(fps.fps == 60 && fps.notes.size() == 1);
    }

    EXPECT(parse_fps_limit("FPS_LIMIT : 144\n", "abc").fps == 144);
    // DISPLAY: 1-based display number, anything else is automatic (0); the last line wins.
    EXPECT(sacred::parse_display_setting("SOUND : 1\r\n") == 0);
    EXPECT(sacred::parse_display_setting("DISPLAY : 2\r\n") == 2);
    EXPECT(sacred::parse_display_setting("DISPLAY:1\nDISPLAY : 3\n") == 3);
    for (const char* bad : {"auto", "0", "-1", "x2", "", "123"}) {
        EXPECT(sacred::parse_display_setting(std::string("DISPLAY : ") + bad + "\n") == 0);
    }
    // Display choice: laptop (display 1, main) at the origin, external monitor to its left and above.
    const std::vector<sacred::DisplayRect> displays = {{0, 0, 1512, 982}, {-2560, -300, 2560, 1080}};
    auto choice = sacred::choose_display(displays, 0, true, -100, 200);
    EXPECT(choice.index == 1 && choice.source == "cursor");
    choice = sacred::choose_display(displays, 0, true, 700, 500);
    EXPECT(choice.index == 0 && choice.source == "cursor");
    choice = sacred::choose_display(displays, 1, true, -100, 200);
    EXPECT(choice.index == 0 && choice.source == "setting");
    choice = sacred::choose_display(displays, 2, false, 0, 0);
    EXPECT(choice.index == 1 && choice.source == "setting");
    // Out of range or pointer outside every display: the first display.
    choice = sacred::choose_display(displays, 3, true, 5000, 5000);
    EXPECT(choice.index == 0 && choice.source == "first");
    choice = sacred::choose_display({}, 2, true, 0, 0);
    EXPECT(choice.index == 0);
    // Edges: right and bottom edges belong to the next display.
    choice = sacred::choose_display(displays, 0, true, 0, 0);
    EXPECT(choice.index == 0);
    choice = sacred::choose_display(displays, 0, true, -1, 0);
    EXPECT(choice.index == 1);
    // Fullscreen client origin: below the notch's menu bar strip (1512x982 display, 1512x949 window), the display's
    // origin for a window as large as the display, the display's own origin on a second display.
    EXPECT((sacred::fullscreen_window_origin({0, 0, 1512, 982}, 1512, 949) == std::pair<int, int>{0, 33}));
    EXPECT((sacred::fullscreen_window_origin({0, 0, 1512, 982}, 1512, 982) == std::pair<int, int>{0, 0}));
    EXPECT((sacred::fullscreen_window_origin({1512, 0, 1920, 1080}, 1920, 1080) == std::pair<int, int>{1512, 0}));
    EXPECT((sacred::fullscreen_window_origin({-1920, -200, 1920, 1080}, 1920, 1080) == std::pair<int, int>{-1920, -200}));
    EXPECT((sacred::fullscreen_window_origin({0, 0, 1512, 982}, 1600, 1000) == std::pair<int, int>{0, 0}));
    // Settings.cfg switches: any value but 0 turns one on, the last line wins, unknown keys are ignored.
    using sacred::parse_option_settings;
    EXPECT(parse_option_settings("SOUND : 1\r\n").empty());
    EXPECT(parse_option_settings("LEGACY_UI : 1\r\n") == std::vector<std::string>{"LEGACY_UI"});
    EXPECT(parse_option_settings("LEGACY_UI:1\nLEGACY_UI : 0\n").empty());
    EXPECT(parse_option_settings("LEGACY_UI : 0\n").empty());
    EXPECT(parse_option_settings("LEGACY_UI :\n").empty());
    // GFX32 and COMPAT_VIDEO are held at 1: missing keys are appended with the file's line ending, other values are
    // replaced in place, a file that already has both stays byte for byte, and every other line is kept.
    using sacred::force_required_settings;
    std::string cfg = "LANGUAGE : US\r\nSOUND : 1\r\n";
    auto forced = force_required_settings(cfg);
    EXPECT(cfg == "LANGUAGE : US\r\nSOUND : 1\r\nGFX32 : 1\r\nCOMPAT_VIDEO : 1\r\n");
    EXPECT(forced.size() == 2 && forced[0].key == "GFX32" && forced[0].was == "missing" &&
           forced[1].key == "COMPAT_VIDEO" && forced[1].was == "missing");
    EXPECT(force_required_settings(cfg).empty());
    cfg = "COMPAT_VIDEO : 0\nFULLSCREEN : 0\nGFX32:0\nSOUND : 1";
    forced = force_required_settings(cfg);
    EXPECT(cfg == "COMPAT_VIDEO : 1\nFULLSCREEN : 0\nGFX32 : 1\nSOUND : 1");
    EXPECT(forced.size() == 2 && forced[0].key == "GFX32" && forced[0].was == "0" && forced[1].key == "COMPAT_VIDEO" &&
           forced[1].was == "0");
    const std::string already = "COMPAT_VIDEO : 1\r\nGFX32 : 1\r\nLANGUAGE : US\r\n";
    cfg = already;
    EXPECT(force_required_settings(cfg).empty() && cfg == already);
    cfg = "GFX32 : 0\r\nLANGUAGE : US";
    forced = force_required_settings(cfg);
    EXPECT(cfg == "GFX32 : 1\r\nLANGUAGE : US\r\nCOMPAT_VIDEO : 1\r\n");
    EXPECT(forced.size() == 2 && forced[0].was == "0" && forced[1].was == "missing");
    cfg.clear();
    force_required_settings(cfg);
    EXPECT(cfg == "GFX32 : 1\r\nCOMPAT_VIDEO : 1\r\n");
    // The file form rewrites only when something changed.
    const std::string path =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/hd_resolution_tests_Settings.cfg";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "GFX32 : 0\r\nSOUND : 1\r\n";
    }

    std::vector<sacred::ForcedSetting> changed;
    EXPECT(sacred::force_required_settings_file(path, changed) && changed.size() == 2);
    EXPECT(sacred::read_setting(path, "GFX32") == "1" && sacred::read_setting(path, "COMPAT_VIDEO") == "1" &&
           sacred::read_setting(path, "SOUND") == "1");
    EXPECT(sacred::force_required_settings_file(path, changed) && changed.empty());
    std::remove(path.c_str());
    EXPECT(!sacred::force_required_settings_file(path, changed));
    if (failures) {
        return 1;
    }

    std::puts("HD_RESOLUTION_TESTS_PASS");
    return 0;
}
