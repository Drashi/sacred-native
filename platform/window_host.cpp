#include "qa.h"
#include "window_host.h"
#include "letterbox.h"
#include "display_choice.h"
#include "display_safe_area.h"
#include "../runtime/process/memory.h"
#include <SDL.h>
#include <algorithm>
#include <future>
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>

namespace sacred {
std::string default_profile_path() {
    char* path = SDL_GetPrefPath("Sacred Native", "Sacred Gold");
    if (!path) {
        throw GuestFault(std::string("cannot select private profile: ") + SDL_GetError());
    }

    std::string result(path);
    SDL_free(path);
    return result;
}

WindowHost::~WindowHost() {
    size_t windows = windows_.size();
    size_t contexts = renderers_.size();
    renderers_.clear();
    for (auto& window : windows_) {
        SDL_DestroyWindow(window.second);
    }

    if (arrow_) {
        SDL_FreeCursor(arrow_);
    }

    if (video_) {
        SDL_Quit();
    }

    std::fprintf(stderr, "SDL_TEARDOWN windows_destroyed=%zu contexts_destroyed=%zu cursor_destroyed=%u SDL_quit=%u\n",
                 windows, contexts, arrow_ ? 1u : 0u, video_ ? 1u : 0u);
}

void WindowHost::video() {
    if (video_) {
        return;
    }

    set_app_identity();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        throw GuestFault(std::string("SDL video initialization failed: ") + SDL_GetError());
    }

    // The project's logo for every window (and on macOS the Dock); without the bundle's file the guest's icon.
    load_app_icon(app_icon_);

    std::fprintf(stderr, "SDL_VIDEO driver=%s\n", SDL_GetCurrentVideoDriver());
    std::lock_guard<std::mutex> lock(mutex_);
    video_ = true;
}

std::string WindowHost::start_audio() {
    std::string error;
    submit([&] {
        video();
        if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            error = SDL_GetError();
        } else {
            std::fprintf(stderr, "SDL_AUDIO driver=%s devices=%d\n", SDL_GetCurrentAudioDriver(),
                         SDL_GetNumAudioDevices(0));
        }
    });
    return error;
}

void WindowHost::stop_audio() {
    submit([] {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        std::fprintf(stderr, "SDL_AUDIO_STOP\n");
    });
}

uint32_t WindowHost::open_audio(uint32_t frequency, std::string& error) {
    uint32_t device = 0;
    submit([&] {
        SDL_AudioSpec requested{}, obtained{};
        requested.freq = frequency;
        requested.format = AUDIO_S16LSB;
        requested.channels = 2;
        requested.samples = 1024;
        device = SDL_OpenAudioDevice(nullptr, 0, &requested, &obtained, 0);
        if (!device) {
            error = SDL_GetError();
        } else {
            SDL_PauseAudioDevice(device, 0);
            std::fprintf(stderr, "SDL_AUDIO_OPEN device=%u frequency=%d bits=%u channels=%u\n", device, obtained.freq,
                         SDL_AUDIO_BITSIZE(obtained.format), obtained.channels);
        }
    });
    return device;
}

void WindowHost::close_audio(uint32_t device) {
    submit([&] {
        SDL_CloseAudioDevice(device);
        std::fprintf(stderr, "SDL_AUDIO_CLOSE device=%u\n", device);
    });
}

void WindowHost::queue_audio(uint32_t device, const void* data, uint32_t bytes) {
    SDL_QueueAudio(device, data, bytes);
}

uint32_t WindowHost::queued_audio(uint32_t device) {
    return SDL_GetQueuedAudioSize(device);
}

void WindowHost::clear_audio(uint32_t device) {
    SDL_ClearQueuedAudio(device);
}

void WindowHost::wake() {
    // Called with mutex_: synchronize publication with the initial SDL setup.
    // The SDL main thread is either blocked in SDL_WaitEventTimeout (sdl_waiting_, set under mutex_ after it saw an
    // empty queue) or waiting on changed_ for the next command of a burst; a Cocoa wakeup event is only needed then.
    if (video_ && sdl_waiting_) {
        SDL_Event event{};
        event.type = SDL_USEREVENT;
        if (SDL_PushEvent(&event) < 0) {
            throw GuestFault("cannot wake SDL host");
        }
    } else {
        changed_.notify_one();
    }
}

// Caller holds mutex_: waits for queue space (posted commands may fill it) and reports a failure of an earlier
// posted command, so it surfaces at the guest's next host request.
void WindowHost::admit(std::unique_lock<std::mutex>& lock) {
    space_.wait(lock, [&] { return stopped_.load() || done_ || commands_.size() < 64; });
    if (stopped_) {
        throw HostStopped();
    }

    if (done_) {
        throw GuestFault("invalid host command queue submission");
    }

    if (posted_error_) {
        std::rethrow_exception(std::exchange(posted_error_, nullptr));
    }
}

// Caller holds draw_mutex_: hands the pending draw calls to the renderer, ahead of whatever is queued next.
void WindowHost::flush_draws() {
    if (pending_draws_.items.empty()) {
        return;
    }

    auto batch = std::make_shared<DrawBatch>(std::move(pending_draws_));
    pending_draws_ = DrawBatch{};
    pending_draws_.states.reserve(batch->states.size());
    pending_draws_.items.reserve(batch->items.size());
    pending_draws_.vertices.reserve(batch->vertices.size());
    pending_draws_.indices.reserve(batch->indices.size());
    pending_draws_.raw.reserve(batch->raw.size());
    enqueue([this, id = draw_window_, batch] {
        auto window = windows_.find(id);
        if (window == windows_.end()) {
            throw GuestFault("unknown renderer window");
        }

        auto renderer = renderers_.find(id);
        if (renderer == renderers_.end()) {
            renderer = renderers_.emplace(id, make_renderer(window->second)).first;
        }

        expand_raw_vertices(*batch);
        renderer->second->draw(*batch);
    });
}

// Queues a command without waiting for it.
void WindowHost::enqueue(std::function<void()> command) {
    std::unique_lock<std::mutex> lock(mutex_);
    admit(lock);
    commands_.push_back([this, command = std::move(command)](bool abandon) {
        if (abandon) {
            return;
        }

        try {
            command();
        } catch (...) {
            std::lock_guard<std::mutex> error_lock(mutex_);
            if (!posted_error_) {
                posted_error_ = std::current_exception();
            }
        }
    });
    wake();
}

