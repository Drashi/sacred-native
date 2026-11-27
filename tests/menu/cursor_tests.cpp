#include "platform/gamepad.h"
#include "platform/letterbox.h"
#include "platform/virtual_cursor.h"
#include "platform/window_host.h"
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
int failures = 0;
void expect(bool condition, const char* what, int line) {
    if (!condition) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++failures;
    }
}
#define EXPECT(condition) expect((condition), #condition, __LINE__)

void letterbox_tests() {
    // Same size: identity, unscaled.
    sacred::Letterbox box = sacred::fit_letterbox(1024, 768, 1024, 768);
    EXPECT(box.x == 0 && box.y == 0 && box.width == 1024 && box.height == 768 && !box.scaled);
    int x = 37, y = 765;
    sacred::letterbox_to_source(box, 1024, 768, x, y);
    EXPECT(x == 37 && y == 765);
    // 4:3 on 16:9: pillarbox.
    box = sacred::fit_letterbox(1024, 768, 1920, 1080);
    EXPECT(box.width == 1440 && box.height == 1080 && box.x == 240 && box.y == 0 && box.scaled);
    x = 240;
    y = 0;
    sacred::letterbox_to_source(box, 1024, 768, x, y);
    EXPECT(x == 0 && y == 0);
    x = 240 + 1439;
    y = 1079;
    sacred::letterbox_to_source(box, 1024, 768, x, y);
    EXPECT(x == 1023 && y == 767);
    x = 960;
    y = 540;
    sacred::letterbox_to_source(box, 1024, 768, x, y);
    EXPECT(x == 512 && y == 384);
    // A point on the left bar maps left of the image (negative), the caller clamps it.
    x = 10;
    y = 540;
    sacred::letterbox_to_source(box, 1024, 768, x, y);
    EXPECT(x < 0);
    // 16:9 guest on a 4:3 area: letterbox.
    box = sacred::fit_letterbox(1366, 768, 1024, 768);
    EXPECT(box.width == 1024 && box.height == 576 && box.x == 0 && box.y == 96 && box.scaled);
    // Integer 2x (HiDPI): no bars, scaled.
    box = sacred::fit_letterbox(1024, 768, 2048, 1536);
    EXPECT(box.x == 0 && box.y == 0 && box.width == 2048 && box.height == 1536 && box.scaled);
    x = 2047;
    y = 1;
    sacred::letterbox_to_source(box, 1024, 768, x, y);
    EXPECT(x == 1023 && y == 0);
    // Degenerate input leaves an empty box and coordinates untouched.
    box = sacred::fit_letterbox(0, 768, 100, 100);
    EXPECT(box.width == 0);
}

void inset_tests() {
    // 1920x1080: the 1024x768 screen fills the height, 1440x1080 in the middle.
    sacred::InsetView view = sacred::fit_inset(0, 1024, 768, 1920, 1080);
    EXPECT(view.box.x == 240 && view.box.y == 0 && view.box.width == 1440 && view.box.height == 1080 &&
           view.box.scaled);
    int x = 240, y = 0;
    sacred::inset_to_source(view, x, y);
    EXPECT(x == 0 && y == 0);
    x = 1679;
    y = 1079;
    sacred::inset_to_source(view, x, y);
    EXPECT(x == 1023 && y == 767);
    x = 960;
    y = 540;
    sacred::inset_to_source(view, x, y);
    EXPECT(x == 512 && y == 384);
    // Positions on the bars are kept inside the screen.
    x = 5;
    y = 540;
    sacred::inset_to_source(view, x, y);
    EXPECT(x == 0 && y == 384);
    x = 1919;
    y = 540;
    sacred::inset_to_source(view, x, y);
    EXPECT(x == 1023);
    // 1366x768: the same height, only bars at the sides; 1024x768: identity.
    view = sacred::fit_inset(0, 1024, 768, 1366, 768);
    EXPECT(view.box.x == 171 && view.box.y == 0 && view.box.width == 1024 && view.box.height == 768 &&
           !view.box.scaled);
    x = 171 + 37;
    y = 765;
    sacred::inset_to_source(view, x, y);
    EXPECT(x == 37 && y == 765);
    view = sacred::fit_inset(0, 1024, 768, 1024, 768);
    x = 37;
    y = 765;
    sacred::inset_to_source(view, x, y);
    EXPECT(x == 37 && y == 765);
    // 5:4 (1280x1024): narrower than the screen, it fills the width with bars above and below.
    view = sacred::fit_inset(0, 1024, 768, 1280, 1024);
    EXPECT(view.box.x == 0 && view.box.y == 32 && view.box.width == 1280 && view.box.height == 960);
    // A screen taken from the frame's centre (the save game window at 2560x1080) keeps its frame offset.
    view = sacred::fit_inset(768, 1024, 768, 2560, 1080);
    EXPECT(view.box.x == 560 && view.box.width == 1440);
    x = 560;
    y = 0;
    sacred::inset_to_source(view, x, y);
    EXPECT(x == 768 && y == 0);
    x = 2559;
    y = 0;
    sacred::inset_to_source(view, x, y);
    EXPECT(x == 768 + 1023);
    // SetCursorPos: every frame position maps back onto itself, at every size.
    const int sizes[][3] = {{0, 1920, 1080}, {0, 1366, 768},    {0, 1024, 768},  {0, 2560, 1080},
                            {0, 1280, 1024}, {768, 2560, 1080}, {0, 3840, 2160}, {448, 1920, 1080}};
    for (const auto& size : sizes) {
        view = sacred::fit_inset(size[0], 1024, 768, size[1], size[2]);
        bool round_trip = true;
        for (int gy = 0; gy < 768; gy += 7) {
            for (int gx = size[0]; gx < size[0] + 1024; gx += 3) {
                x = gx;
                y = gy;
                sacred::inset_from_source(view, x, y);
                const bool inside = x >= 0 && x < size[1] && y >= 0 && y < size[2];
                sacred::inset_to_source(view, x, y);
                round_trip = round_trip && inside && x == gx && y == gy;
            }
        }

        EXPECT(round_trip);
    }
}

