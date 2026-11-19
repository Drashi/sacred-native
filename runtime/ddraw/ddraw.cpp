#include "ddraw.h"
#include "../win32/menu.h"
#include "../../platform/window_host.h"
#include <array>
#include <thread>
#include <cstdio>
#include <cstdlib>

namespace sacred {
namespace {
constexpr uint32_t com_tokens = layout::hle_end - 0x10000;
constexpr std::array<uint8_t, 16> draw7_iid{0xc0, 0x5e, 0xe6, 0x15, 0x9c, 0x3b, 0xd2, 0x11,
                                            0xb9, 0x2f, 0,    0x60, 0x97, 0x97, 0xea, 0x5b};
constexpr std::array<uint8_t, 16> draw1_iid{0x80, 0xdb, 0x14, 0x6c, 0x33, 0xa7, 0xce, 0x11,
                                            0xa5, 0x21, 0,    0x20, 0xaf, 0x0b, 0xe5, 0x60};
constexpr std::array<uint8_t, 16> d3d7_iid{0x77, 0x9e, 0x04, 0xf5, 0x61, 0x48, 0xd2, 0x11,
                                           0xa4, 0x07, 0,    0xa0, 0xc9, 6,    0x29, 0xa8};
constexpr std::array<uint8_t, 16> unknown_iid{0, 0, 0, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46};
constexpr std::array<uint8_t, 16> rgb_device_iid{0x60, 0x5c, 0x66, 0xa4, 0x73, 0x26, 0xcf, 0x11,
                                                 0xa3, 0x1a, 0,    0xaa, 0,    0xb9, 0x33, 0x56};
std::array<uint8_t, 16> guid(Memory& memory, uint32_t address) {
    std::array<uint8_t, 16> value;
    memory.read(address, value.data(), value.size());
    return value;
}
}

DirectDraw::~DirectDraw() {
    if (ground_) {
        ground_mesh_register(nullptr);
    }

    while (!devices_.empty()) {
        auto first = devices_.begin();
        first->second.references = 1;
        release_device(first->first);
    }

    while (!surfaces_.empty()) {
        auto first = surfaces_.begin();
        first->second.references = 1;
        release_surface(first->first);
    }

    for (const auto& pair : clippers_) {
        control_.heap.free(pair.first);
    }

    for (const auto& pair : adapters_) {
        if (pair.second.d3d) {
            control_.heap.free(pair.second.d3d);
        }

        if (pair.second.draw1) {
            control_.heap.free(pair.second.draw1);
        }

        control_.heap.free(pair.second.draw);
    }

    for (uint32_t pointer : tables_) {
        if (pointer) {
            control_.heap.free(pointer);
        }
    }
}

uint32_t DirectDraw::table(Interface type) {
    unsigned index = static_cast<unsigned>(type);
    auto& pointer = tables_[index];
    if (!pointer) {
        const unsigned counts[] = {30, 8, 49, 49, 9, 23, 36};
        unsigned count = counts[index];
        pointer = control_.heap.allocate(count * 4);
        for (unsigned slot = 0; slot < count; ++slot) {
            control_.memory.store32(pointer + slot * 4, com_tokens + index * 0x1000 + slot * 16);
        }
    }

    return pointer;
}

DirectDraw::Adapter& DirectDraw::adapter(uint32_t pointer) {
    for (auto& pair : adapters_) {
        if (pointer == pair.second.draw || (pair.second.d3d && pointer == pair.second.d3d) ||
            (pair.second.draw1 && pointer == pair.second.draw1)) {
            return pair.second;
        }
    }

    throw GuestFault("invalid or released DirectDraw COM pointer");
}

Result DirectDraw::unknown(unsigned slot, const std::vector<uint64_t>& a) {
    auto& object = adapter(uint32_t(a[0]));
    if (slot == 1) {
        return Result{++object.references, {}};
    }

    if (slot == 2) {
        uint32_t references = --object.references;
        if (!references) {
            uint32_t draw = object.draw;
            if (object.d3d) {
                control_.heap.free(object.d3d);
            }

            if (object.draw1) {
                control_.heap.free(object.draw1);
            }

            control_.heap.free(draw);
            adapters_.erase(draw);
        }

        return Result{references, {}};
    }

    auto iid = guid(control_.memory, uint32_t(a[1]));
    control_.memory.check(uint32_t(a[2]), 4, Write);
    uint32_t result = 0;
    if (iid == draw7_iid || iid == unknown_iid) {
        result = object.draw;
    } else if (iid == draw1_iid) {
        if (!object.draw1) {
            object.draw1 = control_.heap.allocate(4);
            control_.memory.store32(object.draw1, table(Interface::Draw1));
        }

        result = object.draw1;
    } else if (iid == d3d7_iid) {
        if (!object.d3d) {
            object.d3d = control_.heap.allocate(4);
            control_.memory.store32(object.d3d, table(Interface::D3d7));
        }

        result = object.d3d;
    }

    control_.memory.store32(uint32_t(a[2]), result);
    if (result) {
        ++object.references;
    }

    return Result{result ? 0u : 0x80004002u, {}};
}

uint32_t DirectDraw::texture_limit() {
    if (!texture_limit_) {
        render([&](Renderer& renderer) { texture_limit_ = renderer.max_texture_size(); });
    }

    return texture_limit_;
}

void DirectDraw::render(const std::function<void(Renderer&)>& command) {
    if (!host_ || !menu_) {
        throw GuestFault("DirectDraw requires the SDL host");
    }

    host_->render(menu_->native_window(), command);
}

Result DirectDraw::method(Interface type, unsigned slot, const std::vector<uint64_t>& a) {
    if (type == Interface::Surface7 || type == Interface::Surface1) {
        return surface_method(slot, a);
    }

    if (type == Interface::Device7) {
        return device_method(slot, a);
    }

    if (type == Interface::Clipper) {
        return clipper_method(slot, a);
    }

    if (slot < 3) {
        return unknown(slot, a);
    }

    adapter(uint32_t(a[0]));
    if (type == Interface::Draw7 && slot == 22) {
        if (a[1] != 1 || a[2]) {
            throw GuestFault("unqualified vertical blank flags or event");
        }

        // A timed sleep wakes late (macOS: 0.4 ms on average and 1 ms at worst at the highest thread class, 0.9 and
        // 4 ms at the default one), and a frame that misses a tick by that much waits for the next one. Sleep to
        // a millisecond before the tick and spin the rest.
        constexpr uint32_t fade_step_return = 0x00645ea8u;  // dxDriver7::flip, one fade step per flip
        constexpr uint32_t frame_wait_return = 0x00645fcbu; // dxDriver7::flip, WAITRETRACE waits per flip
        if (frame_vblank_default_off && vblank_return_ == frame_wait_return) {
            return Result{0, {}};
        }

        const FrameClock& clock = vblank_return_ == fade_step_return ? fade_clock_ : frame_clock_;
        const auto deadline = clock.deadline(FrameClock::Clock::now());
        std::this_thread::sleep_until(deadline - std::chrono::milliseconds(1));
        while (FrameClock::Clock::now() < deadline) {
            std::this_thread::yield();
        }

        return Result{0, {}};
    }

    if (type == Interface::Draw7 && slot == 4) {
        if (a[1] || a[3]) {
            throw GuestFault("unqualified clipper flags or aggregation");
        }

        control_.memory.check(uint32_t(a[2]), 4, Write);
        uint32_t pointer = control_.heap.allocate(4);
        control_.memory.store32(pointer, table(Interface::Clipper));
        clippers_.emplace(pointer, Clipper{});
        control_.memory.store32(uint32_t(a[2]), pointer);
        return Result{0, {}};
    }

    if (type == Interface::D3d7 && slot == 4) {
        if (guid(control_.memory, uint32_t(a[1])) != rgb_device_iid) {
            throw GuestFault("unqualified D3D device class");
        }

        control_.memory.check(uint32_t(a[3]), 4, Write);
        uint32_t pointer = create_device(adapter(uint32_t(a[0])).draw, uint32_t(a[2]));
        control_.memory.store32(uint32_t(a[3]), pointer);
        return Result{0, {}};
    }

    if (type == Interface::Draw7 && slot == 6) {
        if (a[3]) {
            throw GuestFault("unqualified surface aggregation");
        }

        control_.memory.check(uint32_t(a[2]), 4, Write);
        uint32_t pointer = create_surface(uint32_t(a[0]), uint32_t(a[1]));
        control_.memory.store32(uint32_t(a[2]), pointer);
        return Result{pointer ? 0u : 0x8876017cu, {}};
    }

    if (type == Interface::Draw7 && slot == 19) {
        // The port never changes the desktop display mode (SetCooperativeLevel stays windowed), so there is nothing
        // to restore; the original returns DD_OK when no mode was set. Sacred calls this on deactivation and in its
        // WM_CLOSE shutdown path.
        return Result{0, {}};
    }

    if (type == Interface::Draw7 && slot == 20) {
        if (a[2] != 8 && a[2] != 0xc08) {
            throw GuestFault("unqualified DirectDraw cooperative mode");
        }

        if (a[1]) {
            if (!menu_ || !host_) {
                throw GuestFault("cooperative level requires the SDL window host");
            }

            menu_->native_window(uint32_t(a[1]));
        } else if (a[2] != 8) {
            throw GuestFault("missing cooperative window");
        }

        adapter(uint32_t(a[0])).window = uint32_t(a[1]);
        // The windowed path retains desktop ownership. Guest execution is
        // serialized, and the measured FPUSETUP call leaves its explicit x87
        // state unchanged, as in the original Wine trace.
        return Result{0, {}};
    }

    if (type == Interface::Draw7 && slot == 11) {
        // Only the implemented 3D/windowed path is advertised. Blitter, palette,
        // compressed-format, overlay and hardware claims remain absent.
        for (unsigned i = 1; i < 3; ++i) {
            if (a[i]) {
                if (control_.memory.load32(uint32_t(a[i])) != 380) {
                    throw GuestFault("unqualified DDCAPS layout");
                }

                std::array<uint32_t, 95> caps{};
                caps[0] = 380;
                caps[1] = 1;
                caps[2] = 0x80000;
                caps[14] = 0x500;
                // The virtual adapter's resource budget is enforced at allocation.
                caps[15] = 256u * 1024 * 1024;
                caps[16] = caps[15] - adapter(uint32_t(a[0])).used;
                caps[33] = caps[91] = 0x2000 | 0x1000 | 0x4000 | 0x20000;
                control_.memory.write(uint32_t(a[i]), caps.data(), sizeof caps);
            }
        }

        return Result{0, {}};
    }

    if (type == Interface::Draw7 && slot == 8) {
        if (a[1] || a[2] || !a[4]) {
            throw GuestFault("unqualified display mode enumeration");
        }

        const uint32_t limit = texture_limit();
        if (limit < 1024) {
            throw GuestFault("renderer cannot support original menu resolution");
        }

        // The original logical game mode, then an opt-in configured one; SDL presentation scales either.
        std::vector<std::pair<uint32_t, uint32_t>> modes{{1024, 768}};
        if (configured_width_ && configured_height_ && (configured_width_ != 1024 || configured_height_ != 768)) {
            if (configured_width_ > limit || configured_height_ > limit) {
                throw GuestFault("renderer cannot support the configured game resolution");
            }

            modes.emplace_back(configured_width_, configured_height_);
        }

        uint32_t descriptor = control_.heap.allocate(124);
        try {
            for (const auto& [width, height] : modes) {
                std::vector<uint8_t> zero(124);
                control_.memory.write(descriptor, zero.data(), zero.size());
                control_.memory.store32(descriptor, 124);
                control_.memory.store32(descriptor + 4, 0x4100e);
                control_.memory.store32(descriptor + 8, height);
                control_.memory.store32(descriptor + 12, width);
                control_.memory.store32(descriptor + 16, width * 4);
                control_.memory.store32(descriptor + 24, FrameClock::rate);
                const uint32_t format[] = {32, 0x40, 0, 32, 0xff0000, 0xff00, 0xff, 0};
                control_.memory.write(descriptor + 72, format, sizeof format);
                const Result next = active_control().invoke(
                    uint32_t(a[4]), {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}}, ""}, {descriptor, a[3]});
                if (!uint32_t(next.integer)) {
                    break; // DDENUMRET_CANCEL
                }
            }
        } catch (...) {
            control_.heap.free(descriptor);
            throw;
        }

