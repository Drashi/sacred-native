#include "ground_mesh.h"
#include "ddraw.h"
#include "../../platform/window_host.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <tuple>
#include <unordered_map>

namespace sacred {
namespace {
// Guest addresses (Sacred.exe, GOG English build) and structure offsets.
constexpr uint32_t tile_row_entry = 0x0062ae90;   // cWorldView::renderTileRow(device, RowPos*, detail), ret 0xc
constexpr uint32_t texture_get = 0x0065ed90;      // cTextureMgr2::get(handle, flags) -> cTexture*, ret 8
constexpr uint32_t layer_record_get = 0x00635fe0; // cMapData::layerRecord(id) -> LayerRecord*, ret 4
constexpr uint32_t texture_manager = 0x013e7838;  // cTextureMgr2*
constexpr uint32_t world_state = 0x00aa4544;      // WorldState*
constexpr uint32_t render_flags = 0x00cd7a7c;     // RenderFlags* {device, flags}
constexpr uint32_t one_texture_layers = 0x2000;   // render flag: blend layers with one texture
constexpr uint32_t tile_renderer = 0x00ad7740;    // cTileRenderer (static instance)
// WorldState: the lit ground's flags, the frame's light sources ({x, y, radius} in the row walk's unscaled screen
// space, copied from the list the last frame collected) and the colour ramps (256 dwords per channel, each already
// shifted to its place in a D3DCOLOR).
constexpr uint32_t state_flags = 0x10, state_lit = 0x9e0c, state_lights = 0x9e10, state_light_count = 0xce10,
                   state_ramp = 0xfe18, state_ramp_lit = 0x10a18, state_ramp_lift = 0x12e54;
constexpr uint32_t max_lights = 1024;
constexpr uint32_t view_light_corners = 0xab0; // four {x, y} ints: the corners' places around a tile for the lights
// cTileRenderer: the pass the row walk set (1, 2: which row buffer) and the rows it has seen (0, 1, 2 = more).
constexpr uint32_t renderer_pass = 0x34, renderer_rows = 0x38;
// cWorldView
constexpr uint32_t view_map_data = 0x4, view_tile_uvs = 0x830, view_zoom = 0xb60, view_layered_count = 0x3ff1c,
                   view_passes = 0x97088, view_row_length = 0x96ac8, view_sectors = 0x970b0;
// Sector, Tile, TileDef, LayerRecord, cTexture
constexpr uint32_t sector_origin_x = 0x44, sector_origin_y = 0x48, sector_tiles = 0x6c;
constexpr uint32_t tile_size = 0x20, tile_layers_at = 0xc, tile_heights = 0x10, tile_light = 0x14;
constexpr uint32_t def_size = 0x40, def_texture = 0x20, def_uv_cell = 0x24;
constexpr uint32_t record_defs = 0x4, record_next = 0xc;
constexpr uint32_t texture_surface_field = 0x14;

constexpr int sector_side = 64, tiles_per_sector = sector_side * sector_side, loaded_side = 192;
constexpr uint32_t uv_cells = 18;
constexpr float ground_z = 0.9f;
// Corners in the game's vertex order: left, top, bottom, right; offsets around the tile position and which of the
// tile's height/light bytes (left, top, right, bottom) they take.
constexpr float corner_x[4] = {-48.2f, 0.0f, 0.0f, 48.2f};
constexpr float corner_y[4] = {0.0f, -24.2f, 24.2f, 0.0f};
constexpr int corner_byte[4] = {0, 1, 3, 2};
constexpr size_t max_sector_caches = 16; // the 9 loaded sectors and some the camera left recently

float bits_float(uint32_t value) {
    float f;
    std::memcpy(&f, &value, 4);
    return f;
}
}

struct GroundMesh::Group {
    // Draw order: pass, then the record's place among a tile's records of that pass (quads of one tile overlap,
    // quads of different tiles do not, so groups of one pass and place may draw in any order).
    uint16_t pass = 0, place = 0;
    uint32_t texture0 = 0, texture1 = 0; // texture manager handles, 0 for none
    std::vector<RenderVertex> vertices;
    uint32_t mesh = 0;
    uint32_t uploaded = 0; // quads in the mesh
    uint8_t clamp[2] = {3, 3};
};
struct GroundMesh::SectorCache {
    SectorId id;
    uint64_t frame = 0;
    std::vector<Group> groups;
    std::vector<TileKey> keys = std::vector<TileKey>(tiles_per_sector);
    std::vector<uint8_t> built = std::vector<uint8_t>(tiles_per_sector);
    float min_x = 0, min_y = 0, max_x = 0, max_y = 0; // bounds in the sector's own space
    float shift_x = 0, shift_y = 0;                   // the sector's place in the anchor's space (added to its quads)
    bool any = false, stale = false;
};

GroundMesh::GroundMesh(DirectDraw& owner) : dd_(owner) {
    const char* stats = std::getenv("SACRED_GROUND_MESH_STATS");
    stats_ = stats && *stats == '1';
    // SACRED_GROUND_MESH_LIT=0: the lit ground (night, caves) stays with the game's own draws.
    const char* lit = std::getenv("SACRED_GROUND_MESH_LIT");
    lit_enabled_ = !lit || *lit != '0';
}

GroundMesh::~GroundMesh() = default;

void GroundMesh::release(SectorCache& cache) {
    for (Group& group : cache.groups) {
        if (group.mesh) {
            retired_.push_back(group.mesh);
        }
    }

    cache.groups.clear();
    std::fill(cache.built.begin(), cache.built.end(), uint8_t(0));
    cache.any = false;
}

void GroundMesh::reset_all() {
    for (auto& cache : caches_) {
        release(*cache);
    }

    caches_.clear();
    anchored_ = false;
    std::fill(std::begin(slots_), std::end(slots_), nullptr);
}

void GroundMesh::disable(const char* why) {
    if (!enabled_) {
        return;
    }

    std::fprintf(stderr, "GROUND_MESH off: %s\n", why);
    enabled_ = false;
    active_ = false;
    reset_all();
}

void GroundMesh::begin_frame(Control& control, uint32_t view) {
    auto& memory = control.memory;
    present_ = dd_.presents_;
    ++frame_;
    active_ = drawn_ = ground_rows_ = have_offset_ = base_valid_ = false;
    std::fill(std::begin(slots_), std::end(slots_), nullptr);
    for (auto& cache : caches_) {
        if (cache->stale) {
            cache->stale = false;
            release(*cache);
            ++stat_resets_;
        }
    }

    const uint32_t map_data = memory.load32(view + view_map_data);
    const uint32_t defs = map_data ? memory.load32(map_data + 0x258) : 0;
    if (!defs) {
        return;
    }

    float uvs[uv_cells * 8];
    memory.read(view + view_tile_uvs, uvs, sizeof uvs);
    if (view != view_ || map_data != map_data_ || defs != defs_ || std::memcmp(uvs, uvs_, sizeof uvs)) {
        reset_all();
        view_ = view;
        map_data_ = map_data;
        defs_ = defs;
        std::memcpy(uvs_, uvs, sizeof uvs);
    }

    // The game lights the ground through cTileRenderer in these modes (renderTileRow's own test).
    light_.on = false;
    if (const uint32_t ws = memory.load32(world_state)) {
        uint8_t flags = 0, lit = 0;
        memory.read(ws + state_flags, &flags, 1);
        memory.read(ws + state_lit, &lit, 1);
        if ((flags & 0x20) || lit) {
            if (!lit_enabled_) {
                return;
            }

            read_light(control, ws, lit != 0);
        }
    }

    active_ = true;
    if (stats_ && ++stat_frames_ % 300 == 0) {
        size_t quads = 0, groups = 0;
        for (auto& cache : caches_) {
            groups += cache->groups.size();
            for (const Group& group : cache->groups) {
                quads += group.vertices.size() / 4;
            }
        }

        std::fprintf(
            stderr,
            "GROUND_MESH %zu sectors, %zu groups, %zu quads; per frame %.1f draws of %.1f groups, %.1f tiles built; "
            "%llu sector restarts; %s\n",
            caches_.size(), groups, quads, stat_draws_ / 300.0, stat_ranges_ / 300.0, stat_tiles_ / 300.0,
            (unsigned long long)stat_resets_,
            light_.on ? ("lit by " + std::to_string(light_.lights.size() / 3) + " lights").c_str() : "not lit");
        stat_draws_ = stat_ranges_ = stat_tiles_ = stat_resets_ = 0;
    }
}

// What the light lookup (0x41b510) reads, for the vertex shader. The lookup tests the lit flag first and takes that
// mode's ramp; the other mode's ramp may be lifted towards white (state_ramp_lift below 255).
void GroundMesh::read_light(Control& control, uint32_t ws, bool second_ramp) {
    auto& memory = control.memory;
    const uint32_t count = std::min(memory.load32(ws + state_light_count), max_lights);
    light_.lights.resize(size_t(count) * 3);
    if (count) {
        memory.read(ws + state_lights, light_.lights.data(), size_t(count) * 12);
    }

    uint32_t table[3 * 256];
    memory.read(ws + (second_ramp ? state_ramp_lit : state_ramp), table, sizeof table);
    const uint32_t lift = second_ramp ? 0 : 255 - std::min<uint32_t>(memory.load32(ws + state_ramp_lift), 255);
    light_.ramp.resize(256 * 3);
    for (int i = 0; i < 256; ++i) {
        for (int c = 0; c < 3; ++c) {
            const uint32_t value = (table[c * 256 + i] >> (16 - 8 * c)) & 0xff;
            light_.ramp[size_t(i) * 3 + c] = uint8_t(std::min<uint32_t>(value + lift, 255));
        }
    }

    memory.read(view_ + view_light_corners, light_.corners, sizeof light_.corners);
    light_.on = true;
}

bool GroundMesh::sector_id(Control& control, int slot, SectorId& id) {
    auto& memory = control.memory;
    id.sector = memory.load32(view_ + view_sectors + uint32_t(slot) * 4);
    id.tiles = id.sector ? memory.load32(id.sector + sector_tiles) : 0;
    if (!id.tiles) {
        return false;
    }

    id.x = int32_t(memory.load32(id.sector + sector_origin_x));
    id.y = int32_t(memory.load32(id.sector + sector_origin_y));
    return true;
}

GroundMesh::SectorCache* GroundMesh::find_cache(const SectorId& id) {
    for (auto& cache : caches_) {
        if (cache->id == id) {
            return cache.get();
        }
    }

    return nullptr;
}

GroundMesh::SectorCache* GroundMesh::cache_for(const SectorId& id) {
    if (SectorCache* cache = find_cache(id)) {
        return cache;
    }

    if (caches_.size() >= max_sector_caches) {
        // The least recently used one that is not in use this frame.
        auto oldest = caches_.end();
        for (auto it = caches_.begin(); it != caches_.end(); ++it) {
            if ((*it)->frame != frame_ && (oldest == caches_.end() || (*it)->frame < (*oldest)->frame)) {
                oldest = it;
            }
        }

        if (oldest == caches_.end()) {
            return nullptr;
        }

        release(**oldest);
        caches_.erase(oldest);
    }

    caches_.push_back(std::make_unique<SectorCache>());
    SectorCache& cache = *caches_.back();
    cache.id = id;
    // Sector origins are tile column (x) and row (y); a tile is 48 x 24 isometric units each way.
    if (!anchored_) {
        anchored_ = true;
        anchor_x_ = id.x;
        anchor_y_ = id.y;
    }

    cache.shift_x = 48.0f * float((id.x - anchor_x_) - (id.y - anchor_y_));
    cache.shift_y = 24.0f * float((id.x - anchor_x_) + (id.y - anchor_y_));
    return &cache;
}

GroundMesh::Build GroundMesh::build_tile(Control& control, SectorCache& cache, int t, const TileKey& key,
                                         const int8_t heights[4], const uint8_t light[4]) {
    auto& memory = control.memory;
    const int row = t / sector_side, column = t % sector_side;
    const float px = 48.0f * float(column - row), py = 24.0f * float(column + row);
    float x[4], y[4], gray[4];
    for (int i = 0; i < 4; ++i) {
        const int b = corner_byte[i];
        x[i] = px + corner_x[i];
        y[i] = py + corner_y[i] - float(heights[b]);
        gray[i] = float(light[b]) / 255.0f;
    }

    struct Corners {
        uint32_t texture;
        float u[4], v[4];
    };
    auto corners = [&](uint32_t def, uint32_t cell, Corners& out) {
        const uint32_t record = defs_ + def * def_size;
        out.texture = memory.load32(record + def_texture);
        cell = std::min(cell, uv_cells - 1);
        for (int i = 0; i < 4; ++i) {
            out.u[i] = uvs_[cell * 8 + i * 2];
            out.v[i] = uvs_[cell * 8 + i * 2 + 1];
        }
    };
    auto group = [&](uint16_t pass, uint16_t place, uint32_t texture0, uint32_t texture1) -> Group& {
        for (Group& g : cache.groups) {
            if (g.pass == pass && g.place == place && g.texture0 == texture0 && g.texture1 == texture1) {
                return g;
            }
        }

        cache.groups.emplace_back();
        Group& g = cache.groups.back();
        g.pass = pass;
        g.place = place;
        g.texture0 = texture0;
        g.texture1 = texture1;
        return g;
    };
    auto inside = [](float uv) {
        return uv >= -1.0f / 256 && uv <= 1.0f + 1.0f / 256;
    };
    auto append = [&](Group& g, const Corners& stage0, const Corners* stage1) {
        for (int i = 0; i < 4; ++i) {
            RenderVertex v{};
            v.position[0] = x[i] + cache.shift_x;
            v.position[1] = y[i] + cache.shift_y;
            v.position[2] = ground_z;
            v.position[3] = 1;
            v.diffuse[0] = v.diffuse[1] = v.diffuse[2] = gray[i];
            v.diffuse[3] = 1;
            // For the lit ground: the tile's place and the corner (GroundLight).
            v.normal[0] = px + cache.shift_x;
            v.normal[1] = py + cache.shift_y;
            v.normal[2] = float(i);
            v.uv[0][0] = stage0.u[i];
            v.uv[0][1] = stage0.v[i];
            if (stage1) {
                v.uv[1][0] = stage1->u[i];
                v.uv[1][1] = stage1->v[i];
            }

            for (int s = 0; s < 2; ++s) {
                if (!inside(v.uv[s][0])) {
                    g.clamp[s] &= uint8_t(~1u);
                }

                if (!inside(v.uv[s][1])) {
                    g.clamp[s] &= uint8_t(~2u);
                }
            }

            g.vertices.push_back(v);
            if (!cache.any) {
                cache.min_x = cache.max_x = x[i];
                cache.min_y = cache.max_y = y[i];
                cache.any = true;
            }

            cache.min_x = std::min(cache.min_x, x[i]);
            cache.max_x = std::max(cache.max_x, x[i]);
            cache.min_y = std::min(cache.min_y, y[i]);
            cache.max_y = std::max(cache.max_y, y[i]);
        }
    };
    Corners base;
    corners(uint32_t(key.def), memory.load32(defs_ + uint32_t(key.def) * def_size + def_uv_cell) & 0xffff, base);
    append(group(0, 0, base.texture, 0), base, nullptr);
    // Pass 0 (and every even one) of the layers draws two-texture records, the odd ones one-texture records; a
    // chain moves on to the next pass at each change of kind (drawTileLayers).
    uint32_t id = key.layers;
    int pass = 0, place = 0;
    for (int n = 0; id && n < 256; ++n) {
        const uint32_t record = uint32_t(
            control
                .invoke(layer_record_get, {Convention::Thiscall, ReturnChannel::Eax, {{4}, {4}}, ""}, {map_data_, id})
                .integer);
        if (!record) {
            break;
        }

        const uint32_t defs = memory.load32(record + record_defs), next = memory.load32(record + record_next);
        const uint32_t first = defs & 0x1ffff, second = defs >> 17;
        const bool two = second != 0;
        if (two != (pass % 2 == 0)) {
            ++pass;
            place = 0;
        }

        Corners c0, c1;
        corners(first, first % uv_cells, c0);
        if (two) {
            corners(second, second % uv_cells, c1);
        }

        append(group(uint16_t(1 + pass), uint16_t(place++), c0.texture, two ? c1.texture : 0), c0, two ? &c1 : nullptr);
        id = next;
    }

    cache.keys[t] = key;
    cache.built[t] = 1;
    ++stat_tiles_;
    return Build::Ok;
}

bool GroundMesh::visit_tile(Control& control, int slot, int t) {
    SectorId id;
    if (!sector_id(control, slot, id)) {
        return true;
    }

    SectorCache* cache = slots_[slot];
    if (!cache || !(cache->id == id)) {
        cache = cache_for(id);
        if (!cache) {
            return true;
        }

        slots_[slot] = cache;
    }

    cache->frame = frame_;
    uint8_t tile[tile_size];
    control.memory.read(id.tiles + uint32_t(t) * tile_size, tile, sizeof tile);
    TileKey key;
    std::memcpy(&key.def, tile, 4);
    std::memcpy(&key.layers, tile + tile_layers_at, 4);
    std::memcpy(&key.heights, tile + tile_heights, 4);
    std::memcpy(&key.light, tile + tile_light, 4);
    if (cache->built[t]) {
        // Quads cannot be taken out of the buffers: the sector starts over with the next frame (not in the middle
        // of this walk, which already visited some of its tiles).
        if (!(cache->keys[t] == key)) {
            cache->stale = true;
        }

        return true;
    }

    return build_tile(control, *cache, t, key, reinterpret_cast<const int8_t*>(tile + tile_heights),
                      tile + tile_light) == Build::Ok;
}

// Builds what is new in the row the game is about to walk, and takes the view offset from it.
void GroundMesh::visit_row(Control& control, uint32_t row_pointer) {
    auto& memory = control.memory;
    uint8_t row[12];
    memory.read(row_pointer, row, sizeof row);
    float x, y;
    int16_t tile, sector;
    std::memcpy(&x, row, 4);
    std::memcpy(&y, row + 4, 4);
    std::memcpy(&tile, row + 8, 2);
    std::memcpy(&sector, row + 10, 2);
    if (sector < 0 || sector > 8 || tile < 0 || tile > 0xfff) {
        return;
    }

    const int r0 = sector / 3 * sector_side + tile / sector_side;
    const int c0 = sector % 3 * sector_side + tile % sector_side;
    const float ox = x - 48.0f * float(c0 - r0), oy = y - 24.0f * float(c0 + r0);
    if (!have_offset_) {
        have_offset_ = true;
        offset_x_ = ox;
        offset_y_ = oy;
    }

    const int length = int(memory.load32(view_ + view_row_length));
    // Tile j of the row is (r0 - j, c0 + j) of the loaded tiles.
    for (int j = 0; j < length && active_; ++j) {
        const int r = r0 - j, c = c0 + j;
        if (r < 0 || c >= loaded_side) {
            break;
        }

        if (!visit_tile(control, r / sector_side * 3 + c / sector_side,
                        r % sector_side * sector_side + c % sector_side)) {
            return;
        }
    }
}

uint32_t GroundMesh::texture_surface(Control& control, uint32_t handle) {
    if (!handle) {
        return 0;
    }

    const uint32_t manager = control.memory.load32(texture_manager);
    if (!manager) {
        return 0;
    }

    const uint32_t texture = uint32_t(
        control
            .invoke(texture_get, {Convention::Thiscall, ReturnChannel::Eax, {{4}, {4}, {4}}, ""}, {manager, handle, 0})
            .integer);
    return texture ? control.memory.load32(texture + texture_surface_field) : 0;
}

// render_state_cache_set(one_texture_layers, on) as the game makes it (0x643430 -> 0x643470).
void GroundMesh::render_flag(Control& control, uint32_t, bool on) {
    auto& memory = control.memory;
    const uint32_t flags_object = memory.load32(render_flags);
    if (!flags_object) {
        return;
    }

    const uint32_t flags = memory.load32(flags_object + 4);
    if (((flags & one_texture_layers) != 0) == on) {
        return;
    }

    const uint32_t device = memory.load32(flags_object);
    static const uint32_t set_on[][3] = {{0, 4, 4}, {0, 5, 2}, {0, 6, 0}, {1, 1, 1}, {1, 4, 1}};
    static const uint32_t set_off[][3] = {{0, 5, 0}, {1, 1, 3}, {1, 3, 1}, {1, 4, 2}, {1, 5, 2}};
    for (const auto& s : on ? set_on : set_off) {
        dd_.device_texture_state(device, s[0], s[1], s[2]);
    }

    memory.store32(flags_object + 4, on ? flags | one_texture_layers : flags & ~one_texture_layers);
}

// The ground as drawTileLayers would leave it: base tiles, then the layer passes with their render flag.
void GroundMesh::draw_ground(Control& control, uint32_t device_pointer) {
    auto& memory = control.memory;
    auto found = dd_.devices_.find(device_pointer);
    if (found == dd_.devices_.end() || !have_offset_ || !ground_rows_) {
        return;
    }

    const float zoom = bits_float(memory.load32(view_ + view_zoom));
    if (!(zoom > 0.0f)) {
        return;
    }

    const float scale = 1.0f / zoom;
    auto& target = dd_.surface(found->second.target);
    if (light_.on) {
        // renderTileRow: trunc((row position / zoom - W/2) * zoom) + W/2, the same for y with H/2.
        light_.inverse_zoom = scale;
        light_.zoom = zoom;
        light_.origin[0] = float(target.image.width / 2);
        light_.origin[1] = float(target.image.height / 2);
    }

    if (window_ != target.window) {
        if (window_) {
            reset_all();
        }

        retired_.clear();
        window_ = target.window;
    }

    if (!retired_.empty()) {
        dd_.host_->render_posted(window_, [ids = std::move(retired_)](Renderer& renderer) {
            for (uint32_t id : ids) {
                renderer.mesh_destroy(id);
            }
        });
        retired_.clear();
    }

    const float width = float(target.image.width), height = float(target.image.height);
    int max_pass = -1;
    float slot_x[9] = {}, slot_y[9] = {};
    bool visible[9] = {};
    for (int s = 0; s < 9; ++s) {
        // Every loaded sector the cache has, walked this frame or not.
        SectorCache* cache = slots_[s];
        SectorId id;
        if (!cache && sector_id(control, s, id)) {
            slots_[s] = cache = find_cache(id);
        }

        if (!cache || !cache->any) {
            continue;
        }

        cache->frame = frame_;
        const int sr = s / 3 * sector_side, sc = s % 3 * sector_side;
        slot_x[s] = offset_x_ + 48.0f * float(sc - sr);
        slot_y[s] = offset_y_ + 24.0f * float(sc + sr);
        if ((cache->max_x + slot_x[s]) * scale < 0.0f || (cache->min_x + slot_x[s]) * scale > width ||
            (cache->max_y + slot_y[s]) * scale < 0.0f || (cache->min_y + slot_y[s]) * scale > height) {
            continue;
        }

        visible[s] = true;
        for (Group& group : cache->groups) {
            const uint32_t quads = uint32_t(group.vertices.size() / 4);
            if (quads > group.uploaded) {
                if (!group.mesh) {
                    group.mesh = next_mesh_++;
                }

                std::vector<RenderVertex> fresh(group.vertices.begin() + size_t(group.uploaded) * 4,
                                                group.vertices.end());
                dd_.host_->render_posted(window_,
                                         [id = group.mesh, first = group.uploaded * 4, fresh = std::move(fresh)](
                                             Renderer& renderer) { renderer.mesh_write(id, first, fresh); });
                group.uploaded = quads;
            }

            max_pass = std::max(max_pass, int(group.pass));
        }
    }

    std::unordered_map<uint32_t, uint32_t> surfaces; // texture handle -> surface, this frame
    auto surface_of = [&](uint32_t handle) {
        auto it = surfaces.find(handle);
        if (it != surfaces.end()) {
            return it->second;
        }

        uint32_t surface = texture_surface(control, handle);
        if (surface) {
            auto& image = dd_.surface(surface);
            if (image.locked) {
                surface = 0;
            } else {
                dd_.ensure_gpu(surface);
            }
        }

        surfaces.emplace(handle, surface);
        return surface;
    };
    if (base_valid_ && !base_reported_) {
        base_reported_ = true;
        const auto& now = found->second.state;
        std::string differs;
        for (size_t i = 0; i < now.render.size(); ++i) {
            if (now.render[i] != base_state_.render[i]) {
                differs += " rs" + std::to_string(i) + "=" + std::to_string(base_state_.render[i]) + "/" +
                           std::to_string(now.render[i]);
            }
        }

        for (size_t t = 0; t < 2; ++t) {
            for (size_t i = 0; i < now.stages[t].size(); ++i) {
                if (now.stages[t][i] != base_state_.stages[t][i]) {
                    differs += " ts" + std::to_string(t) + "." + std::to_string(i) + "=" +
                               std::to_string(base_state_.stages[t][i]) + "/" + std::to_string(now.stages[t][i]);
                }
            }
        }

        std::fprintf(stderr, "GROUND_MESH base state vs layer pass state:%s\n", differs.c_str());
    }

    // The visible groups in draw order: pass, place, then (in any order within them) textures, clamp and the camera
    // transform of their sector, so that groups that only differ in their sector become one draw call.
    struct Entry {
        const Group* group;
        float tx, ty;
    };
    std::vector<Entry> entries;
    for (int s = 0; s < 9; ++s) {
        if (!visible[s]) {
            continue;
        }

        const SectorCache& cache = *slots_[s];
        for (const Group& group : cache.groups) {
            if (group.uploaded) {
                entries.push_back({&group, slot_x[s] - cache.shift_x, slot_y[s] - cache.shift_y});
            }
        }
    }

    auto key = [](const Entry& e) {
        const Group& g = *e.group;
        return std::make_tuple(g.pass, g.place, g.texture0, g.texture1, g.clamp[0], g.clamp[1], e.tx, e.ty);
    };
    std::sort(entries.begin(), entries.end(), [&](const Entry& a, const Entry& b) { return key(a) < key(b); });
    size_t at = 0;
    for (int pass = 0; pass <= max_pass; ++pass) {
        const bool two = pass >= 1 && (pass - 1) % 2 == 0;
        bool base_sent = false;
        uint32_t base_index = 0;
        if (pass >= 1) {
            render_flag(control, device_pointer, !two);
        }

        while (at < entries.size() && entries[at].group->pass == pass) {
            size_t run_end = at + 1;
            while (run_end < entries.size()) {
                const auto a = key(entries[at]), b = key(entries[run_end]);
                if (std::get<0>(a) != std::get<0>(b) || std::get<1>(a) != std::get<1>(b) ||
                    std::get<2>(a) != std::get<2>(b) || std::get<3>(a) != std::get<3>(b) ||
                    std::get<4>(a) != std::get<4>(b) || std::get<5>(a) != std::get<5>(b) ||
                    std::get<6>(a) != std::get<6>(b) || std::get<7>(a) != std::get<7>(b)) {
                    break;
                }

                ++run_end;
            }

            const Group& first = *entries[at].group;
            // Stage 1 only for two-texture layers; elsewhere the game leaves it as it is.
            dd_.device_texture(device_pointer, 0, surface_of(first.texture0));
            if (two) {
                dd_.device_texture(device_pointer, 1, surface_of(first.texture1));
            }

            auto& device = dd_.devices_.at(device_pointer);
            const bool own_state = pass == 0 && base_valid_;
            dd_.host_->draw(window_, [&](DrawBatch& batch) {
                if (own_state) {
                    // Once per batch (a full batch is queued and the next one starts empty).
                    if (!base_sent || batch.items.empty()) {
                        batch.states.push_back(base_state_);
                        device.state_sent = false;
                        base_sent = true;
                        base_index = uint32_t(batch.states.size() - 1);
                    }
                } else if (batch.states.empty() || !device.state_sent || dd_.batch_device_ != device_pointer) {
                    batch.states.push_back(device.state);
                    device.state_sent = true;
                    dd_.batch_device_ = device_pointer;
                }

                DrawItem item;
                item.target = device.target;
                item.primitive = 4;
                item.state = own_state ? base_index : uint32_t(batch.states.size() - 1);
                item.textures = device.state.textures;
                item.transformed = true;
                item.diffuse = true;
                item.specular = false;
                item.mesh = first.mesh;
                item.first_quad = 0;
                item.quads = first.uploaded;
                item.first_range = uint32_t(batch.mesh_ranges.size());
                for (size_t e = at; e < run_end; ++e) {
                    batch.mesh_ranges.push_back({entries[e].group->mesh, 0, entries[e].group->uploaded});
                }

                item.ranges = uint32_t(run_end - at);
                item.mesh_transform[0] = scale;
                item.mesh_transform[1] = entries[at].tx * scale;
                item.mesh_transform[2] = entries[at].ty * scale;
                item.mesh_transform[3] = ground_z;
                item.mesh_clamp = {first.clamp[0], first.clamp[1]};
                if (light_.on) {
                    if (!batch.ground_light.on) {
                        batch.ground_light = light_;
                    }

                    item.mesh_lit = true;
                    item.mesh_light[0] = entries[at].tx;
                    item.mesh_light[1] = entries[at].ty;
                }

                batch.items.push_back(item);
            });
            target.gpu_dirty = true;
            ++stat_draws_;
            stat_ranges_ += run_end - at;
            at = run_end;
        }

        if (pass >= 1) {
            memory.store32(view_ + view_passes, memory.load32(view_ + view_passes) + 1);
        }
    }

    if (max_pass >= 1) {
        render_flag(control, device_pointer, true);
    }
}

bool GroundMesh::tile_row(X86* c) {
    if (!enabled_) {
        return false;
    }

    Control& control = Control::of(c);
    auto& memory = control.memory;
    const uint32_t view = c->r[R_ECX], esp = c->r[R_ESP];
    if (present_ != dd_.presents_) {
        begin_frame(control, view);
    }

    if (!active_ || view != view_) {
        return false;
    }

    if (memory.load32(esp + 12)) {
        ground_rows_ = true;
        visit_row(control, memory.load32(esp + 8));
    }

    if (light_.on) {
        // The quads that still pass through cTileRenderer (water) take their corners from its row buffers, which
        // expect every tile of a row; with the ground skipped they would be another tile's. In this state it looks
        // every corner up itself.
        memory.store32(tile_renderer + renderer_pass, 0);
        memory.store32(tile_renderer + renderer_rows, 2);
    }

    in_row_ = active_;
    c->eip = tile_row_entry;
    const bool completed = control.run_call();
    in_row_ = false;
    // The game's blend layer list is not used: kept empty, so it never fills up and asks for a flush mid-walk.
    if (completed) {
        memory.store32(view + view_layered_count, 0);
    }

    return true;
}

bool GroundMesh::tile_layers(X86* c) {
    if (!enabled_ || !active_ || c->r[R_ECX] != view_) {
        return false;
    }

    Control& control = Control::of(c);
    const uint32_t esp = c->r[R_ESP];
    const uint32_t device = control.memory.load32(esp + 4);
    if (!drawn_) {
        drawn_ = true;
        draw_ground(control, device);
    }

    c->eip = control.memory.load32(esp);
    c->r[R_ESP] = esp + 8;
    return true;
}

bool GroundMesh::skip_quad(X86* c, unsigned argument_bytes) {
    if (!in_row_ || !active_) {
        return false;
    }

    Control& control = Control::of(c);
    const uint32_t esp = c->r[R_ESP];
    if (!base_valid_) {
        auto found = dd_.devices_.find(control.memory.load32(esp + 4));
        if (found != dd_.devices_.end()) {
            base_state_ = found->second.state;
            base_valid_ = true;
        }
    }

    c->eip = control.memory.load32(esp);
    c->r[R_ESP] = esp + 4 + argument_bytes;
    return true;
}
}

// Stand-ins of runtime/sacred/native.c.
namespace {
sacred::GroundMesh* g_ground = nullptr;
}

namespace sacred {
void ground_mesh_register(GroundMesh* ground) {
    g_ground = ground;
}
}

extern "C" int sacred_ground_row(X86* c) {
    return g_ground && g_ground->tile_row(c);
}

extern "C" int sacred_ground_layers(X86* c) {
    return g_ground && g_ground->tile_layers(c);
}

extern "C" int sacred_ground_skip_build(uint32_t type) {
    return g_ground && g_ground->skip_build(type);
}

extern "C" int sacred_ground_skip_quad(X86* c, unsigned argument_bytes) {
    return g_ground && g_ground->skip_quad(c, argument_bytes);
}
