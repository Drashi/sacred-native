#include "generated.h"
#include "../granny/install.h"
#include "profiler.h"
#include "resolution.h"
#include "auto_pickup.h"
#include "gamepad_play.h"
#include <cstdlib>
#include <cstring>
#include <chrono>
#include "../win32/startup.h"
#include "../win32/menu.h"
#include "../ddraw/ddraw.h"
#include "../ole/com.h"
#include "../miles/miles.h"
#ifdef SACRED_QA
#include "qa_script.h"
#include "oracle.h"
#include "call_trace.h"
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include "../xml/xml.h"
#include "../win32/crt.h"
#include "../win32/registry.h"
#include "../win32/winsock.h"
#include "../../platform/window_host.h"
#include "../../platform/host_thread.h"
#include "../../platform/renderer.h"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

namespace {
struct WinMainStop {};
}

#ifdef SACRED_QA
// Local player: object manager [0xad5c40], slot [[0x182ebe8]+0x14]; position at object+0x18 (u16 sector, i32 x,
// i32 y, u8 layer), as FUN_0060f340 prints it (PPOS). Guest time: the timer manager's double at 0xcd7a58.
struct PlayerState {
    uint32_t sector;
    int32_t x, y;
    uint32_t layer;
    double guest_time;
};
static bool local_player(const sacred::Memory& memory, PlayerState& state) {
    const uint32_t net = memory.load32(0x0182ebe8u), manager = memory.load32(0x00ad5c40u);
    if (!net || !manager) {
        return false;
    }

    const uint32_t slot = memory.load32(net + 0x14), first = memory.load32(manager + 4);
    const uint32_t count = (memory.load32(manager + 8) - first) / 4;
    const uint32_t object = slot && slot < count ? memory.load32(first + slot * 4) : 0;
    if (!object) {
        return false;
    }

    const uint64_t bits = uint64_t(memory.load32(0x00cd7a58u)) | uint64_t(memory.load32(0x00cd7a5cu)) << 32;
    std::memcpy(&state.guest_time, &bits, 8);
    state.sector = memory.load32(object + 0x18) & 0xffff;
    state.x = int32_t(memory.load32(object + 0x1c));
    state.y = int32_t(memory.load32(object + 0x20));
    state.layer = memory.load32(object + 0x24) & 0xff;
    return true;
}
#endif
static bool env_flag(const char* name) {
    const char* v = std::getenv(name);
    return v && *v == '1';
}

static bool env_off(const char* name) {
    const char* v = std::getenv(name);
    return v && *v == '0';
}