void WindowHost::submit(std::function<void()> command) {
    auto complete = std::make_shared<std::promise<void>>();
    auto result = complete->get_future();
    {
        std::lock_guard<std::mutex> draws(draw_mutex_);
        flush_draws();
        std::unique_lock<std::mutex> lock(mutex_);
        admit(lock);
        commands_.push_back([command = std::move(command), complete](bool abandon) {
            try {
                if (abandon) {
                    throw HostStopped();
                }

                command();
                complete->set_value();
            }

            catch (...) {
                complete->set_exception(std::current_exception());
            }
        });
        wake();
    }

    result.get();
    std::lock_guard<std::mutex> lock(mutex_);
    if (posted_error_) {
        std::rethrow_exception(std::exchange(posted_error_, nullptr));
    }
}

bool WindowHost::screen_saver() {
    bool enabled = false;
    submit([&] {
        video();
        enabled = SDL_IsScreenSaverEnabled() == SDL_TRUE;
    });
    return enabled;
}

void WindowHost::screen_saver(bool enabled) {
    submit([&] {
        video();
        if (enabled) {
            SDL_EnableScreenSaver();
        } else {
            SDL_DisableScreenSaver();
        }
    });
}

void WindowHost::load_arrow_cursor() {
    submit([&] {
        video();
        if (!arrow_) {
            arrow_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
        }

        if (!arrow_) {
            throw GuestFault(std::string("cannot create arrow cursor: ") + SDL_GetError());
        }
    });
}

bool WindowHost::hide_cursor() {
    bool previous = false;
    submit([&] {
        video();
        int visible = SDL_ShowCursor(SDL_QUERY);
        if (visible < 0) {
            throw GuestFault("cannot query native cursor visibility");
        }

        previous = visible == SDL_ENABLE;
        if (SDL_ShowCursor(SDL_DISABLE) < 0) {
            throw GuestFault("cannot hide native cursor");
        }
    });
    return previous;
}

// SDL thread only. SDL's display 0 is the main display (on macOS the one with the menu bar), which need not be the
// one the player looks at: a laptop with an external monitor has the laptop screen as display 0 unless the menu
// bar was moved. The game gets the display under the pointer instead, unless DISPLAY / SACRED_DISPLAY names one.
void WindowHost::select_display() {
    video();
    if (display_selected_) {
        return;
    }

    display_selected_ = true;
    std::vector<DisplayRect> displays;
    const int count = SDL_GetNumVideoDisplays();
    for (int i = 0; i < count; ++i) {
        SDL_Rect bounds{};
        if (SDL_GetDisplayBounds(i, &bounds)) {
            break;
        }
        displays.push_back({bounds.x, bounds.y, bounds.w, bounds.h});
    }

    int requested = preferred_display_.load();
    const char* env = std::getenv("SACRED_DISPLAY");
    if (env && *env) {
        requested = parse_display_number(env);
    }

    int pointer_x = 0, pointer_y = 0;
    SDL_GetGlobalMouseState(&pointer_x, &pointer_y);
    const DisplayChoice choice = choose_display(displays, requested, true, pointer_x, pointer_y);
    display_ = choice.index;
    if (!displays.empty()) {
        display_x_ = displays[size_t(display_)].x;
        display_y_ = displays[size_t(display_)].y;
    }

    for (size_t i = 0; i < displays.size(); ++i) {
        const char* name = SDL_GetDisplayName(int(i));
        std::fprintf(stderr, "SACRED_DISPLAY_LIST number=%zu name=%s bounds=%d,%d %dx%d%s\n", i + 1, name ? name : "?",
                     displays[i].x, displays[i].y, displays[i].width, displays[i].height,
                     int(i) == display_ ? " chosen" : "");
    }
    std::fprintf(stderr, "SACRED_DISPLAY number=%d of=%zu source=%s requested=%d pointer=%d,%d\n", display_ + 1,
                 displays.size(), choice.source.c_str(), requested, pointer_x, pointer_y);
}

std::pair<int, int> WindowHost::display_size() {
    std::pair<int, int> size;
    submit([&] {
        select_display();
        SDL_Rect bounds{};
        if (SDL_GetDisplayBounds(display_, &bounds)) {
            throw GuestFault("cannot query SDL display bounds");
        }

        size = {bounds.w, bounds.h};
    });
    // A configured game screen larger than the host display still fits the guest desktop; presentation scales it.
    size.first = std::max(size.first, minimum_display_.first.load());
    size.second = std::max(size.second, minimum_display_.second.load());
    return size;
}

std::pair<int, int> WindowHost::fullscreen_size() {
    std::pair<int, int> size;
    submit([&] {
        select_display();
        SDL_Rect bounds{};
        if (SDL_GetDisplayBounds(display_, &bounds)) {
            throw GuestFault("cannot query SDL display bounds");
        }

        const DisplayInsets insets = display_safe_insets(bounds.x, bounds.y, bounds.w, bounds.h);
        size = {std::max(1, bounds.w - insets.left - insets.right), std::max(1, bounds.h - insets.top - insets.bottom)};
        std::fprintf(stderr, "SACRED_DISPLAY_FULLSCREEN number=%d bounds=%dx%d insets=%d,%d,%d,%d size=%dx%d\n",
                     display_ + 1, bounds.w, bounds.h, insets.top, insets.left, insets.bottom, insets.right, size.first,
                     size.second);
    });
    return size;
}

int WindowHost::display_refresh_rate() {
    int hertz = 0;
    submit([&] {
        select_display();
        SDL_DisplayMode mode{};
        if (SDL_GetCurrentDisplayMode(display_, &mode) == 0) {
            hertz = mode.refresh_rate;
        }
    });
    return hertz;
}