void canvas_tests() {
    // The menu canvas (D of tools/lift/resolution.py: tests/lift/test_resolution.py checks the same values).
    EXPECT(sacred::menu_canvas_extension(1024, 768) == 0 && sacred::menu_canvas_extension(1280, 1024) == 0);
    EXPECT(sacred::menu_canvas_extension(1366, 768) == 171 && sacred::menu_canvas_extension(1920, 1080) == 170);
    EXPECT(sacred::menu_canvas_extension(2560, 1080) == 398 && sacred::menu_canvas_extension(3440, 1440) == 405);
    EXPECT(sacred::menu_canvas_extension(5120, 1440) == 406 && sacred::menu_canvas_extension(1366, 720) == 0);
    // A 1364-wide canvas at 1920x1080 fills the height; its middle 1024 starts 170 in.
    const sacred::InsetView view = sacred::fit_inset(0, 1024 + 2 * 170, 768, 1920, 1080);
    EXPECT(view.box.height == 1080 && view.box.width == 1918 && view.box.x == 1);
    int x = 960, y = 540;
    sacred::inset_to_source(view, x, y);
    EXPECT(x == 682 && y == 384);
}

void math_tests() {
    sacred::VirtualCursor cursor;
    cursor.set_bounds(800, 600);
    cursor.host_motion(100, 100);
    EXPECT(cursor.x() == 100 && cursor.y() == 100);
    cursor.host_motion(110, 95);
    EXPECT(cursor.x() == 110 && cursor.y() == 95);
    // SetCursorPos moves the guest cursor only; the next host motion is a delta from the warp target.
    cursor.set(300, 200);
    EXPECT(cursor.x() == 300 && cursor.y() == 200);
    cursor.host_motion(115, 100);
    EXPECT(cursor.x() == 305 && cursor.y() == 205);
    cursor.host_motion(115, 100);
    EXPECT(cursor.x() == 305 && cursor.y() == 205);
    // Clamped to the client area, never negative and never past the last pixel.
    cursor.set(-50, 9999);
    EXPECT(cursor.x() == 0 && cursor.y() == 599);
    cursor.host_motion(1115, 2100);
    EXPECT(cursor.x() == 799 && cursor.y() == 599);
    // Pushing against an edge leaves no dead zone: moving back follows immediately.
    cursor.host_motion(1114, 2099);
    EXPECT(cursor.x() == 798 && cursor.y() == 598);
    // Shrinking the client area re-clamps the position.
    cursor.set_bounds(400, 300);
    EXPECT(cursor.x() == 399 && cursor.y() == 299);

    // macOS play log: the first motion arrives in the creation geometry (1920x1200), then the window becomes
    // 1024x768. A delta across the resize would keep the guest 147 px above the host: the bottom edge unreachable.
    sacred::VirtualCursor resized;
    resized.set_bounds(1920, 1200);
    resized.host_motion(868, 914);
    resized.set_bounds(1024, 768);
    resized.host_motion(434, 627);
    EXPECT(resized.x() == 434 && resized.y() == 627);
    resized.host_motion(434, 767);
    EXPECT(resized.y() == 767);
    // An out-of-area first report is clamped but does not become the anchor.
    sacred::VirtualCursor outside;
    outside.set_bounds(1024, 768);
    outside.host_motion(868, 914);
    EXPECT(outside.x() == 868 && outside.y() == 767);
    outside.host_motion(434, 627);
    EXPECT(outside.x() == 434 && outside.y() == 627);
    // Re-setting the same bounds keeps the anchor (and with it any SetCursorPos displacement).
    outside.set(500, 500);
    outside.set_bounds(1024, 768);
    outside.host_motion(435, 628);
    EXPECT(outside.x() == 501 && outside.y() == 501);
    // A window move drops the anchor; the pump then places the guest at the live pointer while queued events are
    // still relative to the old origin, and anchors on the first event that agrees with it.
    sacred::VirtualCursor moved;
    moved.set_bounds(1024, 768);
    moved.set_origin(0, 0);
    moved.host_motion(500, 500);
    EXPECT(moved.anchored());
    moved.set_origin(448, 216);
    EXPECT(!moved.anchored());
    moved.place(420, 100);
    EXPECT(!moved.anchored() && moved.x() == 420 && moved.y() == 100);
    moved.host_motion(421, 101);
    EXPECT(moved.anchored() && moved.x() == 421 && moved.y() == 101);
    moved.host_motion(421, 0);
    EXPECT(moved.y() == 0);
    moved.set_origin(448, 216);
    EXPECT(moved.anchored());

    // Issue #19: the game's SetCursorPos displacement (+31,+23 after a special skill on an enemy) accumulates as an
    // offset towards the bottom right. In fullscreen the host pointer stops at the screen edge, so the top and left
    // edges of the game were out of reach; the host pointer reaching an edge now takes the guest cursor with it.
    sacred::VirtualCursor warped;
    warped.set_bounds(2560, 1080);
    warped.host_motion(1000, 500);
    EXPECT(warped.anchored());
    for (int i = 0; i < 4; ++i) {
        warped.set(warped.x() + 31, warped.y() + 23);
    }

    EXPECT(warped.x() == 1124 && warped.y() == 592);
    warped.host_motion(500, 100);
    EXPECT(warped.x() == 624 && warped.y() == 192);
    warped.host_motion(400, 0);
    EXPECT(warped.x() == 524 && warped.y() == 0);
    warped.host_motion(0, 0);
    EXPECT(warped.x() == 0 && warped.y() == 0);
    // Leaving the edge follows the host exactly again.
    warped.host_motion(10, 20);
    EXPECT(warped.x() == 10 && warped.y() == 20);
    // Above the window (the menu bar strip of a notched MacBook) counts as the edge too.
    warped.set(300, 300);
    warped.host_motion(10, -12);
    EXPECT(warped.x() == 300 && warped.y() == 0);
    // Resting on an edge while the game moves the guest: the next motion along the edge brings it back.
    warped.set(warped.x() + 31, warped.y() + 23);
    EXPECT(warped.y() == 23);
    warped.host_motion(15, -12);
    EXPECT(warped.x() == 336 && warped.y() == 0);
    // The far edges work the same way, and moving away from an edge keeps the delta.
    warped.set(2000, 900);
    warped.host_motion(2559, 1079);
    EXPECT(warped.x() == 2559 && warped.y() == 1079);
    warped.set(2400, 1000);
    warped.host_motion(2558, 1078);
    EXPECT(warped.x() == 2399 && warped.y() == 999);
}

