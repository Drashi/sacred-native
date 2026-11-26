#include "platform/app_icon.h"
#include <SDL.h>
#include <cstdlib>
#include <fstream>
#include <iterator>

// PNG only, with internal linkage: runtime/ddraw/texture_pack.cpp holds the program's full copy of stb_image.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "third_party/stb/stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace sacred {
void set_app_identity() {
    SDL_SetHint(SDL_HINT_APP_NAME, "Sacred Native");
#if !defined(__APPLE__) && !defined(_WIN32)
    setenv("SDL_VIDEO_X11_WMCLASS", app_id, 0);
    setenv("SDL_VIDEO_WAYLAND_WMCLASS", app_id, 0);
#endif
}

bool load_png_icon(const std::filesystem::path& file, IconImage& icon) {
    std::ifstream in(file, std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    int width = 0, height = 0, channels = 0;
    if (bytes.empty() || bytes.size() > (64u << 20) ||
        !stbi_info_from_memory(bytes.data(), int(bytes.size()), &width, &height, &channels) || width <= 0 ||
        height <= 0 || width > 1024 || height > 1024) {
        return false;
    }

    stbi_uc* pixels = stbi_load_from_memory(bytes.data(), int(bytes.size()), &width, &height, &channels, 4);
    if (!pixels) {
        return false;
    }

    icon.width = width;
    icon.height = height;
    icon.rgba.assign(pixels, pixels + size_t(width) * size_t(height) * 4);
    stbi_image_free(pixels);
    return true;
}

bool load_app_icon(IconImage& icon) {
    char* base = SDL_GetBasePath();
    if (!base) {
        return false;
    }

    const std::filesystem::path dir = base;
    SDL_free(base);
#ifdef __APPLE__
    return load_png_icon(dir / "icons/sacred-native-512.png", icon);
#else
    return load_png_icon(dir / "icons/sacred-native-256.png", icon);
#endif
}

void set_window_icon(SDL_Window* window, const IconImage& icon) {
    if (!window || icon.rgba.empty()) {
        return;
    }

    if (SDL_Surface* surface =
            SDL_CreateRGBSurfaceWithFormatFrom(const_cast<uint8_t*>(icon.rgba.data()), icon.width, icon.height, 32,
                                               icon.width * 4, SDL_PIXELFORMAT_RGBA32)) {
        SDL_SetWindowIcon(window, surface);
        SDL_FreeSurface(surface);
    }
}
}