uint32_t WindowHost::create_window(const std::string& title, int x, int y, int width, int height, bool borderless,
                                   const std::vector<uint8_t>& icon, int icon_width, int icon_height) {
    uint32_t id = 0;
    submit([&] {
        select_display();
        configure_renderer_window();
        uint32_t flags = SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN | (borderless ? uint32_t(SDL_WINDOW_BORDERLESS) : 0u);
#ifdef __APPLE__
        if (high_dpi_) {
            flags |= SDL_WINDOW_ALLOW_HIGHDPI;
        }
#endif
        // Guest screen coordinates are relative to the chosen display.
        auto* window = SDL_CreateWindow(title.c_str(), x + display_x_, y + display_y_, width, height, flags);
        if (!window) {
            throw GuestFault(std::string("SDL window creation failed: ") + SDL_GetError());
        }

        id = SDL_GetWindowID(window);
        windows_.emplace(id, window);
        logical_[id] = {x, y, width, height, false};
        if (!app_icon_.rgba.empty()) {
            set_window_icon(window, app_icon_);
        } else if (!icon.empty()) {
            auto* surface = SDL_CreateRGBSurfaceWithFormat(0, icon_width, icon_height, 32, SDL_PIXELFORMAT_RGBA32);
            if (!surface) {
                throw GuestFault("cannot allocate window icon surface");
            }

            for (int row = 0; row < icon_height; ++row) {
                std::memcpy(static_cast<uint8_t*>(surface->pixels) + row * surface->pitch,
                            icon.data() + row * icon_width * 4, size_t(icon_width) * 4);
            }

            SDL_SetWindowIcon(window, surface);
            SDL_FreeSurface(surface);
        }

        std::fprintf(stderr, "SDL_WINDOW id=%u title=%s width=%d height=%d hidden=1\n", id, title.c_str(), width,
                     height);
    });
    return id;
}

void WindowHost::show_window(uint32_t id, bool visible) {
    submit([&] {
        auto found = windows_.find(id);
        if (found == windows_.end()) {
            throw GuestFault("unknown native window");
        }

        if (visible) {
            SDL_ShowWindow(found->second);
            SDL_RaiseWindow(found->second);
            // SACRED_FULLSCREEN=1 starts the first shown window fullscreen (Alt+Enter toggles it later).
            static bool start_fullscreen = [] {
                const char* v = std::getenv("SACRED_FULLSCREEN");
                return v && *v == '1';
            }();
            auto logical = logical_.find(id);
            if (start_fullscreen && logical != logical_.end() && !logical->second.fullscreen) {
                start_fullscreen = false;
                toggle_fullscreen(id);
            }
        } else {
            SDL_HideWindow(found->second);
        }

        pump_due_ = true;
    });
}

void WindowHost::raise_window(uint32_t id) {
    submit([&] {
        auto found = windows_.find(id);
        if (found == windows_.end()) {
            throw GuestFault("unknown native window");
        }

        SDL_RaiseWindow(found->second);
        pump_due_ = true;
    });
}

void WindowHost::position_window(uint32_t id, int x, int y, int width, int height) {
    submit([&] {
        auto found = windows_.find(id);
        if (found == windows_.end()) {
            throw GuestFault("unknown native window");
        }

        LogicalWindow& logical = logical_[id];
        logical.x = x;
        logical.y = y;
        logical.width = width;
        logical.height = height;
        // A fullscreen window keeps covering the display; the guest geometry is presented scaled into it.
        if (!logical.fullscreen) {
            SDL_SetWindowPosition(found->second, x + display_x_, y + display_y_);
            SDL_SetWindowSize(found->second, width, height);
        }

        SDL_ShowWindow(found->second);
        SDL_RaiseWindow(found->second);
        pump_due_ = true;
        std::fprintf(stderr, "SDL_POSITION id=%u x=%d y=%d width=%d height=%d\n", id, x, y, width, height);
    });
}

void WindowHost::toggle_fullscreen(uint32_t id) {
    // SDL thread only.
    auto found = windows_.find(id);
    auto logical = logical_.find(id);
    if (found == windows_.end() || logical == logical_.end()) {
        return;
    }

    LogicalWindow& state = logical->second;
    if (!state.fullscreen) {
        if (SDL_SetWindowFullscreen(found->second, SDL_WINDOW_FULLSCREEN_DESKTOP)) {
            std::fprintf(stderr, "native fullscreen refused: %s\n", SDL_GetError());
            return;
        }

        state.fullscreen = true;
    } else {
        SDL_SetWindowFullscreen(found->second, 0);
        SDL_SetWindowSize(found->second, state.width, state.height);
        SDL_SetWindowPosition(found->second, state.x + display_x_, state.y + display_y_);
        state.fullscreen = false;
    }

    std::fprintf(stderr, "SDL_FULLSCREEN id=%u fullscreen=%d\n", id, state.fullscreen ? 1 : 0);
}

void WindowHost::destroy_window(uint32_t id) {
    submit([&] {
        auto found = windows_.find(id);
        if (found == windows_.end()) {
            throw GuestFault("unknown native window");
        }

        renderers_.erase(id);
        logical_.erase(id);
        SDL_DestroyWindow(found->second);
        windows_.erase(found);
    });
}

void WindowHost::erase_window(uint32_t id, uint32_t argb) {
    render(id, [=](Renderer& renderer) {
        renderer.erase(argb);
        std::fprintf(stderr, "WINDOW_ERASE id=%u argb=%08x\n", id, argb);
    });
}

void WindowHost::render(uint32_t id, const std::function<void(Renderer&)>& command) {
    submit([&] {
        auto window = windows_.find(id);
        if (window == windows_.end()) {
            throw GuestFault("unknown renderer window");
        }

        auto renderer = renderers_.find(id);
        if (renderer == renderers_.end()) {
            renderer = renderers_.emplace(id, make_renderer(window->second)).first;
        }

        command(*renderer->second);
    });
}

void WindowHost::render_posted(uint32_t id, std::function<void(Renderer&)> command) {
    std::lock_guard<std::mutex> draws(draw_mutex_);
    flush_draws();
    enqueue([this, id, command = std::move(command)] {
        auto window = windows_.find(id);
        if (window == windows_.end()) {
            throw GuestFault("unknown renderer window");
        }

        auto renderer = renderers_.find(id);
        if (renderer == renderers_.end()) {
            renderer = renderers_.emplace(id, make_renderer(window->second)).first;
        }

        command(*renderer->second);
    });
}

void WindowHost::retire_surface(uint32_t window, uint32_t surface) {
    submit([&] {
        auto renderer = renderers_.find(window);
        // Destroying the SDL window already retired this renderer and all its
        // resources; later COM release still retires its CPU/guest ownership.
        if (renderer != renderers_.end()) {
            renderer->second->destroy_surface(surface);
        }
    });
}

void WindowHost::capture_next(uint32_t window, const std::string& path, bool storage) {
    render(window, [&](Renderer& renderer) { renderer.capture_next(path, storage); });
}

HostWindowState WindowHost::window_state(uint32_t id) {
    HostWindowState state{};
    submit([&] { state = host_window_state(id); });
    return state;
}