int run_guest(int argc, char** argv, sacred::WindowHost* host) {
    std::string assets, profile, evidence, qa_path, audio_wav, game_module;
    bool stop = false, quiet = true;
    uint32_t fast_clock = 1;
#ifdef SACRED_QA
    bool oracle_mode = false;
    std::string oracle_checkpoint;
    const char* oracle_script = nullptr;
    int oracle_protocol = -1;
#endif
    for (int i = 1; i < argc; ++i) {
        std::string option = argv[i];
        if (option == "--stop-at-winmain") {
            stop = true;
        } else if (option == "--no-hle-trace") {
            quiet = true;
        } else if (option == "--hle-trace") {
            quiet = false;
        } else if (i + 1 < argc && option == "--assets") {
            assets = argv[++i];
        } else if (i + 1 < argc && option == "--profile") {
            profile = argv[++i];
        } else if (i + 1 < argc && option == "--evidence") {
            evidence = argv[++i];
        } else if (i + 1 < argc && option == "--game-module") {
            game_module = argv[++i];
        }
#ifdef SACRED_QA
        else if (i + 1 < argc && option == "--qa-script") {
            qa_path = argv[++i];
        } else if (i + 1 < argc && option == "--qa-audio-wav") {
            audio_wav = argv[++i];
        } else if (i + 1 < argc && option == "--qa-fast-clock") {
            fast_clock = std::max(1, std::atoi(argv[++i]));
        } else if (option == "--oracle") {
            oracle_mode = true;
        } else if (i + 1 < argc && option == "--oracle-checkpoint") {
            oracle_checkpoint = argv[++i];
        }
#endif
        else {
            std::cerr << "Unknown or incomplete option: " << option << "\n";
            return 2;
        }
    }

    if (assets.empty() || (!qa_path.empty() && evidence.empty())) {
        std::cerr << "Required: --assets PATH [--profile PATH] [--game-module PATH] [--evidence PATH] [--hle-trace] "
                     "[--stop-at-winmain]\n";
        return 2;
    }
#ifdef SACRED_QA
    if (!oracle_checkpoint.empty() && !oracle_mode) {
        std::cerr << "--oracle-checkpoint requires --oracle\n";
        return 2;
    }

    if (oracle_mode) {
        if (oracle_checkpoint.empty()) {
            oracle_checkpoint = "menu";
        }

        oracle_script = sacred::Oracle::checkpoint_script(oracle_checkpoint);
        if (!oracle_script) {
            std::cerr << "--oracle-checkpoint must be menu or world\n";
            return 2;
        }

        if (!host) {
            std::cerr << "--oracle requires the SDL host\n";
            return 2;
        }

        if (!qa_path.empty()) {
            std::cerr << "--oracle drives its own checkpoint script; --qa-script is not allowed\n";
            return 2;
        }

        if (profile.empty()) {
            std::cerr << "--oracle requires an explicit --profile outside the default profile directory\n";
            return 2;
        }

        auto chosen = std::filesystem::weakly_canonical(std::filesystem::absolute(profile));
        auto standard = std::filesystem::weakly_canonical(std::filesystem::absolute(sacred::default_profile_path()));
        auto below = chosen.lexically_relative(standard), above = standard.lexically_relative(chosen);
        if ((!below.empty() && *below.begin() != "..") || (!above.empty() && *above.begin() != "..")) {
            std::cerr << "--oracle refuses the default profile directory " << standard
                      << "; pass a private --profile\n";
            return 2;
        }
    }
#endif
    if (profile.empty()) {
        profile = sacred::default_profile_path();
    }

    auto asset_root = std::filesystem::canonical(assets);
    auto profile_root = std::filesystem::weakly_canonical(std::filesystem::absolute(profile));
    auto relative = profile_root.lexically_relative(asset_root);
    if (relative.empty() || *relative.begin() != "..") {
        std::cerr << "Private profile must be outside the installation\n";
        return 2;
    }

    std::filesystem::create_directories(profile);
#ifdef SACRED_QA
    if (oracle_mode) {
        if (evidence.empty()) {
            evidence = profile + "/oracle-evidence";
        }

        std::filesystem::create_directories(evidence);
        qa_path = evidence + "/oracle-" + oracle_checkpoint + ".script";
        std::ofstream script(qa_path, std::ios::trunc);
        if (!(script << oracle_script) || !script.flush()) {
            std::cerr << "cannot write " << qa_path << "\n";
            return 2;
        }

        // stdout carries only the protocol; everything else that writes to stdout goes to stderr.
        std::cout.flush();
        oracle_protocol = dup(STDOUT_FILENO);
        if (oracle_protocol < 0 || dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
            std::cerr << "cannot redirect stdout\n";
            return 2;
        }
    }
#endif
    std::ofstream log;
    if (!evidence.empty()) {
        std::filesystem::create_directories(evidence);
        log.open(evidence + "/run.log");
    }

    std::mutex output_mutex;
#ifdef SACRED_QA
    std::atomic<sacred::QaScript*> qa_live{nullptr};
    std::atomic<sacred::Oracle*> oracle_live{nullptr};
#endif
    auto event = [&](const std::string& line) {
        std::lock_guard<std::mutex> lock(output_mutex);
        std::cout << line << std::endl;
        if (log.is_open()) {
            log << line << std::endl;
        }
#ifdef SACRED_QA
        if (auto* live = qa_live.load()) {
            live->note(line);
        }

        if (auto* live = oracle_live.load()) {
            live->note(line);
        }
#endif
    };
    try {
        sacred::Memory memory;
        sacred::Loader loader(memory);
        for (const auto& spec : sacred::sacred_modules) {
            std::string filename = spec.name == "sacred.exe" ? "Sacred.exe" : spec.name;
            loader.map_file(assets + "/" + filename, spec);
        }

        loader.enable_native_catalog();
        loader.bind();
        sacred::Thread thread(memory, 1, loader.tls_templates());
        sacred::Control control(memory, thread);
        for (const auto& module : loader.modules()) {
            control.module(module.name, module.base, module.image_size);
        }

        const SacredGameModule& game = sacred::load_game_module(game_module, sacred::sacred_modules[0].sha256);
        sacred::register_generated(control, game);
        sacred::use_resolution_table(*game.resolution);
        sacred::install_granny_natives(control);
        sacred::StartupEnvironment environment;
        environment.assets = assets;
        environment.profile = profile;
        environment.time_scale = fast_clock;
        // QA frame-rate checks (tests/fps): SACRED_QA_FRAME_DT_US=D makes the guest clock virtual, D microseconds
        // per present once the world runs, so the game sees an exact frame rate whatever the host manages;
        // SACRED_QA_PPOS=1 logs the local player's position at every present to <evidence>/ppos.csv.
        uint64_t frame_dt_ns = 0;
        bool ppos_log = false;
        auto virtual_ns = std::make_shared<std::atomic<uint64_t>>(5000000000ull);
        auto pacing_thread = std::make_shared<std::atomic<std::thread::id>>();
        auto present_real = std::make_shared<std::atomic<uint64_t>>(0);
        auto catchup = std::make_shared<std::pair<std::mutex, uint64_t>>();
#ifdef SACRED_QA
        if (const char* v = std::getenv("SACRED_QA_FRAME_DT_US")) {
            frame_dt_ns = uint64_t(std::max(0, std::atoi(v))) * 1000;
        }

        ppos_log = env_flag("SACRED_QA_PPOS");
        if (frame_dt_ns) {
            // Without presents (loading, waits) the virtual clock follows the host clock, or dt-driven waits would
            // crawl: once 50 ms have passed since the last present, host time elapsed since then is added.
            environment.counter_ns = [virtual_ns, present_real, catchup] {
                const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                  std::chrono::steady_clock::now().time_since_epoch())
                                                  .count());
                const uint64_t idle_from = present_real->load() + 50000000ull;
                if (now > idle_from) {
                    std::lock_guard<std::mutex> lock(catchup->first);
                    const uint64_t from = std::max(catchup->second, idle_from);
                    if (now > from) {
                        virtual_ns->fetch_add(now - from);
                        catchup->second = now;
                    }
                }

                return virtual_ns->fetch_add(1000) + 1000;
            };
            // Waits of the presenting thread move the virtual clock (a zero Sleep by 200 us), so its wait loops end.
            environment.on_sleep = [virtual_ns, pacing_thread](uint32_t ms) {
                if (pacing_thread->load() == std::this_thread::get_id()) {
                    virtual_ns->fetch_add(ms ? uint64_t(ms) * 1000000 : 200000);
                }
            };
            event("QA_FRAME_DT_NS " + std::to_string(frame_dt_ns));
        }