        control_.heap.free(descriptor);
        return Result{0, {}};
    }

    if (type == Interface::D3d7 && slot == 3) {
        if (!a[1]) {
            throw GuestFault("missing D3D7 device enumeration callback");
        }

        const uint32_t limit = texture_limit();
        uint32_t data = control_.heap.allocate(512);
        auto store = [&](uint32_t offset, uint32_t value) {
            control_.memory.store32(data + offset, value);
        };
        store(0, 0x741); // Float TL vertices and system/video-memory textures, no hardware claims.
        for (uint32_t base : {4u, 60u}) {
            const uint32_t caps[] = {56, 0x70, 0x30, 0xff, 0x7ff, 0x3ff, 0xff, 0x4208, 5, 0x03000303, 0xcf, 0x1f, 0, 0};
            control_.memory.write(data + base, caps, sizeof caps);
        }

        store(116, 0x100);
        store(120, 0x500); // 32-bit color, Z16 or packed D24S8.
        store(124, 1);
        store(128, 1);
        store(132, limit);
        store(136, limit);
        store(144, limit);
        store(148, 1);
        store(152, 0xbf800000);
        store(156, 0xbf800000);
        store(160, 0x3f800000);
        store(164, 0x3f800000);
        store(172, 0xff); // All eight D3D7 stencil operations.
        store(176, 2);
        store(180, 0xffff);
        store(184, 0x00020002);
        store(188, 8);
        store(192, 0x7f7fffff);
        control_.memory.write(data + 196, rgb_device_iid.data(), rgb_device_iid.size());
        const char name[] = "Sacred Native RGB";
        control_.memory.write(data + 256, name, sizeof name);
        try {
            active_control().invoke(uint32_t(a[1]), {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}, {4}, {4}}, ""},
                                    {data + 256, data + 256, data, a[2]});
        } catch (...) {
            control_.heap.free(data);
            throw;
        }

        control_.heap.free(data);
        return Result{0, {}};
    }

    if (type == Interface::D3d7 && slot == 6) {
        if (guid(control_.memory, uint32_t(a[1])) != rgb_device_iid || !a[2]) {
            throw GuestFault("unqualified Z-buffer enumeration device or callback");
        }

        uint32_t data = control_.heap.allocate(32);
        try {
            const uint32_t formats[][8] = {{32, 0x400, 0, 16, 0, 0xffff, 0, 0},
                                           {32, 0x4400, 0, 32, 8, 0xffffff, 0xff000000, 0}};
            for (const auto& format : formats) {
                control_.memory.write(data, format, sizeof format);
                auto result = active_control().invoke(
                    uint32_t(a[2]), {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}}, ""}, {data, a[3]});
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

    throw GuestFault("unimplemented DirectDraw COM method");
}