// Host thread only.
HostWindowState WindowHost::host_window_state(uint32_t id) {
    HostWindowState state{};
    auto found = windows_.find(id);
    if (found == windows_.end()) {
        throw GuestFault("unknown native window");
    }

    SDL_GetWindowPosition(found->second, &state.x, &state.y);
    state.x -= display_x_;
    state.y -= display_y_;
    SDL_GetWindowSize(found->second, &state.width, &state.height);
    // The guest sees the client area it asked for; the host window may be larger (fullscreen) or reshaped by
    // the window manager, and presentation scales the guest image into it.
    auto logical = logical_.find(id);
    if (logical != logical_.end()) {
        state.width = logical->second.width;
        state.height = logical->second.height;
        if (logical->second.fullscreen) {
            state.x = logical->second.x;
            state.y = logical->second.y;
        }
    }

    uint32_t flags = SDL_GetWindowFlags(found->second);
    state.visible = flags & SDL_WINDOW_SHOWN;
    state.minimized = flags & SDL_WINDOW_MINIMIZED;
    state.focused = flags & SDL_WINDOW_INPUT_FOCUS;
    return state;
}

static bool qa_input_log() {
    static const bool enabled = qa_env("SACRED_QA_INPUT_LOG");
    return enabled;
}

// QA only (SACRED_QA_INPUT_LOG): the SDL-side capture, grab, focus, cursor-visibility and button state at every
// focus, pointer-focus and button transition, so a stalled-mouse session shows which of them diverged from the host's.
static void log_pointer_state(const HostEvent& event, uint32_t host_buttons) {
    switch (event.kind) {
    case HostEventKind::FocusGained:
    case HostEventKind::FocusLost:
    case HostEventKind::Enter:
    case HostEventKind::Leave:
    case HostEventKind::MouseDown:
    case HostEventKind::MouseUp:
    case HostEventKind::Minimized:
    case HostEventKind::Restored:
    case HostEventKind::Shown:
    case HostEventKind::Hidden:
        break;
    default:
        return;
    }

    SDL_Window* mouse = SDL_GetMouseFocus();
    SDL_Window* keyboard = SDL_GetKeyboardFocus();
    SDL_Window* grabbed = SDL_GetGrabbedWindow();
    const uint32_t sdl_buttons = SDL_GetMouseState(nullptr, nullptr);
    const uint32_t flags = mouse ? SDL_GetWindowFlags(mouse) : 0;
    std::fprintf(
        stderr,
        "QA_INPUT sdl_state after_kind=%d mouse_focus=%u keyboard_focus=%u grabbed_window=%u mouse_grab=%d "
        "relative_mode=%d cursor_visible=%d sdl_buttons=0x%x host_buttons=0x%x event_buttons=0x%x window_flags=0x%x\n",
        int(event.kind), mouse ? SDL_GetWindowID(mouse) : 0u, keyboard ? SDL_GetWindowID(keyboard) : 0u,
        grabbed ? SDL_GetWindowID(grabbed) : 0u, mouse && SDL_GetWindowGrab(mouse) ? 1 : 0,
        SDL_GetRelativeMouseMode() ? 1 : 0, SDL_ShowCursor(SDL_QUERY), sdl_buttons, host_buttons, event.buttons, flags);
}

