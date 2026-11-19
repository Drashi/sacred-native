#include "ddraw.h"
#include "vertices.h"
#include "../../platform/window_host.h"
#include <cstring>

namespace sacred {
namespace {
float scalar(uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

RenderState defaults(uint32_t width, uint32_t height) {
    RenderState result;
    result.width = width;
    result.height = height;
    auto& rs = result.render;
    rs[7] = 1;
    rs[8] = 3;
    rs[9] = 2;
    rs[14] = 1;
    rs[19] = 2;
    rs[20] = 1;
    rs[22] = 3;
    rs[23] = 4;
    rs[25] = 8;
    rs[37] = rs[38] = 0x3f800000;
    rs[53] = rs[54] = rs[55] = 1;
    rs[56] = 8;
    rs[58] = rs[59] = rs[60] = UINT32_MAX;
    rs[136] = rs[137] = rs[141] = rs[142] = 1;
    rs[145] = 1;
    rs[146] = 2;
    for (auto& matrix : result.matrices) {
        for (unsigned i = 0; i < 4; ++i) {
            matrix[i * 5] = 1;
        }
    }

    for (unsigned i = 0; i < 4; ++i) {
        result.material[0][i] = result.material[1][i] = 1;
    }

    for (unsigned i = 0; i < 2; ++i) {
        auto& stage = result.stages[i];
        stage[1] = i ? 1 : 4;
        stage[2] = 2;
        stage[3] = 1;
        stage[4] = i ? 1 : 2;
        stage[5] = 2;
        stage[6] = 1;
        stage[11] = i;
        stage[12] = stage[13] = stage[14] = stage[16] = stage[17] = stage[18] = stage[21] = 1;
    }

    return result;
}

void render_state(RenderState& state, uint32_t key, uint32_t value) {
    bool qualified = true;
    switch (key) {
    case 7:
    case 14:
    case 15:
    case 26:
    case 27:
    case 29:
    case 52:
    case 136:
    case 137:
    case 141:
    case 142:
    case 143:
        qualified = value <= 1;
        break;
    case 8:
    case 22:
        qualified = value >= 1 && value <= 3;
        break;
    case 9:
        qualified = value == 2;
        break;
    case 19:
        qualified = value >= 1 && value <= 11;
        break;
    case 20:
        qualified = value >= 1 && value <= 10;
        break;
    case 23:
    case 25:
    case 53:
    case 54:
    case 55:
    case 56:
        qualified = value >= 1 && value <= 8;
        break;
    case 24:
    case 34:
    case 36:
    case 37:
    case 38:
    case 57:
    case 58:
    case 59:
    case 60:
    case 139:
        break;
    case 28:
    case 35:
    case 47:
    case 48:
    case 140:
    case 151:
    case 152:
        qualified = value == 0;
        break;
    case 145:
    case 146:
    case 147:
    case 148:
        qualified = value <= 2;
        break;
    default:
        qualified = false;
        break;
    }

    if (!qualified) {
        throw GuestFault("unqualified D3D render state " + std::to_string(key) + "=" + std::to_string(value));
    }

    state.render[key] = value;
}

// The game sets most render and texture-stage states again before every draw call, to the value they already have.
bool render_state_changes(const RenderState& state, uint32_t key, uint32_t value) {
    return key >= state.render.size() || state.render[key] != value;
}

bool texture_state_changes(const RenderState& state, uint32_t stage, uint32_t key, uint32_t value) {
    return stage >= 2 || key >= 25 || state.stages[stage][key] != value;
}

unsigned transform_index(uint64_t state) {
    if (state >= 1 && state <= 3) {
        return unsigned(state - 1);
    }

    if (state == 16 || state == 17) {
        return unsigned(state - 13);
    }

    throw GuestFault("unqualified D3D transform");
}

void texture_state(RenderState& state, uint32_t stage, uint32_t key, uint32_t value) {
    if (stage >= 2 || key >= 25) {
        throw GuestFault("unqualified D3D texture stage");
    }

    bool qualified = true;
    switch (key) {
    case 1:
    case 4:
        qualified = value >= 1 && value <= 16;
        break;
    case 2:
    case 3:
    case 5:
    case 6:
        qualified = !(value & ~0x3fu) && (value & 15) <= 4;
        break;
    case 11:
        qualified = value < 2;
        break;
    case 12:
    case 13:
    case 14:
        qualified = value >= 1 && value <= 4;
        break;
    case 15:
        break;
    case 16:
    case 17:
        qualified = value == 1 || value == 2;
        break;
    case 18:
    case 21:
        qualified = value == 1;
        break;
    case 24:
        qualified = value == 0 || value == 2;
        break;
    case 7:
    case 8:
    case 9:
    case 10:
    case 19:
    case 20:
    case 22:
    case 23:
        qualified = value == 0;
        break;
    default:
        qualified = false;
        break;
    }

    if (!qualified) {
        throw GuestFault("unqualified D3D texture state " + std::to_string(key) + "=" + std::to_string(value));
    }

    state.stages[stage][key] = value;
    if (key == 12) {
        state.stages[stage][13] = state.stages[stage][14] = value;
    }
}
}

uint32_t DirectDraw::create_device(uint32_t parent, uint32_t target) {
    auto& owner = adapter(parent);
    auto& buffer = surface(target);
    if (buffer.parent != owner.draw || buffer.render_format != RenderFormat::Rgba8 || !(buffer.caps & 0x2000)) {
        throw GuestFault("unqualified D3D render target");
    }

    ensure_gpu(target);
    uint32_t pointer = control_.heap.allocate(4);
    control_.memory.store32(pointer, table(Interface::Device7));
    devices_.emplace(pointer, Device{owner.draw, 1, target, defaults(buffer.image.width, buffer.image.height)});
    ++owner.references;
    ++buffer.references;
    return pointer;
}

uint32_t DirectDraw::release_device(uint32_t pointer) {
    auto found = devices_.find(pointer);
    if (found == devices_.end()) {
        throw GuestFault("invalid or released D3D device pointer");
    }

    uint32_t references = --found->second.references;
    if (references) {
        return references;
    }

    uint32_t parent = found->second.parent, target = found->second.target;
    auto textures = found->second.state.textures;
    devices_.erase(found);
    control_.heap.free(pointer);
    for (uint32_t texture : textures) {
        if (texture) {
            release_surface(texture);
        }
    }

    release_surface(target);
    unknown(2, {parent});
    return 0;
}

void DirectDraw::device_texture(uint32_t pointer, uint32_t stage, uint32_t texture) {
    auto found = devices_.find(pointer);
    if (found == devices_.end()) {
        throw GuestFault("invalid or released D3D device pointer");
    }

    auto& device = found->second;
    if (stage >= 2) {
        throw GuestFault("unqualified D3D texture stage");
    }

    const uint32_t old = device.state.textures[stage];
    if (texture == old) {
        return;
    }

    if (texture) {
        auto& image = surface(texture);
        if (image.parent != device.parent || image.render_format != RenderFormat::Rgba8 || !(image.caps & 0x1000)) {
            throw GuestFault("unqualified D3D texture surface");
        }

        ++image.references;
    }

    device.state.textures[stage] = texture;
    if (old) {
        release_surface(old);
    }
}

void DirectDraw::device_texture_state(uint32_t pointer, uint32_t stage, uint32_t key, uint32_t value) {
    auto found = devices_.find(pointer);
    if (found == devices_.end()) {
        throw GuestFault("invalid or released D3D device pointer");
    }

    if (texture_state_changes(found->second.state, stage, key, value)) {
        texture_state(found->second.state, stage, key, value);
        found->second.state_sent = false;
    }
}

Result DirectDraw::device_method(unsigned slot, const std::vector<uint64_t>& a) {
    uint32_t pointer = uint32_t(a[0]);
    auto found = devices_.find(pointer);
    if (found == devices_.end()) {
        throw GuestFault("invalid or released D3D device pointer");
    }

    auto& device = found->second;
    auto& memory = control_.memory;
    if (slot == 0) {
        const std::array<uint8_t, 16> own{0x79, 0x9e, 4, 0xf5, 0x61, 0x48, 0xd2, 0x11,
                                          0xa4, 7,    0, 0xa0, 0xc9, 6,    0x29, 0xa8};
        const std::array<uint8_t, 16> identity{0, 0, 0, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46};
        std::array<uint8_t, 16> iid{};
        memory.read(uint32_t(a[1]), iid.data(), iid.size());
        bool known = iid == own || iid == identity;
        memory.store32(uint32_t(a[2]), known ? pointer : 0);
        if (known) {
            ++device.references;
        }

        return Result{known ? 0u : 0x80004002u, {}};
    }

    if (slot == 1) {
        return Result{++device.references, {}};
    }

    if (slot == 2) {
        return Result{release_device(pointer), {}};
    }

    if (slot == 4) {
        uint32_t data = control_.heap.allocate(32);
        try {
            for (auto pixel : {PixelFormat::Rgb565, PixelFormat::Argb1555, PixelFormat::Argb4444, PixelFormat::Xrgb8888,
                               PixelFormat::Argb8888}) {
                auto format = describe(pixel);
                const uint32_t fields[] = {32,          0x40u | (format.alpha ? 1u : 0u),
                                           0,           format.bits,
                                           format.red,  format.green,
                                           format.blue, format.alpha};
                memory.write(data, fields, sizeof fields);
                auto result = active_control().invoke(
                    uint32_t(a[1]), {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}}, ""}, {data, a[2]});
                if (active_control().transferring() || !result.integer) {
                    break;
                }
            }
        } catch (...) {
            control_.heap.free(data);
            throw;
        }