void gamepad_tests() {
    // Radial deadzone: inside reads 0, the rest rescaled to 0..1 with the direction kept.
    EXPECT(sacred::dead_zone(0.1f, 0.1f, 0.24f).length == 0.0f);
    const sacred::Stick full = sacred::dead_zone(1.0f, 0.0f, 0.24f);
    EXPECT(std::fabs(full.length - 1.0f) < 1e-5f && std::fabs(full.x - 1.0f) < 1e-5f && full.y == 0.0f);
    const sacred::Stick half = sacred::dead_zone(0.0f, -0.62f, 0.24f);
    EXPECT(std::fabs(half.length - 0.5f) < 1e-5f && std::fabs(half.y + 0.5f) < 1e-5f && half.x == 0.0f);
    const sacred::Stick corner = sacred::dead_zone(1.0f, 1.0f, 0.24f);
    EXPECT(std::fabs(corner.length - 1.0f) < 1e-5f && std::fabs(corner.x - corner.y) < 1e-6f);
}

struct Mailbox {
    std::mutex mutex;
    std::condition_variable changed;
    bool ready = false;
};

sacred::HostEvent wait_for(sacred::WindowHost& host, Mailbox& box, sacred::HostEventKind kind) {
    sacred::HostEvent event{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::unique_lock<std::mutex> lock(box.mutex);
    for (;;) {
        // Clear the flag before draining: a notification that lands during the drain then stays set for the wait.
        box.ready = false;
        lock.unlock();
        while (host.take_event(event)) {
            if (event.kind == kind) {
                return event;
            }
        }

        lock.lock();
        if (!box.changed.wait_until(lock, deadline, [&] { return box.ready; })) {
            throw std::runtime_error("no host event of the expected kind");
        }
    }
}

sacred::HostEvent wait_for_move(sacred::WindowHost& host, Mailbox& box) {
    return wait_for(host, box, sacred::HostEventKind::MouseMove);
}

void host_tests(sacred::WindowHost& host) {
    Mailbox box;
    host.set_event_notifier([&] {
        {
            std::lock_guard<std::mutex> lock(box.mutex);
            box.ready = true;
        }

        box.changed.notify_all();
    });
    const uint32_t window = host.create_window("cursor", 10, 20, 640, 480, true, {}, 0, 0);
    host.inject_mouse(window, 50, 60);
    sacred::HostEvent event = wait_for_move(host, box);
    EXPECT(event.x == 50 && event.y == 60);
    const sacred::HostWindowState origin = host.window_state(window);
    // The game's SetCursorPos sequence: warp, then continued host motion.
    long followed = 0;
    int host_x = 50, host_y = 60, guest_x = 50, guest_y = 60;
    for (int round = 0; round < 3000; ++round) {
        if (round % 7 == 0) {
            const int target_x = 100 + (round * 13) % 400, target_y = 100 + (round * 29) % 300;
            host.set_cursor_pos(origin.x + target_x, origin.y + target_y);
            event = wait_for_move(host, box);
            EXPECT(event.x == target_x && event.y == target_y);
            const sacred::HostInputState state = host.input_state();
            EXPECT(state.x == target_x && state.y == target_y);
            guest_x = target_x;
            guest_y = target_y;
        }

        const int step_x = (round % 5) - 2, step_y = (round % 3) - 1;
        if (!step_x && !step_y) {
            continue;
        }

        host_x += step_x;
        host_y += step_y;
        guest_x += step_x;
        guest_y += step_y;
        host.inject_mouse(window, host_x, host_y);
        event = wait_for_move(host, box);
        EXPECT(event.x == guest_x && event.y == guest_y);
        ++followed;
    }

    EXPECT(followed > 2000);
    sacred::HostInputState state = host.input_state();
    EXPECT(state.x == guest_x && state.y == guest_y);

    // A controller's stand-in input: the cursor moves in client coordinates and host motion goes on from there.
    host.pad_pointer(200, 150);
    event = wait_for_move(host, box);
    EXPECT(event.x == 200 && event.y == 150);
    state = host.input_state();
    EXPECT(state.x == 200 && state.y == 150);
    host_x += 3;
    host.inject_mouse(window, host_x, host_y);
    event = wait_for_move(host, box);
    EXPECT(event.x == 203 && event.y == 150);
    host.pad_button(1, true);
    event = wait_for(host, box, sacred::HostEventKind::MouseDown);
    EXPECT(event.value == 1 && event.x == 203 && event.y == 150 && (event.buttons & 1));
    EXPECT(host.input_state().buttons & 1);
    host.pad_button(1, false);
    event = wait_for(host, box, sacred::HostEventKind::MouseUp);
    EXPECT(event.value == 1 && !(host.input_state().buttons & 1));
    host.pad_key(32, true);
    event = wait_for(host, box, sacred::HostEventKind::KeyDown);
    EXPECT(event.value == 32 && host.input_state().keys.test(32));
    host.pad_key(32, false);
    event = wait_for(host, box, sacred::HostEventKind::KeyUp);
    EXPECT(event.value == 32 && !host.input_state().keys.test(32));
    host.pad_wheel(-2);
    event = wait_for(host, box, sacred::HostEventKind::Wheel);
    EXPECT(event.y == -2);
    EXPECT(!host.gamepad_state().connected);
    host.destroy_window(window);
}
}

int main() {
    letterbox_tests();
    inset_tests();
    canvas_tests();
    math_tests();
    gamepad_tests();
    sacred::WindowHost host;
    std::exception_ptr failure;
    std::thread guest([&] {
        try {
            host_tests(host);
        } catch (...) {
            failure = std::current_exception();
        }

        host.finish();
    });
    host.loop();
    guest.join();
    if (failure) {
        try {
            std::rethrow_exception(failure);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s\n", error.what());
        }

        return 1;
    }

    if (failures) {
        return 1;
    }

    std::puts("CURSOR_PASS virtual_cursor=1 host_follow=1");
    return 0;
}