void WindowHost::pump(const SDL_Event* initial) {
    SDL_Event input;
    bool pending = initial != nullptr;
    if (initial) {
        input = *initial;
    }

    while (pending || SDL_PollEvent(&input)) {
        pending = false;
        HostEvent event{HostEventKind::Exposed,
                        0,
                        uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count()),
                        0,
                        0,
                        0,
                        {}};
        switch (input.type) {
        case SDL_USEREVENT:
            continue;
        case SDL_WINDOWEVENT:
            event.window = input.window.windowID;
            event.x = input.window.data1;
            event.y = input.window.data2;
            switch (input.window.event) {
            case SDL_WINDOWEVENT_SHOWN:
                event.kind = HostEventKind::Shown;
                break;
            case SDL_WINDOWEVENT_HIDDEN:
                event.kind = HostEventKind::Hidden;
                break;
            case SDL_WINDOWEVENT_EXPOSED:
                event.kind = HostEventKind::Exposed;
                break;
            case SDL_WINDOWEVENT_MOVED:
                event.kind = HostEventKind::Moved;
                break;
            case SDL_WINDOWEVENT_RESIZED:
                event.kind = HostEventKind::Resized;
                break;
            case SDL_WINDOWEVENT_SIZE_CHANGED:
                continue; // RESIZED carries the same logical change.
            case SDL_WINDOWEVENT_FOCUS_GAINED:
                event.kind = HostEventKind::FocusGained;
                break;
            case SDL_WINDOWEVENT_FOCUS_LOST:
                event.kind = HostEventKind::FocusLost;
                break;
            case SDL_WINDOWEVENT_CLOSE:
                event.kind = HostEventKind::Close;
                break;
            case SDL_WINDOWEVENT_ENTER:
                event.kind = HostEventKind::Enter;
                break;
            case SDL_WINDOWEVENT_LEAVE:
                event.kind = HostEventKind::Leave;
                break;
            case SDL_WINDOWEVENT_MINIMIZED:
                event.kind = HostEventKind::Minimized;
                break;
            case SDL_WINDOWEVENT_MAXIMIZED:
                event.kind = HostEventKind::Maximized;
                break;
            case SDL_WINDOWEVENT_RESTORED:
                event.kind = HostEventKind::Restored;
                break;
            default:
                continue;
            }

            break;
        case SDL_MOUSEMOTION:
            if (scripted_pointer_ && input.motion.which != injected_pointer) {
                continue;
            }

            event.kind = HostEventKind::MouseMove;
            event.window = input.motion.windowID;
            event.x = input.motion.x;
            event.y = input.motion.y;
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            if (scripted_pointer_ && input.button.which != injected_pointer) {
                continue;
            }

            event.kind = input.type == SDL_MOUSEBUTTONDOWN ? HostEventKind::MouseDown : HostEventKind::MouseUp;
            event.window = input.button.windowID;
            event.x = input.button.x;
            event.y = input.button.y;
            event.value = input.button.button;
            break;
        case SDL_MOUSEWHEEL:
            event.kind = HostEventKind::Wheel;
            event.window = input.wheel.windowID;
            event.x = input.wheel.x;
            event.y = input.wheel.y;
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            event.kind = input.type == SDL_KEYDOWN ? HostEventKind::KeyDown : HostEventKind::KeyUp;
            event.window = input.key.windowID;
            auto key = input.key.keysym.sym;
            // Alt+Enter toggles host fullscreen and never reaches the guest (neither the press nor its release).
            if (key == SDLK_RETURN && input.type == SDL_KEYDOWN && (input.key.keysym.mod & KMOD_ALT)) {
                if (!input.key.repeat) {
                    toggle_fullscreen(input.key.windowID);
                }

                fullscreen_key_held_ = true;
                continue;
            }

            if (key == SDLK_RETURN && input.type == SDL_KEYUP && fullscreen_key_held_) {
                fullscreen_key_held_ = false;
                continue;
            }

            if (key >= SDLK_a && key <= SDLK_z) {
                event.value = key - SDLK_a + 'A';
            } else if (key >= SDLK_0 && key <= SDLK_9) {
                event.value = key;
            } else {
                switch (key) {
                case SDLK_RETURN:
                    event.value = 13;
                    break;
                case SDLK_ESCAPE:
                    event.value = 27;
                    break;
                case SDLK_SPACE:
                    event.value = 32;
                    break;
                case SDLK_UP:
                    event.value = 38;
                    break;
                case SDLK_DOWN:
                    event.value = 40;
                    break;
                case SDLK_LEFT:
                    event.value = 37;
                    break;
                case SDLK_RIGHT:
                    event.value = 39;
                    break;
                case SDLK_TAB:
                    event.value = 9;
                    break;
                case SDLK_BACKSPACE:
                    event.value = 8;
                    break;
                case SDLK_LSHIFT:
                case SDLK_RSHIFT:
                    event.value = 16;
                    break;
                case SDLK_LCTRL:
                case SDLK_RCTRL:
                    event.value = 17;
                    break;
                case SDLK_LALT:
                case SDLK_RALT:
                    event.value = 18;
                    break;
                case SDLK_PAUSE:
                    event.value = 19;
                    break;
                case SDLK_CAPSLOCK:
                    event.value = 20;
                    break;
                case SDLK_PAGEUP:
                    event.value = 33;
                    break;
                case SDLK_PAGEDOWN:
                    event.value = 34;
                    break;
                case SDLK_END:
                    event.value = 35;
                    break;
                case SDLK_HOME:
                    event.value = 36;
                    break;
                case SDLK_PRINTSCREEN:
                    event.value = 44;
                    break;
                case SDLK_INSERT:
                    event.value = 45;
                    break;
                case SDLK_DELETE:
                    event.value = 46;
                    break;
                case SDLK_LGUI:
                    event.value = 91;
                    break;
                case SDLK_RGUI:
                    event.value = 92;
                    break;
                case SDLK_APPLICATION:
                    event.value = 93;
                    break;
                case SDLK_KP_ENTER:
                    event.value = 13;
                    break;
                case SDLK_KP_MULTIPLY:
                    event.value = 106;
                    break;
                case SDLK_KP_PLUS:
                    event.value = 107;
                    break;
                case SDLK_KP_MINUS:
                    event.value = 109;
                    break;
                case SDLK_KP_PERIOD:
                    event.value = 110;
                    break;
                case SDLK_KP_DIVIDE:
                    event.value = 111;
                    break;
                case SDLK_NUMLOCKCLEAR:
                    event.value = 144;
                    break;
                case SDLK_SCROLLLOCK:
                    event.value = 145;
                    break;
                case SDLK_SEMICOLON:
                    event.value = 186;
                    break;
                case SDLK_EQUALS:
                    event.value = 187;
                    break;
                case SDLK_COMMA:
                    event.value = 188;
                    break;
                case SDLK_MINUS:
                    event.value = 189;
                    break;
                case SDLK_PERIOD:
                    event.value = 190;
                    break;
                case SDLK_SLASH:
                    event.value = 191;
                    break;
                case SDLK_BACKQUOTE:
                    event.value = 192;
                    break;
                case SDLK_LEFTBRACKET:
                    event.value = 219;
                    break;
                case SDLK_BACKSLASH:
                    event.value = 220;
                    break;
                case SDLK_RIGHTBRACKET:
                    event.value = 221;
                    break;
                case SDLK_QUOTE:
                    event.value = 222;
                    break;
                default:
                    if (key >= SDLK_F1 && key <= SDLK_F12) {
                        event.value = int(112 + (key - SDLK_F1));
                        break;
                    }

                    if (key >= SDLK_KP_1 && key <= SDLK_KP_9) {
                        event.value = int(97 + (key - SDLK_KP_1));
                        break;
                    }

                    if (key == SDLK_KP_0) {
                        event.value = 96;
                        break;
                    }

                    continue; // A host key with no Windows virtual-key equivalent is dropped, never a guest fault.
                }
            }

            event.x = input.key.keysym.scancode;
            event.y = input.key.repeat;
            break;
        }
        case SDL_TEXTINPUT:
            event.kind = HostEventKind::Text;
            event.window = input.text.windowID;
            event.text = input.text.text;
            break;
        case SDL_QUIT:
            for (const auto& window : windows_) {
                std::lock_guard<std::mutex> lock(mutex_);
                events_.push_back({HostEventKind::Close, window.first, event.time, 0, 0, 0, {}});
            }

            continue;
        case SDL_CONTROLLERDEVICEADDED:
        case SDL_CONTROLLERDEVICEREMOVED:
        case SDL_CONTROLLERAXISMOTION:
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
            gamepad_event(input);
            continue;
        default:
            continue; // Backend notifications without a registered guest consumer.
        }

        std::function<void()> notify;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (event.kind == HostEventKind::MouseMove || event.kind == HostEventKind::MouseDown ||
                event.kind == HostEventKind::MouseUp) {
                if (event.window) {
                    VirtualCursor& cursor = cursors_[event.window];
                    bool live = false;
                    int live_x = 0, live_y = 0;
                    if (SDL_Window* native = SDL_GetWindowFromID(event.window)) {
                        int width = 0, height = 0, origin_x = 0, origin_y = 0;
                        SDL_GetWindowSize(native, &width, &height);
                        SDL_GetWindowPosition(native, &origin_x, &origin_y);
                        // Host positions are in the host window; the guest works in its own client area, which
                        // presentation letterboxes into the host window (identity when the sizes agree).
                        int guest_width = width, guest_height = height;
                        auto logical = logical_.find(event.window);
                        if (logical != logical_.end()) {
                            guest_width = logical->second.width;
                            guest_height = logical->second.height;
                        }

                        const Letterbox box = fit_letterbox(guest_width, guest_height, width, height);
                        cursor.set_bounds(guest_width, guest_height);
                        cursor.set_origin(origin_x, origin_y);
                        // Re-anchoring checks a real pointer event against the live pointer: after a window move or
                        // resize macOS still delivers queued motion relative to the old frame, and a delta from such
                        // an anchor would keep the guest cursor offset (one window edge unreachable) for good.
                        const bool real = input.type == SDL_MOUSEMOTION ? input.motion.which != injected_pointer
                                                                        : input.button.which != injected_pointer;
                        // Injected QA input is already in guest coordinates.
                        // With bars or scaling, a position on the bars is clamped to the image edge first: the
                        // bars belong to the host window, so motion there must not push the delta anchor past the
                        // guest client area. An unscaled window keeps the original unclamped host positions.
                        const bool letterboxed = box.scaled || box.x || box.y;
                        auto to_guest = [&](int& x, int& y) {
                            if (!letterboxed) {
                                return;
                            }

                            letterbox_to_source(box, guest_width, guest_height, x, y);
                            x = std::clamp(x, 0, guest_width - 1);
                            y = std::clamp(y, 0, guest_height - 1);
                        };
                        if (real) {
                            to_guest(event.x, event.y);
                        }

                        if (!cursor.anchored() && real) {
                            SDL_GetGlobalMouseState(&live_x, &live_y);
                            live_x -= origin_x;
                            live_y -= origin_y;
                            to_guest(live_x, live_y);
                            live = std::abs(live_x - event.x) > 2 || std::abs(live_y - event.y) > 2;
                        }
                    }

                    if (live) {
                        cursor.place(live_x, live_y);
                    } else {
                        cursor.host_motion(event.x, event.y);
                    }

                    event.x = cursor.x();
                    event.y = cursor.y();
                } else {
                    event.x = input_.x;
                    event.y = input_.y;
                }
            }

            if (input_observer_ &&
                (event.kind == HostEventKind::KeyDown || event.kind == HostEventKind::KeyUp ||
                 event.kind == HostEventKind::MouseDown || event.kind == HostEventKind::MouseUp ||
                 event.kind == HostEventKind::FocusLost || event.kind == HostEventKind::Wheel) &&
                input_observer_(event) &&
                (event.kind == HostEventKind::KeyDown || event.kind == HostEventKind::KeyUp)) {
                continue;
            }

            switch (event.kind) {
            case HostEventKind::MouseMove:
                input_.window = event.window;
                input_.x = event.x;
                input_.y = event.y;
                break;
            case HostEventKind::MouseDown:
            case HostEventKind::MouseUp: {
                const uint32_t bit = event.value == 1 ? 1 : event.value == 3 ? 2 : event.value == 2 ? 4 : 0;
                input_.window = event.window;
                input_.x = event.x;
                input_.y = event.y;
                if (event.kind == HostEventKind::MouseDown) {
                    input_.buttons |= bit;
                } else {
                    input_.buttons &= ~bit;
                }

                break;
            }
            case HostEventKind::KeyDown:
                input_.keys.set(size_t(event.value));
                break;
            case HostEventKind::KeyUp:
                input_.keys.reset(size_t(event.value));
                break;
            case HostEventKind::FocusLost:
                event.buttons = input_.buttons;
                input_.keys.reset();
                input_.buttons = 0;
                break;
            default:
                break;
            }

            if (event.kind != HostEventKind::FocusLost) {
                event.buttons = input_.buttons;
            }

            if (qa_input_log()) {
                log_pointer_state(event, input_.buttons);
            }

            if (event.kind == HostEventKind::MouseMove && !events_.empty() &&
                events_.back().kind == HostEventKind::MouseMove && events_.back().window == event.window) {
                events_.back() = std::move(event);
            } else {
                drop_oldest_if_full();
                events_.push_back(std::move(event));
            }

            notify = notifier_;
        }

        if (notify) {
            notify();
        }
    }
}