#endif
        if (!qa_path.empty()) {
            environment.qa_marker = event;
        }
#ifdef SACRED_QA
        // Call trace (call_trace.h). Segment records carry the local player's position and the files opened.
        std::unique_ptr<sacred::CallTrace> trace = sacred::CallTrace::from_environment(
            [&memory]() -> std::string {
                try {
                    PlayerState player;
                    if (!local_player(memory, player)) {
                        return "null";
                    }

                    std::ostringstream text;
                    text << "{\"sector\":" << player.sector << ",\"x\":" << player.x << ",\"y\":" << player.y
                         << ",\"layer\":" << player.layer << ",\"guest_time\":" << std::setprecision(12)
                         << player.guest_time << "}";
                    return text.str();
                } catch (const std::exception&) {
                    return "null";
                }
            },
            event);
        // Declared after the trace: the host stops calling into it before it goes away.
        struct TraceInput {
            sacred::WindowHost* host;
            ~TraceInput() {
                if (host) {
                    host->set_input_observer({});
                }
            }
        } trace_input{trace ? host : nullptr};
        if (trace) {
            if (host) {
                host->set_input_observer(
                    [live = trace.get()](const sacred::HostEvent& input) { return live->input(input); });
            }

            environment.qa_marker = [&event, live = trace.get(), scripted = !qa_path.empty()](const std::string& line) {
                if (scripted) {
                    event(line);
                }

                if (!line.compare(0, 13, "QA_FILE_OPEN ")) {
                    live->note(line.substr(13));
                }
            };
        }