void DirectDraw::bind() {
    // The ground mesh is on unless SACRED_GROUND_MESH=0.
    if (const char* ground = std::getenv("SACRED_GROUND_MESH"); !ground || *ground != '0') {
        ground_ = std::make_unique<GroundMesh>(*this);
        ground_mesh_register(ground_.get());
        std::fprintf(stderr, "GROUND_MESH on\n");
    }

    // Granny's skinned meshes deformed in the vertex shader unless SACRED_GPU_SKIN=0 (gpu_skin.cpp).
    if (const char* skin = std::getenv("SACRED_GPU_SKIN"); !skin || *skin != '0') {
        skin_ = std::make_unique<GpuSkin>(control_.memory);
        std::fprintf(stderr, "GPU_SKIN on\n");
    }

    struct Method {
        Interface type;
        unsigned slot;
        const char* name;
        unsigned arguments;
    };
    const Method methods[] = {
#define COM(type, slot, name, arguments) {Interface::type, slot, #name, arguments},
#include "com.def"
#undef COM
    };
    for (const auto& method : methods) {
        const char* names[] = {"IDirectDraw7::",       "IDirect3D7::",  "IDirectDrawSurface7::", "IDirect3DDevice7::",
                               "IDirectDrawClipper::", "IDirectDraw::", "IDirectDrawSurface::"};
        std::string name = names[static_cast<unsigned>(method.type)];
        control_.host(
            com_tokens + static_cast<unsigned>(method.type) * 0x1000 + method.slot * 16,
            {Convention::Stdcall, ReturnChannel::Eax, std::vector<Argument>(method.arguments), ""},
            [this, type = method.type, slot = method.slot](Control& c, const std::vector<uint64_t>& a) {
                call_return_ = c.memory.load32(c.cpu.r[4]);
                ++com_calls_;
                if (type == Interface::Draw7 && slot == 22) {
                    vblank_return_ = call_return_;
                }

                return this->method(type, slot, a);
            },
            name + method.name);
    }

    control_.host(
        loader_.resolve("ddraw.dll", "DirectDrawCreateEx"),
        {Convention::Stdcall,
         ReturnChannel::Eax,
         {{4}, {4, PointerDirection::Out}, {4, PointerDirection::In}, {4}},
         ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            if (a[0] || a[3] || guid(c.memory, uint32_t(a[2])) != draw7_iid) {
                throw GuestFault("unqualified DirectDraw adapter, aggregation or interface");
            }

            c.memory.check(uint32_t(a[1]), 4, Write);
            uint32_t vtable = table(Interface::Draw7);
            uint32_t pointer = c.heap.allocate(4);
            c.memory.store32(pointer, vtable);
            adapters_.emplace(pointer, Adapter{1, pointer, 0});
            c.memory.store32(uint32_t(a[1]), pointer);
            return Result{0, {}};
        },
        "ddraw.dll!DirectDrawCreateEx");
    control_.host(
        loader_.resolve("ddraw.dll", "DirectDrawEnumerateExA"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}, {4}}, "stdcall BOOL(GUID*,char*,char*,void*,HMONITOR)"},
        [](Control& c, const std::vector<uint64_t>& a) {
            if (!a[0] || (a[2] & ~7u)) {
                throw GuestFault("unqualified DirectDraw enumeration");
            }

            // One native renderer adapter. Optional non-display/secondary
            // adapters are not advertised without implemented device support.
            const char description[] = "Sacred Native OpenGL";
            const char driver[] = "display";
            uint32_t strings = c.heap.allocate(sizeof description + sizeof driver);
            c.memory.write(strings, description, sizeof description);
            c.memory.write(strings + sizeof description, driver, sizeof driver);
            try {
                c.invoke(uint32_t(a[0]), {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}, {4}, {4}, {4}}, ""},
                         {0, strings, strings + sizeof description, a[1], 0});
            } catch (...) {
                c.heap.free(strings);
                throw;
            }

            c.heap.free(strings);
            return Result{0, {}};
        },
        "ddraw.dll!DirectDrawEnumerateExA");
}
}