        control_.heap.free(data);
        return Result{0, {}};
    }

    if (slot == 5 || slot == 6) {
        if (device.scene == (slot == 5)) {
            throw GuestFault("unqualified D3D scene nesting");
        }

        device.scene = slot == 5;
        if (slot == 5) {
            ++scenes_;
            inset_schedule_.drew();
        }

        return Result{0, {}};
    }

    if (slot == 8) {
        if (a[2]) {
            throw GuestFault("unqualified render-target flags");
        }

        auto& target = surface(uint32_t(a[1]));
        if (target.parent != device.parent || !(target.caps & 0x2000) || target.render_format != RenderFormat::Rgba8) {
            throw GuestFault("unqualified D3D render target");
        }

        if (device.target != a[1]) {
            ensure_gpu(uint32_t(a[1]));
            revert_texture(uint32_t(a[1]));
            target.content_hash = 0;
            target.backdrop_tile = nullptr;
            ++target.references;
            release_surface(device.target);
            device.target = uint32_t(a[1]);
        }

        return Result{0, {}};
    }

    if (slot == 10) {
        auto& target = surface(device.target);
        const auto& state = device.state;
        int x = 0, y = 0, width = 0, height = 0;
        if (a[1] == 1 && a[2]) {
            // One D3DRECT {x1, y1, x2, y2} (the LAN screens clear the depth of a sub-rectangle), clipped to the target.
            memory.check(uint32_t(a[2]), 16, Read);
            const int x1 = std::max(0, int(memory.load32(uint32_t(a[2])))),
                      y1 = std::max(0, int(memory.load32(uint32_t(a[2]) + 4)));
            const int x2 = std::min(int(target.image.width), int(memory.load32(uint32_t(a[2]) + 8)));
            const int y2 = std::min(int(target.image.height), int(memory.load32(uint32_t(a[2]) + 12)));
            if (x2 <= x1 || y2 <= y1) {
                return Result{0, {}};
            }

            x = x1;
            y = y1;
            width = x2 - x1;
            height = y2 - y1;
        } else if (a[1] || a[2] || state.x || state.y || state.width != target.image.width ||
                   state.height != target.image.height) {
            throw GuestFault("unqualified partial D3D clear");
        }

        if ((a[3] & 6) && !target.attached) {
            throw GuestFault("D3D clear requires depth attachment");
        }

        // Queued like the draw calls around it: the guest does not wait for the window thread to catch up.
        host_->render_posted(target.window, [id = device.target, flags = uint32_t(a[3]), color = uint32_t(a[4]),
                                             depth = scalar(uint32_t(a[5])), stencil = uint32_t(a[6]), x, y, width,
                                             height](Renderer& renderer) {
            renderer.clear(id, flags, color, depth, stencil, x, y, width, height);
        });
        if (a[3] & 1) {
            target.gpu_dirty = true;
        }

        return Result{0, {}};
    }