#endif
        if (fast_clock > 1) {
            event("QA_FAST_CLOCK " + std::to_string(fast_clock));
        }

        sacred::Startup startup(control, loader, environment);
        startup.bind();
        // Startup has seeded the profile's Settings.cfg (never the installation's): GFX32 and COMPAT_VIDEO are held
        // at 1 before the game reads the file (see resolution.h).
        {
            std::vector<sacred::ForcedSetting> forced;
            if (!sacred::force_required_settings_file(profile + "/Settings.cfg", forced)) {
                event("SETTINGS_FORCE_FAILED path=" + profile + "/Settings.cfg");
            }

            for (const sacred::ForcedSetting& setting : forced) {
                event("SETTINGS_FORCED key=" + setting.key + " was=" + (setting.was.empty() ? "<empty>" : setting.was));
            }
        }

        // Startup has seeded the profile's Settings.cfg; the resolution cells must hold their values before any guest
        // code runs.
        sacred::ResolutionConfig resolution = sacred::read_resolution_settings(profile + "/Settings.cfg");
        // The display to run on (DISPLAY : n, else the one under the pointer) is fixed before its size is first read.
        if (host) {
            host->prefer_display(sacred::read_display_setting(profile + "/Settings.cfg"));
        }

        // Starting fullscreen without HD_WIDTH/HD_HEIGHT: the game screen takes the size of the fullscreen window (the
        // display less a camera notch), so the frame fills it 1:1 instead of a letterboxed, scaled 1024x768.
        // SACRED_HD_AUTO=0 keeps the original size.
        if (!resolution.enabled && host && env_flag("SACRED_FULLSCREEN") && !env_off("SACRED_HD_AUTO")) {
            const auto display = host->fullscreen_size();
            resolution = sacred::display_resolution(display.first, display.second);
            if (resolution.enabled) {
                event("HD_MODE_AUTO display=" + std::to_string(display.first) + "x" + std::to_string(display.second));
            }
        }

        // Frame rate limit (Settings.cfg FPS_LIMIT, default 60 as in the original): the game's limiter, the
        // frame-counted timeouts and the vertical-blank clock follow it; see resolution.h.
        const sacred::FpsLimit fps_setting = sacred::read_fps_limit(profile + "/Settings.cfg");
        for (const std::string& note : fps_setting.notes) {
            event("FPS_LIMIT_IGNORED " + note);
        }

        const int display_hz = fps_setting.automatic && host ? host->display_refresh_rate() : 0;
        const int32_t fps_limit = sacred::resolve_fps_limit(fps_setting, display_hz);
        if (fps_limit != sacred::fps_limit_default || fps_setting.automatic) {
            event("FPS_LIMIT fps=" + std::to_string(fps_limit) +
                  (fps_setting.automatic ? " auto display_hz=" + std::to_string(display_hz) : std::string()));
        }

        const unsigned resolution_writes = sacred::apply_resolution(memory, resolution, fps_limit);
        if (resolution.enabled) {
            event("HD_MODE width=" + std::to_string(resolution.width) + " height=" + std::to_string(resolution.height) +
                  " writes=" + std::to_string(resolution_writes));
        }

        for (const std::string& option : sacred::apply_options(memory, profile + "/Settings.cfg")) {
            event("SETTINGS_OPTION " + option);
        }

        sacred::Menu menu(control, loader, profile, host, &startup);
        if (!stop) {
            menu.bind();
        }

        sacred::DirectDraw draw(control, loader, &menu, host);
        // Under the virtual clock a vertical-blank wait only costs host time.
        draw.scanout_rate(frame_dt_ns ? 1000u : uint32_t(fps_limit));
        // SHOW_FPS : 1 in Settings.cfg (or SACRED_SHOW_FPS=1, which wins) draws a frame rate counter in the top-left
        // corner (renderer.h present_fps_counter).
        {
            const char* env = std::getenv("SACRED_SHOW_FPS");
            const std::string value =
                env ? std::string(env) : sacred::read_setting(profile + "/Settings.cfg", "SHOW_FPS");
            if (!value.empty() && value != "0") {
                sacred::present_fps_counter() = true;
                event("SHOW_FPS on");
            }
        }

        // AUTO_PICKUP : 1 in Settings.cfg (or SACRED_AUTO_PICKUP=1, which wins): the hero picks up the loot next to it
        // that the game's own pick-up filter accepts (auto_pickup.h).
        std::unique_ptr<sacred::AutoPickup> auto_pickup;
        {
            const char* env = std::getenv("SACRED_AUTO_PICKUP");
            const std::string value =
                env ? std::string(env) : sacred::read_setting(profile + "/Settings.cfg", "AUTO_PICKUP");
            if (!value.empty() && value != "0") {
                auto_pickup = std::make_unique<sacred::AutoPickup>(event, value == "trace");
                event("AUTO_PICKUP on");
            }
        }

        // GAMEPAD : 1 in Settings.cfg (or SACRED_GAMEPAD=1, which wins): play with a game controller (gamepad_play.h).
        std::unique_ptr<sacred::GamepadPlay> gamepad;
        if (host) {
            const char* env = std::getenv("SACRED_GAMEPAD");
            const std::string value =
                env ? std::string(env) : sacred::read_setting(profile + "/Settings.cfg", "GAMEPAD");
            if (!value.empty() && value != "0") {
                host->enable_gamepad();
                gamepad = std::make_unique<sacred::GamepadPlay>(*host, event, value == "trace",
                                                                resolution.enabled ? resolution.width : 1024,
                                                                resolution.enabled ? resolution.height : 768);
                event("GAMEPAD on");
            }
        }

        if (!sacred::has_wait_retrace(profile + "/Settings.cfg")) {
            draw.frame_vblank_default_off = true;
            event("WAITRETRACE default=0");
        }

        if (resolution.enabled) {
            draw.configured_mode(uint32_t(resolution.width), uint32_t(resolution.height));
            if (host) {
                host->minimum_display_size(resolution.width, resolution.height);
            }

            draw.inset_word = sacred::resolution_inset_word();
            draw.observe_inset = [&menu, &gamepad](bool on, const sacred::InsetView& view) {
                menu.set_pointer_inset(on, view);
                if (gamepad) {
                    gamepad->set_inset(on, view);
                }
            };
            if (const char* trace = std::getenv("SACRED_INSET_TRACE"); trace && *trace && *trace != '0') {
                draw.trace_present = [&event](const std::string& line) {
                    event(line);
                };
            }
        }

        // Opt-in HD rendering (off by default): internal render scale and texture replacement / dump.
        sacred::HdOptions hd = sacred::read_hd_options(profile + "/Settings.cfg");
        for (const std::string& note : hd.notes) {
            event("HD_OPTIONS_IGNORED " + note);
        }

        {
            // A directory named in Settings.cfg that is gone (an older layout of the HD files) does not leave the
            // menus narrow or the textures at their original size: the wide pictures that ship next to the
            // executable (the bundle's menu-backgrounds) stand in, and a missing texture pack becomes the engine's
            // own 2x upscale.
            std::error_code error;
            const std::filesystem::path self = std::filesystem::canonical("/proc/self/exe", error);
            std::filesystem::path here;
            if (!error) {
                here = self.parent_path();
            }
#ifdef __APPLE__
            else {
                char path[4096];
                uint32_t size = sizeof path;
                if (_NSGetExecutablePath(path, &size) == 0) {
                    here = std::filesystem::weakly_canonical(path, error).parent_path();
                }
            }
#endif
            if (hd.menu_backgrounds.empty() || !std::filesystem::is_directory(hd.menu_backgrounds, error)) {
                const std::filesystem::path bundled = here / "menu-backgrounds";
                if (!here.empty() && std::filesystem::is_directory(bundled, error)) {
                    if (!hd.menu_backgrounds.empty()) {
                        event("MENU_BACKGROUNDS " + hd.menu_backgrounds + " does not exist; using " + bundled.string());
                    }

                    hd.menu_backgrounds = bundled.string();
                }
            }

            if (!hd.texture_pack.empty() && !std::filesystem::is_directory(hd.texture_pack, error) &&
                !hd.texture_upscale) {
                event("SACRED_TEXTURE_PACK " + hd.texture_pack + " does not exist; upscaling the game's textures 2x");
                hd.texture_pack.clear();
                hd.texture_upscale = 2;
            }
        }

        draw.render_scale(hd.render_scale);
