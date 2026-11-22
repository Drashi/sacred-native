#pragma once
#include "gamepad.h"
#include "renderer.h"
#include "virtual_cursor.h"
#include <atomic>
#include <bitset>
#include <condition_variable>
#include <deque>
#include <exception>
#include <memory>
#include <functional>
#include <mutex>
#include <utility>
#include <chrono>
#include <map>
#include <string>
#include <vector>
#include <cstdint>

struct SDL_Cursor;
struct SDL_Window;
union SDL_Event;
namespace sacred {
std::string default_profile_path();
// Thrown to the guest side once the host loop has exited: every pending and future host command fails with this,
// and the guest unwinds into a clean shutdown instead of blocking on a loop that no longer runs.
struct HostStopped : std::exception {
    const char* what() const noexcept override { return "native window host stopped"; }
};
enum class HostEventKind {
    Shown,
    Hidden,
    Exposed,
    Moved,
    Resized,
    FocusGained,
    FocusLost,
    Close,
    MouseMove,
    MouseDown,
    MouseUp,
    KeyDown,
    KeyUp,
    Text,
    Enter,
    Leave,
    Minimized,
    Restored,
    Maximized,
    Wheel
};
struct HostEvent {
    HostEventKind kind;
    uint32_t window, time;
    int x = 0, y = 0, value = 0;
    std::string text;
    // Mouse buttons (bit 1 left, 2 right, 4 middle) held when the event happened; for FocusLost the buttons that were
    // held at the moment the host reset its button state.
    uint32_t buttons = 0;
};
struct HostInputState {
    uint32_t window = 0, buttons = 0;
    int x = 0, y = 0;
    std::bitset<256> keys;
};
struct HostWindowState {
    int x, y, width, height;
    bool visible, minimized, focused;
};
// Main-thread SDL ownership; the serialized guest worker never pumps SDL.
class WindowHost {
public:
    ~WindowHost();
    void loop();
    void finish();
    // Called when the host loop exits: fails every pending/future command with HostStopped and fires the event
    // notifier.
    void shutdown();
    bool stopped() const { return stopped_.load(); }
    bool screen_saver();
    void screen_saver(bool enabled);
    void load_arrow_cursor();
    bool hide_cursor();
    // The guest desktop: the host display, but at least the size set with minimum_display_size().
    std::pair<int, int> display_size();
    // Refresh rate of the host display in Hz, 0 when SDL does not know it.
    int display_refresh_rate();
    void minimum_display_size(int width, int height) {
        minimum_display_.first = width;
        minimum_display_.second = height;
    }

    uint32_t create_window(const std::string& title, int x, int y, int width, int height, bool borderless,
                           const std::vector<uint8_t>& icon, int icon_width, int icon_height);
    void show_window(uint32_t window, bool visible);
    void raise_window(uint32_t window);
    void position_window(uint32_t window, int x, int y, int width, int height);
    void destroy_window(uint32_t window);
    // The guest client area of a window; window_state() reports it even when the host window has another size.
    HostWindowState window_state(uint32_t window);
    void erase_window(uint32_t window, uint32_t argb);
    void render(uint32_t window, const std::function<void(Renderer&)>& command);
    // Queues a rendering command without waiting for it, in order with every other host command. A failure is
    // reported by the guest's next host request. The command runs on the host thread, where it may ask for
    // window geometry with host_window_state().
    void render_posted(uint32_t window, std::function<void(Renderer&)> command);
    HostWindowState host_window_state(uint32_t window);
    // Draw calls: `append` adds items to the window's pending batch. The guest does not wait for them. A batch is
    // queued for the renderer when it is full and before any other host command, so draw calls stay in order with
    // everything else; a failure is reported by the guest's next host request. `append` must not call the host.
    template <class Append> void draw(uint32_t window, Append&& append) {
        std::lock_guard<std::mutex> lock(draw_mutex_);
        if (draw_window_ != window) {
            flush_draws();
            draw_window_ = window;
        }

        append(pending_draws_);
        if (pending_draws_.items.size() >= 64 || pending_draws_.vertices.size() >= 16384 ||
            pending_draws_.raw.size() >= 16384 * 32) {
            flush_draws();
        }
    }