    if (slot == 11) {
        auto& matrix = device.state.matrices[transform_index(a[1])];
        memory.check(uint32_t(a[2]), 64, Read);
        for (unsigned i = 0; i < 16; ++i) {
            matrix[i] = scalar(memory.load32(uint32_t(a[2]) + i * 4));
        }

        device.state_sent = false;
        return Result{0, {}};
    }

    if (slot == 12) {
        const auto& matrix = device.state.matrices[transform_index(a[1])];
        memory.check(uint32_t(a[2]), 64, Write);
        for (unsigned i = 0; i < 16; ++i) {
            uint32_t bits;
            const float value = matrix[i];
            std::memcpy(&bits, &value, sizeof bits);
            memory.store32(uint32_t(a[2]) + i * 4, bits);
        }

        return Result{0, {}};
    }

    if (slot == 13) {
        memory.check(uint32_t(a[1]), 24, Read);
        // An empty viewport fails with DDERR_INVALIDPARAMS and leaves the current one, as Direct3D 7 does: the world
        // map's zoom view asks for one while its zoom decays to zero after the button is released.
        if (!memory.load32(uint32_t(a[1]) + 8) || !memory.load32(uint32_t(a[1]) + 12)) {
            return Result{0x80070057, {}};
        }

        auto& state = device.state;
        device.state_sent = false;
        state.x = memory.load32(uint32_t(a[1]));
        state.y = memory.load32(uint32_t(a[1]) + 4);
        state.width = memory.load32(uint32_t(a[1]) + 8);
        state.height = memory.load32(uint32_t(a[1]) + 12);
        state.min_z = scalar(memory.load32(uint32_t(a[1]) + 16));
        state.max_z = scalar(memory.load32(uint32_t(a[1]) + 20));
        auto& target = surface(device.target);
        if (!state.width || !state.height || uint64_t(state.x) + state.width > target.image.width ||
            uint64_t(state.y) + state.height > target.image.height || state.min_z < 0 || state.max_z > 1 ||
            state.min_z > state.max_z) {
            throw GuestFault("unqualified D3D viewport " + std::to_string(state.x) + "," + std::to_string(state.y) +
                             " " + std::to_string(state.width) + "x" + std::to_string(state.height) + " z " +
                             std::to_string(state.min_z) + ".." + std::to_string(state.max_z) + " on a " +
                             std::to_string(target.image.width) + "x" + std::to_string(target.image.height) +
                             " target");
        }

        return Result{0, {}};
    }