#ifdef __APPLE__
        // A drawable in the display's pixels for the game window (resolution.h hidpi_drawable); the SDL_DRAWABLE line
        // of the renderer says what the display gave.
        if (host) {
            const bool hidpi = sacred::hidpi_drawable(hd);
            host->high_dpi(hidpi);
            event(std::string("HIDPI ") + (hidpi ? "on" : "off") + " render_scale=" + std::to_string(hd.render_scale) +
                  (hd.hidpi ? "" : " (HIDPI : 0 or SACRED_HIDPI=0)"));
        }
#endif
        if (resolution.enabled) {
            // The menu canvas textures (water, sky, background tiles; runtime/ddraw/menu_backdrops.h) and the user's
            // wide pictures, if any. The game loads texture.pak, then texture03.pak (later entries win); the profile
            // overlay replaces either. The split pictures are put back together on the installation's own tiles
            // (a profile texture03.pak from tools/legacy_char_pak.py repaints GUI_CHAR_xx).
            auto backdrops = std::make_shared<sacred::MenuBackdrops>(
                hd.menu_backgrounds,
                std::vector<std::filesystem::path>{startup.read_path("PAK/texture.pak"),
                                                   startup.read_path("PAK/texture03.pak")},
                std::vector<std::filesystem::path>{startup.install_path("PAK/texture.pak"),
                                                   startup.install_path("PAK/texture03.pak")});
            backdrops->wide = int64_t(resolution.width) * 768 > int64_t(resolution.height) * 1024;
            for (const std::string& note : backdrops->notes) {
                event("MENU_BACKGROUNDS " + note);
            }

            if (!hd.menu_backgrounds.empty()) {
                event("MENU_BACKGROUNDS dir=" + hd.menu_backgrounds + " images=" + std::to_string(backdrops->size()) +
                      " wide=" + std::to_string(int(backdrops->wide)) +
                      " canvas=" + std::to_string(sacred::menu_canvas_extension(resolution.width, resolution.height)));
            }

            draw.menu_backdrops(backdrops);
            menu.menu_backdrops(backdrops);
        } else if (!hd.menu_backgrounds.empty()) {
            event("MENU_BACKGROUNDS dir=" + hd.menu_backgrounds +
                  " not used: the pictures need HD mode (HD_WIDTH and "
                  "HD_HEIGHT in Settings.cfg, or fullscreen on a display other than 1024x768)");
        }

        if (!hd.texture_pack.empty() || hd.texture_upscale) {
            // Decoded (or upscaled) replacements kept in memory (SACRED_TEXTURE_CACHE_MB, default 128); the GPU keeps
            // its own copy. TEXTURE_UPSCALE makes the textures of the game's paks (as the profile overlay serves
            // them) that the pack has no file for.
            std::string cache_note;
            const size_t megabytes =
                sacred::parse_texture_cache_mb(std::getenv("SACRED_TEXTURE_CACHE_MB"), &cache_note);
            if (!cache_note.empty()) {
                event("HD_OPTIONS_IGNORED " + cache_note);
            }

            std::vector<std::string> paks;
            if (hd.texture_upscale) {
                for (const char* name : {"PAK/texture.pak", "PAK/texture03.pak"}) {
                    paks.push_back(startup.read_path(name).string());
                }
            }

            draw.texture_pack(std::make_shared<sacred::TexturePack>(
                hd.texture_pack, true, megabytes << 20, hd.texture_upscale, paks, profile + "/texture-upscale.index"));
        }

        draw.texture_dump(hd.texture_dump);
        if (hd.render_scale > 1 || !hd.texture_pack.empty() || !hd.texture_dump.empty() || hd.texture_upscale) {
            event("HD_OPTIONS render_scale=" + std::to_string(hd.render_scale) +
                  " texture_pack=" + (hd.texture_pack.empty() ? "-" : hd.texture_pack) +
                  " texture_upscale=" + std::to_string(hd.texture_upscale) +
                  " texture_dump=" + (hd.texture_dump.empty() ? "-" : hd.texture_dump));
        }

        if (!stop) {
            draw.bind();
        }

        sacred::Miles miles(
            control, loader, host, [&startup](const std::string& name) { return startup.read_path(name); }, event,
            audio_wav);
        if (!stop) {
            miles.bind();
        }

        sacred::Com com(
            control, loader, event, &draw, host,
            [&startup](const std::string& name) { return startup.read_path(name); }, audio_wav,
            [&miles] { return miles.mixer(); });
        if (!stop) {
            com.bind();
        }

        sacred::Xml xml(control, loader, &startup);
        if (!stop) {
            xml.bind();
        }

        sacred::Crt crt_functions(control, loader, startup);
        if (!stop) {
            crt_functions.bind();
        }

        sacred::Registry registry(control, loader);
        if (!stop) {
            registry.bind();
        }

        sacred::Winsock winsock(control, loader);
        if (!stop) {
            winsock.bind();
        }

        bool first_present = false, first_3d = false, drew = false, gdi = false, first_gdi = false;
        std::atomic<uint64_t> presents{0};
