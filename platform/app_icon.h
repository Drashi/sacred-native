// The project's own logo as the icon of the launcher's and the game's windows (on macOS SDL also shows it in the
// Dock), and the name the desktop groups those windows under. The logo is a PNG of the bundle
// (icons/sacred-native-<size>.png in Contents/Resources of the macOS app, next to the executables in the Linux
// bundle); without it the windows keep their default icon.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

struct SDL_Window;

namespace sacred {
struct IconImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba; // width * height * 4 bytes, top row first, straight (not premultiplied) alpha
};

// The desktop file's name and StartupWMClass (tools/dist/linux/sacred-native.desktop), the X11 WM_CLASS and the
// Wayland app_id of every window.
inline constexpr const char* app_id = "sacred-native";

// Before SDL_Init: the application name SDL hands to the system (audio streams, screen saver inhibition) and, on
// X11 and Wayland, the window class (app_id above) unless SDL_VIDEO_X11_WMCLASS / SDL_VIDEO_WAYLAND_WMCLASS already
// name one.
void set_app_identity();

// A PNG file as RGBA, at most 1024 pixels on a side. False, with `icon` left as it was, when the file cannot be read
// or is no such PNG.
bool load_png_icon(const std::filesystem::path& file, IconImage& icon);

// The logo from the bundle's resources (SDL_GetBasePath: Contents/Resources in the macOS app, the executable's
// directory otherwise): 512 pixels on macOS (the Dock), 256 elsewhere. False when there is none.
bool load_app_icon(IconImage& icon);

// The window's icon; nothing happens for an empty image.
void set_window_icon(SDL_Window* window, const IconImage& icon);
}
