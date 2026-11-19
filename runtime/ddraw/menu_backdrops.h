#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace sacred {
// Opt-in wide backgrounds for the HD mode's 1024x768 screens (Settings.cfg MENU_BACKGROUNDS : dir, or
// SACRED_MENU_BACKGROUNDS=dir, which wins). The directory holds one PNG per screen, named by its key (table below):
// the screen's picture extended to the sides, any height (768 * k), with the original 1024x768 centred horizontally.
// Nothing derived from the game is in the repository; the user's images are read at run time like TEXTURE_PACK.
// Links are followed. The notes name a link whose target is missing, a directory that cannot be opened and one with
// no usable picture, with the path as resolved (a relative one against the working directory).
//
//   main-sacred.png / main-underworld.png               main menu (LEGACY_UI 1 / 0)         alpha: holes for sky/water
//   charselect-sacred.png / charselect-underworld.png   character select
//   options-sacred.png / options-underworld.png         options
//   saveload.png                                        save game window (load and save, from the main menu or the
//                                                       game menu; the same background with either LEGACY_UI)
//   loading-<bitmap>.png                                loading screen PAK/<bitmap>.bmp, e.g. loading-loadinguw00.png
//   worldmap.png                                        world map (drawn around the screen only)
//
// Still screens (options, save game window, loading, world map): while the window is wider than 4:3, the screen's own
// background (its 4x3 grid of 256x256 tiles, recognised by the content of the texture.pak entries they come from,
// whether or not TEXTURE_PACK replaces them, or the loading bitmap) is drawn from the picture's centre, and the
// presentation draws the picture's sides around the scaled screen. The picture's centre may differ from the original
// near the 4:3 borders (a blend into the new sides): since the guest's background itself is taken from the picture, no
// seam shows, and the guest's interface and pointer are still drawn over it.
//
// The main menu and the character select are laid out on the menu canvas (platform/letterbox.h,
// menu_canvas_extension; the guest's windows, projections and water grid follow it through
// tools/lift/resolution_sites.json). Their background tiles are drawn from the picture with the outer columns
// stretched to the canvas edges, the scrolling sky is continued over the canvas, and the 3D water draws from wide
// versions of its floor and reflection textures; the live sky and water show through the picture's alpha holes. With
// a picture the whole canvas is shown, without one only its middle 1024x768, as in the original.
// A missing picture leaves a still screen black at the sides.
struct Backdrop {
    std::string key;
    uint32_t width = 0, height = 0; // pixels of the image
    std::vector<uint8_t> rgba;      // top row first
    bool alpha = false;             // some pixel is not opaque
    uint32_t surface = 0;           // renderer surface id once uploaded (DirectDraw)
    // Width in screen pixels when the image is shown 768 pixels high, and the left edge of the original screen in it.
    double logical_width() const { return height ? double(width) * 768.0 / double(height) : 0.0; }
    double centre_left() const { return (logical_width() - 1024.0) / 2.0; }
};
// A texture of the menu canvas screens (main menu, character select), recognised by its texture.pak content: one
// 256x256 background tile of a screen (column and row in its 4x3 grid, `backdrop` the picture's index, or -1 without
// one), the scrolling sky, or the floor or reflection texture of the 3D water, with the key of the screen's picture.
struct BackdropTile {
    enum Role : uint8_t { Tile, Sky, Floor, Reflection };
    Role role = Tile;
    int backdrop = -1;
    unsigned column = 0, row = 0;
    std::string screen; // picture key of the screen the texture belongs to
};
// The screens with tiled backgrounds: key and the texture.pak entry names of their tiles, row by row.
struct BackdropScreen {
    const char* key;
    std::vector<std::string> tiles;
    const char* floor = nullptr; // water floor and reflection textures (main menu, character select)
    const char* reflection = nullptr;
    const char* sky = nullptr;
};
const std::vector<BackdropScreen>& backdrop_screens();
// FNV-1a 64 of the raw pixel data of every 32-bit (format 6) entry of TEX\x03 pak files, by upper-case entry name,
// later files winning: the content hash the guest's texture of that entry gets (texture_hash, A8R8G8B8).
std::map<std::string, uint64_t> texture_pak_hashes(const std::vector<std::filesystem::path>& paks);
// FNV-1a 64 of the first 1024 bytes of a texture's rows (fewer when it has fewer): a cheap first test before the full
// hash. `prefixes` (optional) receives that of every entry, by name.
uint64_t texture_prefix_hash(const uint8_t* bytes, size_t size);
std::map<std::string, uint64_t> texture_pak_hashes(const std::vector<std::filesystem::path>& paks,
                                                   std::map<std::string, uint64_t>* prefixes);