#ifdef SACRED_QA
        std::unique_ptr<sacred::Oracle> oracle;
#endif
        // A network game keeps the original 60: the game's limiter pumps the network while it waits and the network
        // code keeps a frame-indexed history (0x7cb740), neither checked at other rates. The limiter's own test of
        // a network session (byte 0x182ebec) switches the limit back to 60 and to FPS_LIMIT again after it.
        bool fps_network = false;
        std::ofstream ppos_file;
        draw.observe_present = [&](uint32_t window) {
            ++presents;
            pacing_thread->store(std::this_thread::get_id());
            // Only once the engine's render thread runs (its first-run flag, bit 0 of 0xad6ef4): menus and movies
            // keep host time, so recorded QA scripts reach the world as usual.
            if (frame_dt_ns && (memory.load32(0x00ad6ef4u) & 1)) {
                virtual_ns->fetch_add(frame_dt_ns);
                present_real->store(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                 std::chrono::steady_clock::now().time_since_epoch())
                                                 .count()));
            }

            if (ppos_log && !evidence.empty()) {
                if (!ppos_file.is_open()) {
                    ppos_file.open(evidence + "/ppos.csv");
                    ppos_file << "present,host_ns,guest_time,sector,x,y,layer\n";
                }
#ifdef SACRED_QA
                PlayerState player;
                if (local_player(memory, player)) {
                    const uint64_t host_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                          std::chrono::steady_clock::now().time_since_epoch())
                                                          .count());
                    ppos_file << presents.load() << ',' << host_ns << ',' << std::setprecision(12) << player.guest_time
                              << ',' << player.sector << ',' << player.x << ',' << player.y << ',' << player.layer
                              << '\n';
                }
#endif
            }

            if (auto_pickup) {
                auto_pickup->present(sacred::active_control());
            }

            if (gamepad) {
                gamepad->present(sacred::active_control());
            }

            if (fps_limit != sacred::fps_limit_default) {
                const bool network = (memory.load32(0x0182ebecu) & 0xffu) != 0;
                if (network != fps_network) {
                    fps_network = network;
                    const int32_t fps = network ? sacred::fps_limit_default : fps_limit;
                    sacred::apply_fps_limit(memory, resolution, fps);
                    if (!frame_dt_ns) {
                        draw.scanout_rate(uint32_t(fps));
                    }

                    event("FPS_LIMIT_NETWORK " + std::string(network ? "on" : "off") + " fps=" + std::to_string(fps));
                }
            }
