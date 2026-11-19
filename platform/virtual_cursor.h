#pragma once
#include <algorithm>

namespace sacred {
// Guest-visible pointer for one window, in client coordinates. SetCursorPos moves only this value (the host pointer is
// never warped: Wayland/Xwayland and macOS do not honour warps reliably, and the game draws its own cursor);
// host motion is applied as the delta from the previous host position and clamped to the client area.
class VirtualCursor {
public:
    // Client area; the guest cursor stays inside [0, width-1] x [0, height-1]. Re-clamps the current position.
    // A changed client area drops the anchor: host positions reported before the change are relative to the old
    // geometry (macOS delivers the first motion after SDL_SetWindowSize still in the old frame), so a delta across
    // the change would leave a permanent guest/host offset and make an edge of the window unreachable.
    void set_bounds(int width, int height) {
        width = std::max(width, 1);
        height = std::max(height, 1);
        if (width != width_ || height != height_) {
            anchored_ = false;
        }

        width_ = width;
        height_ = height;
        x_ = clamp(x_, width_);
        y_ = clamp(y_, height_);
    }

    // Screen position of the client area. A move drops the anchor like a resize: the host reports positions relative
    // to the client origin, and events queued before the move are still relative to the old one.
    void set_origin(int x, int y) {
        if (x != origin_x_ || y != origin_y_) {
            anchored_ = false;
        }

        origin_x_ = x;
        origin_y_ = y;
    }

    bool anchored() const { return anchored_; }
    // Unanchored only: the host's live pointer position disagrees with a (stale) event, so the guest follows the live
    // position and the anchor waits for an event that agrees with it.
    void place(int x, int y) {
        x_ = clamp(x, width_);
        y_ = clamp(y, height_);
    }

    // Host pointer reported at (host_x, host_y). An unanchored cursor takes the host position; only a report inside
    // the client area becomes the anchor, so an out-of-area first report cannot fix an offset through the clamp.
    void host_motion(int host_x, int host_y) {
        if (anchored_) {
            x_ = clamp(x_ + host_x - host_x_, width_);
            y_ = clamp(y_ + host_y - host_y_, height_);
        } else {
            x_ = clamp(host_x, width_);
            y_ = clamp(host_y, height_);
            anchored_ = x_ == host_x && y_ == host_y;
        }

        host_x_ = host_x;
        host_y_ = host_y;
    }

    // SetCursorPos: moves the guest cursor only.
    void set(int x, int y) {
        x_ = clamp(x, width_);
        y_ = clamp(y, height_);
    }

    int x() const { return x_; }
    int y() const { return y_; }

private:
    static int clamp(int value, int extent) { return std::min(std::max(value, 0), extent - 1); }
    int width_ = 1 << 15, height_ = 1 << 15, x_ = 0, y_ = 0, host_x_ = 0, host_y_ = 0;
    int origin_x_ = 0, origin_y_ = 0;
    bool anchored_ = false;
};
}