    if (slot == 16) {
        memory.check(uint32_t(a[1]), 68, Read);
        device.state_sent = false;
        for (unsigned field = 0; field < 4; ++field) {
            for (unsigned channel = 0; channel < 4; ++channel) {
                device.state.material[field][channel] =
                    scalar(memory.load32(uint32_t(a[1]) + field * 16 + channel * 4));
            }
        }

        device.state.power = scalar(memory.load32(uint32_t(a[1]) + 64));
        return Result{0, {}};
    }

    if (slot == 18) {
        const uint32_t base = uint32_t(a[2]);
        if (a[1] >= device.state.lights.size()) {
            throw GuestFault("unqualified D3D light index");
        }

        memory.check(base, 104, Read);
        const uint32_t type = memory.load32(base);
        if (type < 1 || type > 3) {
            throw GuestFault("unqualified D3D light type");
        }

        auto field = [&](unsigned offset) {
            return scalar(memory.load32(base + offset));
        };
        auto& light = device.state.lights[a[1]];
        device.state_sent = false;
        light.type = type;
        for (unsigned i = 0; i < 4; ++i) {
            light.diffuse[i] = field(4 + i * 4);
            light.specular[i] = field(20 + i * 4);
            light.ambient[i] = field(36 + i * 4);
        }

        for (unsigned i = 0; i < 3; ++i) {
            light.position[i] = field(52 + i * 4);
            light.direction[i] = field(64 + i * 4);
            light.attenuation[i] = field(84 + i * 4);
        }

        light.range = field(76);
        light.falloff = field(80);
        light.theta = field(96);
        light.phi = field(100);
        device.lights_set |= 1u << a[1];
        return Result{0, {}};
    }