uint32_t WindowHost::first_window() {
    uint32_t id = 0;
    submit([&] {
        if (!windows_.empty()) {
            id = windows_.begin()->first;
        }
    });
    return id;
}

void WindowHost::script_pointer() {
    scripted_pointer_ = true;
}

void WindowHost::inject_mouse(uint32_t window, int x, int y) {
    SDL_Event event{};
    event.type = SDL_MOUSEMOTION;
    event.motion.windowID = window;
    event.motion.which = injected_pointer;
    event.motion.x = x;
    event.motion.y = y;
    if (SDL_PushEvent(&event) < 0) {
        throw GuestFault("cannot inject native mouse motion");
    }
}

void WindowHost::inject_button(uint32_t window, int x, int y, int button, bool down) {
    SDL_Event event{};
    event.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    event.button.windowID = window;
    event.button.which = injected_pointer;
    event.button.button = uint8_t(button);
    event.button.state = down ? SDL_PRESSED : SDL_RELEASED;
    event.button.clicks = 1;
    event.button.x = x;
    event.button.y = y;
    if (SDL_PushEvent(&event) < 0) {
        throw GuestFault("cannot inject native mouse button");
    }
}

void WindowHost::inject_wheel(uint32_t window, int y) {
    SDL_Event event{};
    event.type = SDL_MOUSEWHEEL;
    event.wheel.windowID = window;
    event.wheel.y = y;
    if (SDL_PushEvent(&event) < 0) {
        throw GuestFault("cannot inject native mouse wheel");
    }
}

void WindowHost::inject_key(uint32_t window, const std::string& name, bool down) {
    const SDL_Keycode key = SDL_GetKeyFromName(name.c_str());
    if (key == SDLK_UNKNOWN) {
        throw GuestFault("unknown native key name: " + name);
    }

    SDL_Event event{};
    event.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    event.key.windowID = window;
    event.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    event.key.keysym.sym = key;
    event.key.keysym.scancode = SDL_GetScancodeFromKey(key);
    if (SDL_PushEvent(&event) < 0) {
        throw GuestFault("cannot inject native key");
    }
}

void WindowHost::inject_text(uint32_t window, const std::string& text) {
    SDL_Event event{};
    event.type = SDL_TEXTINPUT;
    event.text.windowID = window;
    std::snprintf(event.text.text, sizeof event.text.text, "%s", text.c_str());
    if (SDL_PushEvent(&event) < 0) {
        throw GuestFault("cannot inject native text");
    }
}

bool WindowHost::take_event(HostEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (events_.empty()) {
        return false;
    }

    event = std::move(events_.front());
    events_.pop_front();
    return true;
}

HostInputState WindowHost::input_state() {
    std::lock_guard<std::mutex> lock(mutex_);
    return input_;
}