    void retire_surface(uint32_t window, uint32_t surface);
    void capture_next(uint32_t window, const std::string& path, bool storage = true);
    bool take_event(HostEvent& event);
    uint32_t first_window();
    // A replayed QA script owns the pointer: events not produced by inject_mouse/inject_button are dropped.
    void script_pointer();
    void inject_mouse(uint32_t window, int x, int y);
    void inject_button(uint32_t window, int x, int y, int button, bool down);
    void inject_key(uint32_t window, const std::string& name, bool down);
    void inject_wheel(uint32_t window, int y);
    void inject_text(uint32_t window, const std::string& text);
    HostInputState input_state();
    void set_event_notifier(std::function<void()> notifier);
    // QA: sees every key, mouse-button, wheel and focus-loss event on the SDL thread before the guest does (mouse
    // positions in guest coordinates); returning true keeps a key event from the guest. Called with the host lock held:
    // it must be quick and must not call the host.
    void set_input_observer(std::function<bool(const HostEvent&)> observer);
    std::pair<int, int> global_mouse();
    // Moves the guest-visible cursor (screen coordinates) without warping the host pointer.
    void set_cursor_pos(int x, int y);
    // Game controllers (GAMEPAD : 1): opens the controllers and keeps the state of the one that last gave input.
    // Without this call the controller subsystem is never started.
    void enable_gamepad();
    GamepadState gamepad_state();
    // Input a controller stands in for, delivered to the guest like the mouse's and the keyboard's (the window that
    // last saw input): the guest cursor moves to (x, y) in client coordinates; a button (1 left, 3 right) or a key
    // (Windows virtual-key code) goes down or up at the cursor; the wheel turns by whole notches.
    void pad_pointer(int x, int y);
    void pad_button(int button, bool down);
    void pad_key(int key, bool down);
    void pad_wheel(int notches);
    // A key pressed now and released `hold_ms` later by the host loop, whatever the guest does meanwhile: the movie
    // player waits for Esc to come up again without presenting a frame (Sacred.exe 0x6a147c).
    void pad_key_tap(int key, int hold_ms);
    std::string start_audio();
    void stop_audio();
    uint32_t open_audio(uint32_t frequency, std::string& error);
    void close_audio(uint32_t device);
    // Thread-safe PCM hand-off for the Miles mixer thread (signed 16-bit stereo).
    void queue_audio(uint32_t device, const void* data, uint32_t bytes);
    uint32_t queued_audio(uint32_t device);
    void clear_audio(uint32_t device);

private:
    using Command = std::function<void(bool abandon)>;
    void run_loop();
    void drop_oldest_if_full();
    void submit(std::function<void()> command);
    void enqueue(std::function<void()> command);
    void flush_draws();
    void wake();
    void video();
    std::mutex draw_mutex_; // Guards the pending draw batch; taken before mutex_, never after it.
    uint32_t draw_window_ = 0;
    DrawBatch pending_draws_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::condition_variable space_;
    std::exception_ptr posted_error_;
    std::deque<Command> commands_;
    bool done_ = false, video_ = false, sdl_waiting_ = false;
    std::atomic<bool> stopped_{false};
    std::atomic<bool> scripted_pointer_{false};
    std::pair<std::atomic<int>, std::atomic<int>> minimum_display_{0, 0};
    static constexpr uint32_t injected_pointer = 0x51a0c0de;
    SDL_Cursor* arrow_ = nullptr;
    std::map<uint32_t, SDL_Window*> windows_;
    // SDL thread only: the client geometry the guest asked for and whether the host window is fullscreen.
    struct LogicalWindow {
        int x, y, width, height;
        bool fullscreen;
    };
    std::map<uint32_t, LogicalWindow> logical_;
    bool fullscreen_key_held_ = false;
    void toggle_fullscreen(uint32_t window);
    std::map<uint32_t, std::unique_ptr<Renderer>> renderers_;
    std::deque<HostEvent> events_;
    bool dropped_logged_ = false;
    HostInputState input_;
    std::map<uint32_t, VirtualCursor> cursors_;
    std::chrono::steady_clock::time_point last_command_pump_{};
    bool pump_due_ = false; // SDL thread only: the last command changed window visibility or stacking.
    std::function<void()> notifier_;
    std::function<bool(const HostEvent&)> input_observer_;
    void pump(const SDL_Event* initial = nullptr);
    // SDL thread: a controller event (added, removed, axis, button); updates gamepad_.
    void gamepad_event(const SDL_Event& input);
    // Hands a controller's stand-in event to the guest queue.
    void deliver_pad(HostEvent event);
    bool gamepad_enabled_ = false; // SDL thread
    int32_t gamepad_active_ = -1;  // SDL thread: instance id of the controller that last gave input
    GamepadState gamepad_;         // guarded by mutex_
    // Taps (pad_key_tap) still down: key and release time, guarded by mutex_; released on the SDL thread.
    std::vector<std::pair<int, std::chrono::steady_clock::time_point>> taps_;
    void release_taps();
    void admit(std::unique_lock<std::mutex>& lock);
};
}