#ifdef SACRED_QA
            if (trace) {
                trace->frame();
            }

            if (oracle) {
                oracle->frame();
            }

            uint32_t left = 0;
            if (auto* live = qa_live.load(); live && !evidence.empty()) {
                left = live->record.load();
                while (left && !live->record.compare_exchange_weak(left, left - 1)) {
                }
            }

            if (left) {
                char name[32];
                std::snprintf(name, sizeof name, "/rec/%06llu.bmp", static_cast<unsigned long long>(presents.load()));
                std::filesystem::create_directories(evidence + "/rec");
                host->capture_next(window, evidence + name, false);
            }
#endif
            if (evidence.empty()) {
                return;
            }

            if (!first_present) {
                host->capture_next(window, evidence + "/native-first-present.bmp");
                first_present = true;
                event("CAPTURE_REQUEST native-first-present.bmp source=SDL_drawable_before_swap");
            }

            if (drew && !first_3d) {
                host->capture_next(window, evidence + "/native-first-3d.bmp");
                first_3d = true;
                event("CAPTURE_REQUEST native-first-3d.bmp source=SDL_drawable_before_swap");
            }

            if (gdi && !first_gdi) {
                host->capture_next(window, evidence + "/native-first-gdi.bmp");
                first_gdi = true;
                event("CAPTURE_REQUEST native-first-gdi.bmp source=SDL_drawable_before_swap");
            }
        };
#ifdef SACRED_QA
        if (oracle_mode) {
            oracle = std::make_unique<sacred::Oracle>(*host, oracle_checkpoint, oracle_protocol, asset_root, evidence,
                                                      event);
            oracle_live = oracle.get();
        }

        std::unique_ptr<sacred::QaScript> qa;
        if (!qa_path.empty()) {
            if (!host) {
                throw std::runtime_error("--qa-script requires the SDL host");
            }

            qa = std::make_unique<sacred::QaScript>(
                *host, qa_path, evidence, [host] { return host->first_window(); }, presents, event);
            qa_live = qa.get();
        }

        struct QaLiveReset {
            std::atomic<sacred::QaScript*>& live;
            std::atomic<sacred::Oracle*>& oracle;
            ~QaLiveReset() {
                live = nullptr;
                oracle = nullptr;
            }
        } qa_live_reset{qa_live, oracle_live};