void WindowHost::set_event_notifier(std::function<void()> notifier) {
    std::lock_guard<std::mutex> lock(mutex_);
    notifier_ = std::move(notifier);
}

void WindowHost::set_input_observer(std::function<bool(const HostEvent&)> observer) {
    std::lock_guard<std::mutex> lock(mutex_);
    input_observer_ = std::move(observer);
}

std::pair<int, int> WindowHost::global_mouse() {
    std::pair<int, int> position;
    submit([&] {
        select_display();
        SDL_GetGlobalMouseState(&position.first, &position.second);
        position.first -= display_x_;
        position.second -= display_y_;
    });
    return position;
}

void WindowHost::set_cursor_pos(int x, int y) {
    uint32_t window;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        window = input_.window;
    }

    if (!window) {
        window = first_window();
    }

    if (!window) {
        return;
    }

    const HostWindowState origin = window_state(window);
    std::function<void()> notify;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        VirtualCursor& cursor = cursors_[window];
        cursor.set(x - origin.x, y - origin.y);
        input_.window = window;
        input_.x = cursor.x();
        input_.y = cursor.y();
        HostEvent event{HostEventKind::MouseMove,
                        window,
                        uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count()),
                        cursor.x(),
                        cursor.y(),
                        0,
                        {},
                        input_.buttons};
        if (!events_.empty() && events_.back().kind == HostEventKind::MouseMove && events_.back().window == window) {
            events_.back() = std::move(event);
        } else {
            drop_oldest_if_full();
            events_.push_back(std::move(event));
        }

        notify = notifier_;
    }

    if (notify) {
        notify();
    }
}

void WindowHost::enable_gamepad() {
    submit([&] {
        video();
        if (gamepad_enabled_) {
            return;
        }

        // SDL reports the controllers present at start as added devices; they are opened there.
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER)) {
            std::fprintf(stderr, "GAMEPAD unavailable: %s\n", SDL_GetError());
            return;
        }

        gamepad_enabled_ = true;
        std::fprintf(stderr, "GAMEPAD on joysticks=%d\n", SDL_NumJoysticks());
    });
}

GamepadState WindowHost::gamepad_state() {
    std::lock_guard<std::mutex> lock(mutex_);
    return gamepad_;
}

void WindowHost::gamepad_event(const SDL_Event& input) {
    if (!gamepad_enabled_) {
        return;
    }

    if (input.type == SDL_CONTROLLERDEVICEADDED) {
        if (SDL_GameController* controller = SDL_GameControllerOpen(input.cdevice.which)) {
            const char* name = SDL_GameControllerName(controller);
            std::fprintf(stderr, "GAMEPAD added \"%s\"\n", name ? name : "?");
        }

        return;
    }

    const SDL_JoystickID id = input.type == SDL_CONTROLLERDEVICEREMOVED ? input.cdevice.which
                              : input.type == SDL_CONTROLLERAXISMOTION  ? input.caxis.which
                                                                        : input.cbutton.which;
    SDL_GameController* controller = SDL_GameControllerFromInstanceID(id);
    if (input.type == SDL_CONTROLLERDEVICEREMOVED) {
        if (controller) {
            SDL_GameControllerClose(controller);
        }

        std::fprintf(stderr, "GAMEPAD removed\n");
        if (id == gamepad_active_) {
            gamepad_active_ = -1;
            std::lock_guard<std::mutex> lock(mutex_);
            gamepad_ = {};
        }

        return;
    }

    if (!controller) {
        return;
    }

    // A stick resting slightly off its centre must not take over from the controller in use.
    if (id != gamepad_active_ && input.type == SDL_CONTROLLERAXISMOTION && std::abs(int(input.caxis.value)) < 12000) {
        return;
    }

    gamepad_active_ = id;
    auto axis = [&](SDL_GameControllerAxis which) {
        return std::clamp(float(SDL_GameControllerGetAxis(controller, which)) / 32767.0f, -1.0f, 1.0f);
    };
    GamepadState state;
    state.connected = true;
    state.lx = axis(SDL_CONTROLLER_AXIS_LEFTX);
    state.ly = axis(SDL_CONTROLLER_AXIS_LEFTY);
    state.rx = axis(SDL_CONTROLLER_AXIS_RIGHTX);
    state.ry = axis(SDL_CONTROLLER_AXIS_RIGHTY);
    state.lt = std::max(0.0f, axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT));
    state.rt = std::max(0.0f, axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT));
    static constexpr std::pair<SDL_GameControllerButton, uint32_t> buttons[] = {
        {SDL_CONTROLLER_BUTTON_A, pad::A},
        {SDL_CONTROLLER_BUTTON_B, pad::B},
        {SDL_CONTROLLER_BUTTON_X, pad::X},
        {SDL_CONTROLLER_BUTTON_Y, pad::Y},
        {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, pad::LB},
        {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, pad::RB},
        {SDL_CONTROLLER_BUTTON_BACK, pad::Back},
        {SDL_CONTROLLER_BUTTON_START, pad::Start},
        {SDL_CONTROLLER_BUTTON_LEFTSTICK, pad::L3},
        {SDL_CONTROLLER_BUTTON_RIGHTSTICK, pad::R3},
        {SDL_CONTROLLER_BUTTON_DPAD_UP, pad::Up},
        {SDL_CONTROLLER_BUTTON_DPAD_DOWN, pad::Down},
        {SDL_CONTROLLER_BUTTON_DPAD_LEFT, pad::Left},
        {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, pad::Right},
    };
    for (const auto& [button, bit] : buttons) {
        if (SDL_GameControllerGetButton(controller, button)) {
            state.buttons |= bit;
        }
    }

    if (state.lt > pad::trigger_press) {
        state.buttons |= pad::LT;
    }

    if (state.rt > pad::trigger_press) {
        state.buttons |= pad::RT;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    gamepad_ = state;
}