    if (slot == 44) {
        if (a[1] >= device.state.lights.size() || !(device.lights_set >> a[1] & 1)) {
            throw GuestFault("unqualified D3D light enable");
        }

        device.state.lights[a[1]].enabled = a[2] != 0;
        device.state_sent = false;
        return Result{0, {}};
    }

    if (slot == 20) {
        if (render_state_changes(device.state, uint32_t(a[1]), uint32_t(a[2]))) {
            render_state(device.state, uint32_t(a[1]), uint32_t(a[2]));
            device.state_sent = false;
        }

        return Result{0, {}};
    }

    if (slot == 35) {
        device_texture(pointer, uint32_t(a[1]), uint32_t(a[2]));
        return Result{0, {}};
    }

    if (slot == 37) {
        device_texture_state(pointer, uint32_t(a[1]), uint32_t(a[2]), uint32_t(a[3]));
        return Result{0, {}};
    }

    if (slot == 25 || slot == 26 || slot == 30) {
        // D3DDP_WAIT (1) and D3DDP_DONOTUPDATEEXTENTS (8) only schedule the call or skip bookkeeping we do not keep;
        // the ALT item-name overlay (FUN_00627180, call at 0x62793f) passes D3DDP_WAIT.
        const uint64_t flags = a[slot == 25 ? 5 : 7];
        if (!device.scene) {
            throw GuestFault("D3D draw outside BeginScene/EndScene");
        }

        if (flags & ~uint64_t(0x9)) {
            throw GuestFault("unqualified D3D draw flags " + std::to_string(flags));
        }

        if (!a[4] || (slot != 25 && !a[6])) {
            return Result{0, {}};
        }

        auto& vertices = draw_vertices_;
        auto& indices = draw_indices_;
        // A plain vertex buffer goes to the renderer thread as the guest's bytes and is expanded there
        // (expand_raw_vertices); a strided one is gathered here.
        const uint32_t fvf = uint32_t(a[2]), count = uint32_t(a[4]);
        FvfLayout raw;
        skin_draw_.rows = nullptr;
        skin_draw_.mesh = 0;
        skin_draw_.upload.clear();
        skin_draw_.retired.clear();
        skin_draw_.color_on = false;
        const bool skinned =
            slot == 30 && skin_ && skin_->draw(memory, fvf, uint32_t(a[3]), count, vertices, skin_draw_);
        if (slot == 30) {
            if (!skinned) {
                decode_strided(memory, fvf, uint32_t(a[3]), count, vertices);
            }
        } else {
            raw = fvf_layout(fvf);
            if (!raw.stride) {
                throw GuestFault("unqualified D3D7 FVF");
            }

            memory.check(uint32_t(a[3]), uint64_t(raw.stride) * count, Read);
            vertices.vertices.clear();
            vertices.transformed = raw.transformed;
            vertices.diffuse = raw.diffuse;
            vertices.specular = raw.specular;
            vertices.coordinates = raw.coordinates;
        }

        if (slot != 25) {
            decode_indices(memory, uint32_t(a[5]), uint32_t(a[6]), count, indices);
        } else {
            indices.clear();
        }

        const auto& state = device.state;
        auto& target = surface(device.target);
        if (target.locked) {
            return Result{0x887601ae, {}};
        }

        for (unsigned i = 0; i < state.textures.size(); ++i) {
            if (state.textures[i]) {
                auto& texture = surface(state.textures[i]);
                if (texture.locked) {
                    return Result{0x887601ae, {}};
                }

                if (state.textures[i] == device.target) {
                    throw GuestFault("unqualified render-target texture feedback");
                }

                if (state.stages[i][1] != 1 && state.stages[i][11] >= vertices.coordinates) {
                    throw GuestFault("D3D texture coordinate index exceeds FVF");
                }

                ensure_gpu(state.textures[i]);
            }
        }

        if (skinned && (!skin_draw_.upload.empty() || !skin_draw_.retired.empty())) {
            host_->render_posted(target.window, [id = skin_draw_.mesh, upload = std::move(skin_draw_.upload),
                                                 retired = std::move(skin_draw_.retired)](Renderer& renderer) {
                for (uint32_t old : retired) {
                    renderer.mesh_destroy(old);
                }

                if (!upload.empty()) {
                    renderer.mesh_write(id, 0, upload);
                }
            });
        }

        // A translucent untextured quad over the whole target (the shade under a confirmation window of the save game
        // screen, a fade) also covers the wide picture's sides drawn at presentation: note what it does to them.
        if (backdrops_ && backdrops_->wide && raw.stride && raw.transformed && raw.diffuse && !state.textures[0] &&
            count >= 3 && count <= 6 && state.render[27] && state.render[19] == 5 && state.render[20] == 6) {
            backdrop_shade(reinterpret_cast<const void*>(memory.host_address(uint32_t(a[3]))), raw, count,
                           target.image.width, target.image.height);
        }

        DrawItem substitute;
        if (backdrops_ && backdrops_->wide && state.textures[0] && state.stages[0][11] < 2) {
            backdrop_substitute(target.window, state.textures[0], state.stages[0][11], substitute);
        }

        ++stat_draws_;
        ++stat_draws_total_;
        if (state.textures[0] != stat_last_texture_) {
            ++stat_texture_runs_;
            stat_last_texture_ = state.textures[0];
        }

        if (!device.state_sent) {
            ++stat_state_runs_;
        }

        host_->draw(target.window, [&](DrawBatch& batch) {
            // A new batch starts without states; one state serves every draw call until the device changes it.
            if (batch.states.empty() || !device.state_sent || batch_device_ != pointer) {
                batch.states.push_back(state);
                device.state_sent = true;
                batch_device_ = pointer;
            }

            DrawItem item;
            item.target = device.target;
            item.state = uint32_t(batch.states.size() - 1);
            item.primitive = uint32_t(a[1]);
            item.textures = state.textures;
            if (substitute.textures[0]) {
                item.textures[0] = substitute.textures[0];
            }

            item.remap = substitute.remap;
            item.first_vertex = uint32_t(batch.vertices.size());
            item.vertex_count = count;
            if (raw.stride) {
                item.raw_fvf = fvf;
                item.raw_offset = uint32_t(batch.raw.size());
                const uint8_t* bytes = reinterpret_cast<const uint8_t*>(memory.host_address(uint32_t(a[3])));
                batch.raw.insert(batch.raw.end(), bytes, bytes + size_t(raw.stride) * count);
            }

            item.first_index = uint32_t(batch.indices.size());
            item.index_count = uint32_t(indices.size());
            item.transformed = vertices.transformed;
            item.diffuse = vertices.diffuse;
            item.specular = vertices.specular;
            if (skinned) {
                item.skin_first = uint32_t(batch.skin_rows.size() / 4);
                item.skin_bones = uint32_t(skin_draw_.rows->size() / 12);
                item.skin_normalize = skin_draw_.normalize;
                batch.skin_rows.insert(batch.skin_rows.end(), skin_draw_.rows->begin(), skin_draw_.rows->end());
                if (skin_draw_.mesh) {
                    item.skin_mesh = skin_draw_.mesh;
                    item.first_vertex = 0;
                    item.mesh_clamp = {skin_draw_.clamp, skin_draw_.clamp};
                    item.skin_color_on = skin_draw_.color_on;
                    std::copy(std::begin(skin_draw_.color), std::end(skin_draw_.color), item.skin_color);
                }
            }

            batch.vertices.insert(batch.vertices.end(), vertices.vertices.begin(), vertices.vertices.end());
            batch.indices.insert(batch.indices.end(), indices.begin(), indices.end());
            batch.items.push_back(item);
        });
        target.gpu_dirty = true;
        return Result{0, {}};
    }

    throw GuestFault("unimplemented D3D device COM method");
}
}