#endif
        unsigned attachments = 0;
        bool crt = false;
        // Declared after every subsystem and observer so it runs first on unwind: guest threads are stopped and
        // joined before anything they call into is destroyed.
        struct ThreadTeardown {
            sacred::Startup& startup;
            ~ThreadTeardown() { startup.shutdown_threads(); }
        } thread_teardown{startup};
        control.observe_entry = [&](uint32_t address) {
            if (address == 0x84d964) {
                crt = true;
                event("CRT_ENTRY pc=0084d964");
            }

            if (address != 0x817b30) {
                return;
            }
#ifdef SACRED_QA
            // Original-process checkpoint asserts: QA builds only. A production run must not turn a harmless
            // environment difference (launcher arguments, FPU control word) into a fatal error.
            if (!crt || attachments != 2 || control.source_pc != 0x84da3f) {
                throw sacred::GuestFault("WinMain did not follow real DLL attach and CRT startup");
            }

            uint32_t sp = control.cpu.r[R_ESP];
            if (memory.load32(sp) != 0x84da44 || memory.load32(sp + 4) != 0x400000 || memory.load32(sp + 8) != 0 ||
                memory.load32(sp + 16) != 10 || control.cpu.r[R_EBP] != sp + 0x88)
                throw sacred::GuestFault("WinMain original call/argument checkpoint mismatch");
            uint8_t tail;
            memory.read(memory.load32(sp + 12), &tail, 1);
            if (tail) {
                throw sacred::GuestFault("WinMain command-line tail is not empty");
            }

            if (control.cpu.r[R_EAX] != 0x400000 || control.cpu.r[R_EBX] || control.cpu.r[R_ESI] ||
                control.cpu.r[R_EDI] || control.cpu.fpu_cw != 0x023f || control.cpu.fpu_sw != 0x0020 ||
                control.cpu.fpu_top || control.cpu.fpu_tag != 0xffff || control.cpu.mxcsr != 0x1f80 ||
                (x86_get_eflags(&control.cpu) & 0x400))
                throw sacred::GuestFault("WinMain original CPU/FPU checkpoint mismatch");
            std::ostringstream state;
            state << std::hex << std::setfill('0') << "WINMAIN pc=00817b30 source=0084da3f";
            for (unsigned i = 0; i < 8; ++i) {
                state << " r" << i << "=" << std::setw(8) << control.cpu.r[i];
            }

            state << " flags=" << x86_get_eflags(&control.cpu) << " fcw=" << control.cpu.fpu_cw
                  << " fsw=" << control.cpu.fpu_sw << " top=" << control.cpu.fpu_top << " tag=" << control.cpu.fpu_tag
                  << " fs=" << control.cpu.fs_base << " mxcsr=" << control.cpu.mxcsr << " stack=";
            for (unsigned i = 0; i < 5; ++i) {
                state << std::setw(8) << memory.load32(sp + i * 4) << ",";
            }

            event(state.str());
#else
            (void)crt;
#endif
            if (stop) {
                event("STRICT_STOP before_first_WinMain_instruction post_WinMain_import=not_executed");
                throw WinMainStop{};
            }

            event("WINMAIN_CONTINUE strict_imports=enabled");
            control.stop_observing_entries(); // nothing left to observe after WinMain
            sacred::Control::enable_direct_calls(!env_off("SACRED_DIRECT_CALLS"));
#ifdef SACRED_QA
            if (trace) {
                trace->start(&control); // turns the direct calls off again
            }
#endif
            if (!quiet)
                control.observe_host = [&](const std::string& name, uint32_t source, const std::vector<uint64_t>& args,
                                           sacred::ReturnChannel channel, const sacred::Result& result) {
                    if (name == "gdi32.dll!BitBlt" && result.integer) {
                        gdi = true;
                    }

                    if (!result.integer && ((name == "IDirect3DDevice7::DrawPrimitive" && args[4]) ||
                                            (name == "IDirect3DDevice7::DrawIndexedPrimitive" && args[4] && args[6])))
                        drew = true;
                    std::ostringstream trace;
                    auto& caller = sacred::active_control();
                    trace << "HLE tid=" << caller.memory.load32(caller.cpu.fs_base + 0x24) << " name=" << name
                          << " source=" << std::hex << source << " args=";
                    for (uint64_t argument : args) {
                        trace << argument << ',';
                    }

                    if (channel == sacred::ReturnChannel::Void) {
                        trace << " return=void";
                    } else if (channel == sacred::ReturnChannel::OutBuffer) {
                        trace << " return=out-buffer";
                    } else if (channel == sacred::ReturnChannel::St0) {
                        trace << " return_st0=" << result.floating.signExp << ':' << result.floating.signif;
                    } else {
                        trace << " return=" << result.integer;
                    }

                    event(trace.str());
                };
        };
        const sacred::Signature dll_signature{
            sacred::Convention::Stdcall, sacred::ReturnChannel::Eax, {{4}, {4}, {4}}, ""};
        uint32_t entry = loader.initialize(thread, [&](const sacred::InitCall& init) {
            std::ostringstream message;
            message << "DLL_ATTACH_BEGIN module=" << init.module << " pc=" << std::hex << init.target;
            event(message.str());
            auto result = control.invoke(init.target, dll_signature, {init.instance, init.reason, init.reserved});
            if (result.integer) {
                ++attachments;
                event("DLL_ATTACH_COMPLETE module=" + init.module);
            }

            return result.integer != 0;
        });
        control.cpu.fpu_cw = 0x027f;         // Original process-entry profile.
        x86_set_eflags(&control.cpu, 0x246); // Same original process-entry reference.
        control.invoke(entry, {sacred::Convention::Stdcall, sacred::ReturnChannel::Void, {}, ""}, {});
        throw sacred::GuestFault("CRT returned without reaching WinMain");
    } catch (const sacred::ProcessExit& exit) {
        event("PROCESS_EXIT code=" + std::to_string(exit.code));
        return int(exit.code);
    } catch (const WinMainStop&) {
        event("CLEAN_EXIT status=0");
        return 0;
    } catch (const std::exception& error) {
        event(std::string(stop ? "STRICT_FAILURE " : "RUN_FAILURE ") + error.what());
        return 1;
    }
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--stop-at-winmain") {
            return run_guest(argc, argv, nullptr);
        }
    }

    sacred::start_profiler();
    sacred::WindowHost host;
    int result = 1;
    sacred::HostThread guest([&] {
        try {
            result = run_guest(argc, argv, &host);
            host.finish();
        }

        catch (const sacred::HostStopped&) {
            std::cerr << "HOST_STOPPED guest shut down after the window host exited\n";
        } catch (const std::exception& error) {
            std::cerr << "RUN_FAILURE " << error.what() << '\n';
        }

        // finish() must run on every path or the host loop never sees done_.
        try {
            host.finish();
        } catch (const std::exception& error) {
            std::cerr << "host finish failure: " << error.what() << '\n';
        }
    });
    try {
        host.loop();
        host.finish();
    }

    catch (const std::exception& error) {
        std::cerr << "host loop failure: " << error.what() << '\n';
    }

    guest.join();
    return result;
}
