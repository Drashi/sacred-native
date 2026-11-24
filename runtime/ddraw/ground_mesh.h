#pragma once
#include "../../platform/renderer.h"
#include <cstdint>
#include <memory>
#include <vector>

struct X86;
namespace sacred {
class DirectDraw;
class Control;
// The ground kept in vertex buffers on the renderer (on by default, SACRED_GROUND_MESH=0 turns it off).
//
// Every frame cWorldView0::render (0x632140) walks the visible tile rows: renderTileRow (0x62ae90) builds a
// pretransformed quad per tile and adds it to the view's quad batcher (0x629180), which draws a few quads per
// texture change; drawTileLayers (0x62d3c0) then looks up every collected tile's blend layer records and draws them
// in passes the same way. In the zoomed-out HD view that is thousands of draw calls and about a quarter of the
// game thread. A tile's quads depend only on the tile (texture cell, corner heights and light, layer records) and
// on the camera, which moves and scales the whole ground at once. So each loaded sector (64x64 tiles) keeps its
// tiles' quads in its own isometric space, built once as tiles come into view, grouped by pass and textures, and
// the ground is drawn from those buffers with the camera's offset and zoom applied in the vertex shader.
//
// The game's own ground draws are skipped (the quad batcher's add and setTexture inside the row walk, the layer
// pass); the walk itself still runs (it collects objects and water tiles). Changed tiles are noticed when the walk
// visits them and their sector is built again.
//
// At night and under ground the game lights the ground: renderTileRow passes each quad through cTileRenderer
// (0x61ed70), which colours its corners with the world state's light lookup (0x41b510: the light sources within
// reach of the corner add up to an index into a colour ramp, scaled by the tile's own light). The cached quads keep
// the tile's light and their place; the vertex shader does that lookup from the frame's lights (GroundLight in
// platform/renderer.h), so the cache stays on.
class GroundMesh {
public:
    explicit GroundMesh(DirectDraw& owner);
    ~GroundMesh();
    // Stand-ins (runtime/sacred/native.c), each true when it handled the call.
    bool tile_row(X86* c);
    bool tile_layers(X86* c);
    bool skip_quad(X86* c, unsigned argument_bytes);
    // renderTileRow's quad build for a tile of this type (tools/lift/generate.py NATIVE_JUMPS): water and lava keep
    // it, their quads go to the water tile list.
    bool skip_build(uint32_t type) const {
        return in_row_ && active_ && (type & 0xf0) != 0x90 && (type & 0xf0) != 0xa0;
    }

private:
    struct Group;
    struct SectorCache;
    struct TileKey {
        int32_t def = 0;
        uint32_t layers = 0, heights = 0, light = 0;
        bool operator==(const TileKey& o) const {
            return def == o.def && layers == o.layers && heights == o.heights && light == o.light;
        }
    };
    struct SectorId {
        uint32_t sector = 0, tiles = 0;
        int32_t x = 0, y = 0;
        bool operator==(const SectorId& o) const {
            return sector == o.sector && tiles == o.tiles && x == o.x && y == o.y;
        }
    };
    enum class Build { Ok, Unsupported };
    DirectDraw& dd_;
    bool enabled_ = true;
    std::vector<std::unique_ptr<SectorCache>> caches_;
    SectorCache* slots_[9] = {};
    // Quads are kept in the space of the first sector cached since the last reset (the anchor): sectors whose
    // camera transform then agrees are drawn together, one draw call per pass, place and textures.
    bool anchored_ = false;
    int32_t anchor_x_ = 0, anchor_y_ = 0;
    uint32_t next_mesh_ = 1;
    uint32_t window_ = 0; // renderer window of the meshes
    // What every cached quad depends on besides its tile.
    uint32_t view_ = 0, map_data_ = 0, defs_ = 0;
    float uvs_[18 * 8] = {};
    // This frame.
    uint64_t frame_ = 0, present_ = ~uint64_t(0);
    bool active_ = false, ground_rows_ = false, have_offset_ = false, drawn_ = false, in_row_ = false;
    float offset_x_ = 0, offset_y_ = 0;
    // The device state at the first quad the walk added this frame: the state the game's base tile flushes draw in
    // (cTileRenderer passes between the rows change it before drawTileLayers).
    RenderState base_state_;
    bool base_valid_ = false, base_reported_ = false;
    // The frame's lights when the game lights the ground (light_.on).
    GroundLight light_;
    bool lit_enabled_ = true;
    void read_light(Control& control, uint32_t world_state, bool second_ramp);
    // Statistics (SACRED_GROUND_MESH_STATS=1), printed every 300 frames.
    bool stats_ = false;
    uint64_t stat_frames_ = 0, stat_draws_ = 0, stat_ranges_ = 0, stat_tiles_ = 0, stat_resets_ = 0;
    std::vector<uint32_t> retired_; // meshes to destroy with the next renderer command
    void begin_frame(Control& control, uint32_t view);
    void reset_all();
    void release(SectorCache& cache);
    void disable(const char* why);
    bool sector_id(Control& control, int slot, SectorId& id);
    SectorCache* find_cache(const SectorId& id);
    SectorCache* cache_for(const SectorId& id);
    void visit_row(Control& control, uint32_t row);
    bool visit_tile(Control& control, int slot, int tile);
    Build build_tile(Control& control, SectorCache& cache, int t, const TileKey& key, const int8_t heights[4],
                     const uint8_t light[4]);
    void draw_ground(Control& control, uint32_t device);
    uint32_t texture_surface(Control& control, uint32_t handle);
    void render_flag(Control& control, uint32_t device, bool on);
};
// The instance the stand-ins reach (null: off).
void ground_mesh_register(GroundMesh* ground);
}
