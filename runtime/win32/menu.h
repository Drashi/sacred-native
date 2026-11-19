#pragma once
#include <algorithm>
#include "../process/control.h"
#include <array>
#include <deque>
#include <map>
#include <memory>
#include "../../platform/font.h"
#include "../../platform/letterbox.h"

namespace sacred {
class WindowHost;
class Startup;
class Font;
class FontResource;
class MenuBackdrops;
// Post-WinMain boundary. Only dynamically reached operations are bound.
class Menu {
public:
    Menu(Control& control, const Loader& loader, std::string profile = {}, WindowHost* host = nullptr,
         Startup* files = nullptr);
    ~Menu();
    void bind();
    uint32_t native_window(uint32_t window = 0) const;
    uint32_t surface_dc(uint32_t data, uint32_t width, uint32_t height, uint32_t pitch);
    void release_surface_dc(uint32_t dc);
    // Opt-in screen mode: the last frame showed a rectangle of the frame (a 1024x768 screen) as an inset, scaled into
    // a box of the client area (`view`, platform/letterbox.h; on false: no inset). Pointer positions given to the
    // guest are taken through that box and kept inside the rectangle it shows; SetCursorPos maps back.
    // Wide screen images (runtime/ddraw/menu_backdrops.h): a loading bitmap with an opaque image loads its centre.
    void menu_backdrops(std::shared_ptr<MenuBackdrops> backdrops) { backdrops_ = std::move(backdrops); }
    void set_pointer_inset(bool on, const InsetView& view) {
        inset_on_ = on;
        inset_ = view;
    }

private:
    bool inset_on_ = false;
    InsetView inset_;
    std::shared_ptr<MenuBackdrops> backdrops_;
    std::map<uint32_t, size_t> backdrop_bitmaps_; // bitmap handle -> wide image it was loaded from (index)
    void to_inset(int32_t& x, int32_t& y) const {
        if (!inset_on_) {
            return;
        }

        int px = x, py = y;
        inset_to_source(inset_, px, py);
        x = px;
        y = py;
    }

    void from_inset(int32_t& x, int32_t& y) const {
        if (!inset_on_) {
            return;
        }

        int px = x, py = y;
        inset_from_source(inset_, px, py);
        x = px;
        y = py;
    }

    Control& control_;
    const Loader& loader_;
    std::string profile_;
    uint32_t foreground_timeout_ = 0;
    WindowHost* host_;
    Startup* files_;
    struct Bitmap {
        uint32_t width, height, pitch, bits, data;
        bool top_down = false;
    };
    std::map<uint32_t, Bitmap> bitmaps_;
    uint32_t next_bitmap_ = 0xe8000001;
    struct FontResourceEntry {
        uint32_t references;
        std::unique_ptr<FontResource> native;
    };
    std::map<std::string, FontResourceEntry> font_resources_;
    struct FontEntry {
        std::array<uint8_t, 60> logical;
        std::unique_ptr<Font> native;
    };
    std::map<uint32_t, FontEntry> fonts_;
    uint32_t next_font_ = 0xe9000001;
    struct DeviceContext {
        Bitmap target;
        uint32_t font = 0xe9000000, bitmap = 0xe8000000, stock_data = 0;
        bool compatible = false;
        uint32_t background_mode = 2, text_color = 0, window = 0, background_color = 0xffffff;
    };
    std::map<uint32_t, DeviceContext> contexts_;
    std::map<uint32_t, uint32_t> bitmap_contexts_;
    uint32_t next_dc_ = 0xea000001;
    void bind_gdi();
    std::u16string guest_text(Control& c, uint32_t pointer, int32_t count, bool wide) const;
    DeviceContext& text_context(uint32_t handle);
    void paint_run(Control& c, DeviceContext& dc, const TextRun& run, int32_t x, int32_t y);
    Result draw_text(Control& c, uint32_t handle, const std::u16string& text, uint32_t rect, uint32_t format);
    std::string ansi(uint32_t pointer) const;
    uint16_t word(uint32_t address) const;
    std::pair<uint32_t, uint32_t> version(const std::string& path) const;
    struct VersionBuffer {
        uint32_t size, next;
        std::map<std::string, uint32_t> strings;
    };
    std::map<uint32_t, VersionBuffer> version_buffers_;
    uint32_t resource(uint32_t module, uint32_t name, uint32_t type) const;
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> resources_;
    struct Icon {
        uint32_t width, height;
        std::vector<uint8_t> rgba;
    };
    std::map<uint32_t, Icon> icons_;
    std::map<std::pair<uint32_t, std::string>, uint32_t> shared_icons_;
    uint32_t load_icon(uint32_t instance, uint32_t name);
    std::map<uint32_t, uint32_t> brushes_{{0xe4000004, 0xff000000}};
    struct WindowClass {
        uint32_t atom, style, procedure, instance, icon, cursor, brush, small_icon;
        uint32_t window_extra, menu;
        std::vector<uint8_t> extra;
    };
    std::map<std::string, WindowClass> classes_;
    struct Window {
        uint32_t native, procedure, style, extended, instance, parent, menu, userdata = 0;
        uint32_t owner_thread, brush;
        int32_t x, y, width, height;
        std::vector<uint8_t> extra;
        bool invalid = true;
        bool erase = true;
    };
    std::map<uint32_t, Window> windows_;
    // The window the guest last made foreground while the host compositor kept another application active (see
    // SetForegroundWindow); 0 once a window of ours really had the focus.
    uint32_t foreground_claim_ = 0;
    Result window_message(uint32_t window, uint32_t message, uint32_t wparam, uint32_t lparam);
    struct Message {
        uint32_t window, message, wparam, lparam, time, x, y;
    };
    std::deque<Message> messages_;
    bool quit_posted_ = false;
    uint32_t quit_code_ = 0;
    void bind_input();
    bool destroy_window(uint32_t handle);
    void translate_events();
    bool fetch_message(uint32_t window, uint32_t low, uint32_t high, bool remove, Message& out);
};
}
