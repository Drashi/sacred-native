#pragma once
#include <algorithm>
#include <cstdint>

namespace sacred {
// Placement of a guest image of (source_width x source_height) inside a host area of (area_width x area_height):
// the largest rectangle with the source aspect ratio, centred, with black bars on the remaining sides
// (pillarbox for a wider area, letterbox for a taller one). An area with the source size maps 1:1.
struct Letterbox {
    int x = 0, y = 0, width = 0, height = 0;
    bool scaled = false; // the destination differs from the source size: present with filtering
};
inline Letterbox fit_letterbox(int source_width, int source_height, int area_width, int area_height) {
    Letterbox box;
    if (source_width <= 0 || source_height <= 0 || area_width <= 0 || area_height <= 0) {
        return box;
    }

    // Compare area_width/source_width with area_height/source_height without rounding.
    if (int64_t(area_width) * source_height <= int64_t(area_height) * source_width) {
        box.width = area_width;
        box.height = int((int64_t(area_width) * source_height + source_width / 2) / source_width);
    } else {
        box.height = area_height;
        box.width = int((int64_t(area_height) * source_width + source_height / 2) / source_height);
    }

    box.width = std::clamp(box.width, 1, area_width);
    box.height = std::clamp(box.height, 1, area_height);
    box.x = (area_width - box.width) / 2;
    box.y = (area_height - box.height) / 2;
    box.scaled = box.width != source_width || box.height != source_height;
    return box;
}

// Host point (in area coordinates) to guest coordinates; points on the bars map past the image edge and are left to
// the caller's clamp. Identity when the area has the source size.
inline void letterbox_to_source(const Letterbox& box, int source_width, int source_height, int& x, int& y) {
    if (box.width <= 0 || box.height <= 0) {
        return;
    }

    x = int((int64_t(x - box.x) * source_width + (x >= box.x ? 0 : -(box.width - 1))) / box.width);
    y = int((int64_t(y - box.y) * source_height + (y >= box.y ? 0 : -(box.height - 1))) / box.height);
}

// HD mode screens designed for a fixed size (the 1024x768 menus, loading screens, the world map): the guest draws
// them into a source_width x source_height rectangle of its frame at (source_x, 0), and the host shows that rectangle
// scaled to fill the client area's height (or its width, for a client area narrower than the screen's aspect), with
// its aspect kept and centred. `box` is the destination in client coordinates.
struct InsetView {
    Letterbox box;
    int source_x = 0, source_width = 1024, source_height = 768;
};
inline InsetView fit_inset(int source_x, int source_width, int source_height, int area_width, int area_height) {
    InsetView view;
    view.box = fit_letterbox(source_width, source_height, area_width, area_height);
    view.source_x = source_x;
    view.source_width = source_width;
    view.source_height = source_height;
    return view;
}

// Client position to the guest frame position of the pixel shown there, clamped to the rectangle the inset shows.
inline void inset_to_source(const InsetView& view, int& x, int& y) {
    if (view.box.width <= 0 || view.box.height <= 0) {
        return;
    }

    letterbox_to_source(view.box, view.source_width, view.source_height, x, y);
    x = view.source_x + std::clamp(x, 0, view.source_width - 1);
    y = std::clamp(y, 0, view.source_height - 1);
}

// Guest frame position to a client position that maps back to it (SetCursorPos): the first client pixel of the
// span the frame pixel covers, so inset_to_source(inset_from_source(p)) == p for every scale.
inline void inset_from_source(const InsetView& view, int& x, int& y) {
    if (view.box.width <= 0 || view.box.height <= 0) {
        return;
    }

    auto up = [](int64_t value, int64_t from, int64_t to) { // ceil(value * to / from) for any sign of value
        const int64_t product = value * to;
        return int(product >= 0 ? (product + from - 1) / from : -((-product) / from));
    };
    x = view.box.x + up(int64_t(x) - view.source_x, view.source_width, view.box.width);
    y = view.box.y + up(y, view.source_height, view.box.height);
}

// The menu canvas: in a frame wider than 4:3 and at least 768 high, the main menu and the character select are laid
// out on a canvas 768 * width / height wide (at most 1024 + 2 * 406, the width of the wide pictures), their original
// 1024x768 in its middle. The result is how far in that middle starts (D of tools/lift/resolution.py, which moves the
// guest's windows and projections by the same formula), 0 when there is no canvas.
constexpr int menu_extension_max = 406;
inline int menu_canvas_extension(int width, int height) {
    if (height < 768 || int64_t(width) * 768 <= int64_t(height) * 1024) {
        return 0;
    }

    return int(std::min<int64_t>((768 * int64_t(width) / height - 1024) / 2, menu_extension_max));
}
}