void WindowHost::deliver_pad(HostEvent event) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        event.window = input_.window;
    }

    if (!event.window) {
        event.window = first_window();
        if (!event.window) {
            return;
        }
    }

    event.time = uint32_t(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
    std::function<void()> notify;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        VirtualCursor& cursor = cursors_[event.window];
        switch (event.kind) {
        case HostEventKind::MouseMove:
            cursor.set(event.x, event.y);
            event.x = cursor.x();
            event.y = cursor.y();
            input_.x = event.x;
            input_.y = event.y;
            break;
        case HostEventKind::MouseDown:
        case HostEventKind::MouseUp: {
            const uint32_t bit = event.value == 1 ? 1 : event.value == 3 ? 2 : 4;
            event.x = input_.x;
            event.y = input_.y;
            if (event.kind == HostEventKind::MouseDown) {
                input_.buttons |= bit;
            } else {
                input_.buttons &= ~bit;
            }

            break;
        }
        case HostEventKind::KeyDown:
            input_.keys.set(size_t(event.value & 0xff));
            break;
        case HostEventKind::KeyUp:
            input_.keys.reset(size_t(event.value & 0xff));
            break;
        default:
            break;
        }

        input_.window = event.window;
        event.buttons = input_.buttons;
        if (event.kind == HostEventKind::MouseMove && !events_.empty() &&
            events_.back().kind == HostEventKind::MouseMove && events_.back().window == event.window) {
            events_.back() = std::move(event);
        } else {
            drop_oldest_if_full();
            events_.push_back(std::move(event));
        }

        notify = notifier_;
    }

    if (notify) {
        notify();
    }
}

void WindowHost::pad_pointer(int x, int y) {
    HostEvent event{HostEventKind::MouseMove, 0, 0, x, y, 0, {}};
    deliver_pad(std::move(event));
}

void WindowHost::pad_button(int button, bool down) {
    HostEvent event{down ? HostEventKind::MouseDown : HostEventKind::MouseUp, 0, 0, 0, 0, button, {}};
    deliver_pad(std::move(event));
}

void WindowHost::pad_key(int key, bool down) {
    HostEvent event{down ? HostEventKind::KeyDown : HostEventKind::KeyUp, 0, 0, 0, 0, key, {}};
    deliver_pad(std::move(event));
    // A key that types a character gives SDL text input too, as the keyboard does: the game takes letters, digits and
    // the space bar (its potion and window keys among them) from WM_CHAR, which only that text produces.
    if (down && (key == ' ' || (key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z'))) {
        HostEvent text{
            HostEventKind::Text, 0, 0, 0, 0, 0, std::string(1, char(key >= 'A' && key <= 'Z' ? key + 32 : key))};
        deliver_pad(std::move(text));
    }
}

void WindowHost::pad_wheel(int notches) {
    HostEvent event{HostEventKind::Wheel, 0, 0, 0, notches, 0, {}};
    deliver_pad(std::move(event));
}

void WindowHost::pad_key_tap(int key, int hold_ms) {
    pad_key(key, true);
    std::lock_guard<std::mutex> lock(mutex_);
    taps_.emplace_back(key, std::chrono::steady_clock::now() + std::chrono::milliseconds(hold_ms));
}

void WindowHost::release_taps() {
    std::function<void()> notify;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        for (auto tap = taps_.begin(); tap != taps_.end();) {
            if (tap->second > now || !input_.window) {
                ++tap;
                continue;
            }

            input_.keys.reset(size_t(tap->first & 0xff));
            HostEvent event{
                HostEventKind::KeyUp,
                input_.window,
                uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()),
                0,
                0,
                tap->first,
                {},
                input_.buttons};
            drop_oldest_if_full();
            events_.push_back(std::move(event));
            notify = notifier_;
            tap = taps_.erase(tap);
        }
    }

    if (notify) {
        notify();
    }
}

void WindowHost::drop_oldest_if_full() {
    // Caller holds mutex_. A stalled guest must not turn host input into a fatal error: drop the oldest event.
    if (events_.size() < 256) {
        return;
    }

    events_.pop_front();
    if (!dropped_logged_) {
        dropped_logged_ = true;
        std::cerr << "native input queue full: dropping oldest events\n";
    }
}

void WindowHost::finish() {
    std::lock_guard<std::mutex> lock(mutex_);
    done_ = true;
    space_.notify_all();
    wake();
}

void WindowHost::shutdown() {
    std::deque<Command> pending;
    std::function<void()> notify;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
        pending.swap(commands_);
        space_.notify_all();
        // After a normal finish() the guest is done and its notifier target may already be gone; only an abnormal loop
        // exit has a guest that still needs waking.
        if (!done_) {
            notify = notifier_;
        }
    }

    for (auto& command : pending) {
        command(true);
    }

    if (notify) {
        notify();
    }
}

void WindowHost::loop() {
    try {
        run_loop();
    } catch (...) {
        shutdown();
        throw;
    }

    shutdown();
}

void WindowHost::run_loop() {
    bool burst = false;
    for (;;) {
        release_taps();
        Command command;
        bool video;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!video_) {
                changed_.wait(lock, [&] { return done_ || !commands_.empty(); });
            } else if (commands_.empty() && !done_ && burst) {
                // The guest submits rendering in bursts (one synchronous command per draw call). Waiting briefly
                // for the next one is far cheaper than a Cocoa event-loop pass and a wakeup event per command.
                changed_.wait_for(lock, std::chrono::microseconds(500), [&] { return done_ || !commands_.empty(); });
            }

            if (!commands_.empty()) {
                command = std::move(commands_.front());
                commands_.pop_front();
                space_.notify_all();
            } else if (done_) {
                return;
            }

            video = video_;
            if (!command && video) {
                sdl_waiting_ = true;
            }
        }

        burst = bool(command);
        if (command) {
            command(false);
            // Pump host events between commands at most every 2 ms: a Cocoa pump (run-loop observers, Core Animation
            // commit, gesture bookkeeping) costs far more than most commands, and the guest thread waits for each one.
            // A command that shows, raises or moves a window pumps at once: the guest's next question is usually
            // GetForegroundWindow/IsIconic, and the focus and map events must be in the window flags by then.
            if (video_) {
                const auto now = std::chrono::steady_clock::now();
                if (pump_due_ || now - last_command_pump_ >= std::chrono::milliseconds(2)) {
                    pump_due_ = false;
                    last_command_pump_ = now;
                    pump();
                }
            }

            continue;
        }

        if (video) {
            SDL_Event event{};
            // SDL's cross-thread wakeup is edge-triggered and can be lost between the queue check
            // above and this wait; the bounded wait guarantees a queued command is never stranded.
            const int status = SDL_WaitEventTimeout(&event, 20);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                sdl_waiting_ = false;
            }

            if (status < 0) {
                // Never throw on the SDL main thread: that would std::terminate. Log and stop pumping.
                std::cerr << "SDL event wait failed: " << SDL_GetError() << '\n';
                return;
            }

            if (status) {
                pump(&event);
            }
        }
    }
}
}