// The menu water's floor seen on the menu canvas: the canvas point (x, y) of a texel (u, v) of the wide floor texture
// (u 0..1 across the texture twice as wide, the original in its middle half; v as in the original), x measured from
// the canvas middle, for the main menu (character false) or the character select. False when the texel is not in front
// of the camera. Measured from the live game (a floor texture coded with its own coordinates, 1836x768): per screen
// row y the floor runs at u = 0.5 + (x - centre) / (slope * (y - horizon)) and v = a + b / (y - horizon), a plane.
bool water_floor_point(bool character, double u, double v, double& x, double& y);
// Whether a picture's alpha holes show the live water of its screen: its painted water, projected back onto the
// floor texture (w x h RGBA) through water_floor_point, follows the floor's light and dark (correlation 0.3 or more)
// at about its brightness (the floor plus the light the water draw adds, within 25). The Sacred main menu's
// picture does; pictures painted dark and flat under their holes do not.
bool picture_shows_water(const Backdrop& picture, const std::vector<uint8_t>& floor, uint32_t width, uint32_t height,
                         bool character);
// The wide water texture (2w x h, RGBA, top row first) of a menu canvas screen from its original floor or
// reflection texture (w x h): the original in the middle half, its two outer columns on each side repaired (they hold
// a light seam that the 4:3 screen never showed), and the sides, each starting with a mirrored band of 24 texels at
// the old edges so the live water goes on without a seam:
// - painted (picture_shows_water): the floor carries the picture's painted water, projected back through
//   water_floor_point (less the light the water draw adds), and the reflection fades out (the painting shows it);
// - otherwise the floor is made of patches of its own part without objects (main menus: above the stair slab, the
//   letters, the coins and the dagger, which are in its lower 63 %; character select: all of it), brought to the
//   brightness of its edge row by row, and the reflection is mirrored, fading into a blur of itself.
std::vector<uint8_t> wide_water_texture(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height, bool floor,
                                        const Backdrop* picture, bool character, bool painted);

class MenuBackdrops {
public:
    // `paks`: the texture.pak files the game loads, in load order (with the profile overlay: what the game draws).
    // `originals`: the installation's own paks (no overlay), the base split pictures are put back together on; empty
    // takes `paks`. A profile's texture03.pak (tools/legacy_char_pak.py) replaces GUI_CHAR_xx with a repainted
    // backdrop, and the changes in <key>.sides.png only fit the game's own pixels. Unreadable or invalid images, and
    // split pictures that do not compose to the hash in composed.sha256, are skipped with a note.
    MenuBackdrops(const std::string& directory, const std::vector<std::filesystem::path>& paks,
                  const std::vector<std::filesystem::path>& originals = {});
    // The menu canvas textures only (no pictures).
    explicit MenuBackdrops(const std::vector<std::filesystem::path>& paks) : MenuBackdrops(std::string(), paks) {}
    size_t size() const { return backdrops_.size(); }
    Backdrop& at(size_t index) { return backdrops_[index]; }
    // Index of the image for a key, or -1.
    int find(const std::string& key) const;
    // The role of a texture content, or null.
    const BackdropTile* tile(uint64_t hash) const;
    // Whether a texture whose first bytes hash to this (texture_prefix_hash) may have a role: only those get the
    // full hash, so the game's other textures cost one short hash each.
    bool candidate(uint64_t prefix) const { return prefixes_.count(prefix) != 0; }
    // The image for a loading bitmap file (any path; matched by its file stem, case-insensitively), or -1.
    int loading(const std::string& bitmap_path) const;
    // Copies the image's centre 1024x768 screen into a 24-bit bottom-up bitmap of width x height (BGR, `pitch`
    // bytes per row), box-filtered when the image is larger. False when the sizes do not fit.
    bool centre_bitmap(size_t index, uint32_t width, uint32_t height, uint32_t pitch, std::vector<uint8_t>& bgr) const;
    // Whether the guest frame is wider than 4:3 (the sides are shown); set from the HD size.
    bool wide = false;
    // Wide versions of the water textures of a screen (DirectDraw makes them on first use): renderer surface ids.
    std::map<std::string, uint32_t> wide_water;
    // Per screen key: whether the wide water's sides carry the picture's painted water (picture_shows_water).
    std::map<std::string, bool> painted_water;
    // The image the current frame drew its background from (index + 1, 0 for none): set while drawing, taken at
    // presentation.
    size_t frame = 0;
    // What the current frame's translucent untextured quads over the whole target did to what lay under them, so the
    // presentation does the same to the picture's sides: colour added (rgb) and scale (a) of the colour beneath. The
    // save game screen shades everything under its confirmation windows that way. Reset at presentation.
    std::array<float, 4> shade{0, 0, 0, 1};
    std::vector<std::string> notes;

private:
    std::vector<Backdrop> backdrops_;
    std::map<uint64_t, BackdropTile> tiles_;
    std::set<uint64_t> prefixes_;
};
}
