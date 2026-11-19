#include <ctime>
#include "ddraw.h"
#include "../granny/native.h"
#include "../win32/menu.h"
#include "../../platform/window_host.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace sacred {
namespace {
// DDLOCK_* values (SURFACEMEMORYPTR is 0). EVENT, HASVOLUMETEXTUREBOXRECT and NODIRTYUPDATE stay unsupported.
constexpr uint32_t LockWait = 0x1, LockEvent = 0x2, LockReadOnly = 0x10, LockWriteOnly = 0x20, LockNoSysLock = 0x800,
                   LockNoOverwrite = 0x1000, LockDiscard = 0x2000, LockDoNotWait = 0x4000,
                   LockSupported = LockWait | LockEvent | LockReadOnly | LockWriteOnly | LockNoSysLock |
                                   LockNoOverwrite | LockDiscard | LockDoNotWait;
PixelRect bounds(const PixelImage& image) {
    return {0, 0, int32_t(image.width), int32_t(image.height)};
}

PixelRect rectangle(Memory& memory, uint32_t pointer, PixelRect fallback) {
    if (!pointer) {
        return fallback;
    }

    memory.check(pointer, 16, Read);
    return {int32_t(memory.load32(pointer)), int32_t(memory.load32(pointer + 4)), int32_t(memory.load32(pointer + 8)),
            int32_t(memory.load32(pointer + 12))};
}

PixelRect clipped(PixelRect rect, const PixelImage& image) {
    return {std::max(0, rect.left), std::max(0, rect.top), std::min(int32_t(image.width), rect.right),
            std::min(int32_t(image.height), rect.bottom)};
}

std::array<uint32_t, 8> format(PixelFormat pixel) {
    auto value = describe(pixel);
    return {32, 0x40u | (value.alpha ? 1u : 0u), 0, value.bits, value.red, value.green, value.blue, value.alpha};
}
}

DirectDraw::Surface& DirectDraw::surface(uint32_t pointer) {
    auto found = surfaces_.find(pointer);
    if (found == surfaces_.end()) {
        char text[160];
        auto released = released_.find(pointer);
        if (released != released_.end()) {
            std::snprintf(text, sizeof text,
                          "released surface COM pointer %08x (call from %08x; released by the call "
                          "from %08x, %llu calls earlier)",
                          pointer, call_return_, released->second.caller,
                          (unsigned long long)(com_calls_ - released->second.call));
        } else {
            std::snprintf(text, sizeof text, "invalid surface COM pointer %08x (call from %08x)", pointer,
                          call_return_);
        }

        throw GuestFault(text);
    }

    return found->second;
}

uint32_t DirectDraw::create_surface(uint32_t parent, uint32_t descriptor) {
    auto& owner = adapter(parent);
    auto& memory = control_.memory;
    memory.check(descriptor, 124, Read);
    uint32_t flags = memory.load32(descriptor + 4), caps = memory.load32(descriptor + 104);
    uint32_t caps2 = memory.load32(descriptor + 108);
    if (memory.load32(descriptor) != 124 || !(flags & 1) || (flags & ~0x1007u) ||
        (caps & ~(0x40u | 0x200u | 0x800u | 0x1000u | 0x2000u | 0x4000u | 0x20000u)) || (caps2 & ~0x10u) ||
        memory.load32(descriptor + 112) || memory.load32(descriptor + 116) || !owner.window) {
        throw GuestFault("unqualified color surface description or cooperative window");
    }

    uint32_t width = memory.load32(descriptor + 12), height = memory.load32(descriptor + 8);
    if (caps & 0x200) {
        if (flags & 6) {
            throw GuestFault("unqualified explicit primary dimensions");
        }

        auto size = host_->display_size();
        width = size.first;
        height = size.second;
        caps |= 0x8000;
    } else if ((flags & 6) != 6) {
        throw GuestFault("missing color surface dimensions");
    }

    const uint32_t limit = texture_limit();
    if (!width || !height || width > limit || height > limit) {
        throw GuestFault("color surface exceeds admitted renderer dimensions");
    }

    PixelFormat pixel = PixelFormat::Xrgb8888;
    RenderFormat storage = RenderFormat::Rgba8;
    std::array<uint32_t, 8> depth_pixel{};
    if (caps & 0x20000) {
        if (!(flags & 0x1000)) {
            throw GuestFault("missing depth pixel format");
        }

        for (unsigned i = 0; i < depth_pixel.size(); ++i) {
            depth_pixel[i] = memory.load32(descriptor + 72 + i * 4);
        }

        const std::array<uint32_t, 8> z16{32, 0x400, 0, 16, 0, 0xffff, 0, 0};
        const std::array<uint32_t, 8> d24s8{32, 0x4400, 0, 32, 8, 0xffffff, 0xff000000, 0};
        if (depth_pixel == z16) {
            storage = RenderFormat::Depth16;
            pixel = PixelFormat::Rgb565;
        } else if (depth_pixel == d24s8) {
            storage = RenderFormat::Depth24Stencil8;
        } else {
            std::ostringstream reason;
            reason << std::hex << "unqualified depth format flags=" << depth_pixel[1] << " bits=" << depth_pixel[3]
                   << " stencil=" << depth_pixel[4];
            throw GuestFault(reason.str());
        }
    } else if (flags & 0x1000) {
        bool matched = false;
        for (auto candidate : {PixelFormat::Xrgb8888, PixelFormat::Argb8888, PixelFormat::Rgb565, PixelFormat::Argb1555,
                               PixelFormat::Argb4444}) {
            auto fields = format(candidate);
            bool equal = true;
            for (unsigned i = 0; i < fields.size(); ++i) {
                equal &= fields[i] == memory.load32(descriptor + 72 + i * 4);
            }

            if (equal) {
                pixel = candidate;
                matched = true;
                break;
            }
        }

        if (!matched) {
            throw GuestFault("unqualified color surface pixel format");
        }
    }

    // Charged at the guest's pixel size, as a card holds it. The texture manager (cTextureMgr2, 0x13e7838) keeps
    // its textures under a budget of its own (128 MB) counted the same way; charging its 16-bit textures at the
    // 4 bytes the renderer stores made the adapter run out first after a minute of running through regions, and
    // a texture whose CreateSurface fails keeps the surface pointer it had before its eviction.
    const uint64_t charged =
        uint64_t(width) * height * (storage == RenderFormat::Depth16 ? 2 : describe(pixel).bits / 8);
    if (charged > 256u * 1024 * 1024 - owner.used) {
        std::fprintf(stderr, "DDRAW_OUT_OF_MEMORY %ux%u used_mb=%u surfaces=%zu\n", width, height, owner.used >> 20,
                     surfaces_.size());
        return 0;
    }

    uint32_t pitch = (width * describe(pixel).bits / 8 + 3) & ~3u;
    PixelImage image{width, height, pitch, pixel, std::vector<uint8_t>(size_t(pitch) * height)};
    uint32_t data = control_.heap.allocate(uint32_t(image.pixels.size())), pointer = 0;
    try {
        pointer = control_.heap.allocate(4);
        control_.memory.store32(pointer, table(Interface::Surface7));
        if (caps & 0x4000) {
            caps |= 0x10000000;
        }

        if (caps2 & 0x10) {
            caps |= 0x800;
        }

        Surface value{owner.draw,      1, data, menu_->native_window(owner.window), caps, caps2, uint32_t(charged),
                      std::move(image)};
        value.render_format = storage;
        value.depth_pixel = depth_pixel;
        // Render scale: what the device draws into (the primary surface, 3D render targets and depth buffers) gets
        // k x storage on the GPU, within the renderer's limit (a color target and its depth buffer have one size,
        // so they always get the same factor).
        if (render_scale_ > 1 && (storage != RenderFormat::Rgba8 || (caps & (0x200u | 0x2000u)))) {
            value.scale = render_scale_fit(width, height, render_scale_, limit);
            if (value.scale != render_scale_ && scale_capped_.insert(uint64_t(width) << 32 | height).second) {
                std::fprintf(stderr, "SACRED_RENDER_SCALE %ux%u at %u exceeds the GPU limit %u; using %u\n", width,
                             height, render_scale_, limit, value.scale);
            }
        }

        surfaces_.emplace(pointer, std::move(value));
        released_.erase(pointer);
    } catch (...) {
        if (pointer) {
            control_.heap.free(pointer);
        }

        control_.heap.free(data);
        throw;
    }

    owner.used += uint32_t(charged);
    ++owner.references;
    return pointer;
}

uint32_t DirectDraw::release_surface(uint32_t pointer) {
    auto& value = surface(pointer);
    uint32_t references = --value.references;
    if (references) {
        return references;
    }

    if (value.gpu) {
        host_->retire_surface(value.window, pointer);
    }

    if (value.dc) {
        menu_->release_surface_dc(value.dc);
    }

    uint32_t parent = value.parent, attached = value.attached, clipper = value.clipper;
    adapter(parent).used -= value.charge;
    control_.heap.free(value.data);
    if (value.legacy) {
        surface_aliases_.erase(value.legacy);
        control_.heap.free(value.legacy);
    }

    control_.heap.free(pointer);
    surfaces_.erase(pointer);
    released_[pointer] = Released{call_return_, com_calls_};
    if (attached) {
        release_surface(attached);
    }

    if (clipper) {
        clipper_method(2, {clipper});
    }

    unknown(2, {parent});
    return 0;
}

void DirectDraw::surface_description(uint32_t pointer, uint32_t destination, bool locked) {
    auto& value = surface(pointer);
    auto& memory = control_.memory;
    if (memory.load32(destination) != 124) {
        throw GuestFault("unqualified surface output layout");
    }

    std::array<uint32_t, 31> fields{};
    fields[0] = 124;
    fields[1] = 0x100f | (locked ? 0x800 : 0);
    fields[2] = value.image.height;
    fields[3] = value.image.width;
    fields[4] = value.image.pitch;
    fields[9] = locked ? value.data : 0;
    auto pixel = value.render_format == RenderFormat::Rgba8 ? format(value.image.format) : value.depth_pixel;
    std::copy(pixel.begin(), pixel.end(), fields.begin() + 18);
    fields[26] = value.caps;
    fields[27] = value.caps2;
    memory.write(destination, fields.data(), sizeof fields);
}

void DirectDraw::ensure_gpu(uint32_t pointer) {
    auto& value = surface(pointer);
    if (value.gpu) {
        return;
    }

    auto pixels = value.render_format == RenderFormat::Rgba8 ? unpack(value.image) : std::vector<uint8_t>{};
    // Queued, not waited for: a round trip would wait for all the GL work queued before it (a few ms per new
    // texture while the world streams in). Later commands of this surface queue behind it.
    host_->render_posted(value.window,
                         [pointer, width = value.image.width, height = value.image.height, format = value.render_format,
                          scale = value.scale, pixels = std::move(pixels)](Renderer& renderer) {
                             renderer.create_surface(pointer, width, height, format, scale);
                             if (format == RenderFormat::Rgba8) {
                                 renderer.upload(pointer, pixels);
                             }
                         });
    value.gpu = true;
}

void DirectDraw::sync_surface(uint32_t pointer) {
    auto& value = surface(pointer);
    if (!value.gpu_dirty) {
        return;
    }

    if (value.scale > 1) {
        ++hd_stats_.scaled_reads;
    }

    std::vector<uint8_t> rgba;
    host_->render(value.window, [&](Renderer& renderer) { rgba = renderer.read(pointer); });
    pack(value.image, rgba);
    control_.memory.write(value.data, value.image.pixels.data(), value.image.pixels.size());
    value.gpu_dirty = false;
}

void DirectDraw::upload_surface(uint32_t pointer, PixelRect rect) {
    auto& value = surface(pointer);
    rect = clipped(rect, value.image);
    if (rect.left >= rect.right || rect.top >= rect.bottom) {
        return;
    }

    value.content_hash = 0; // any later write makes the content anonymous again
    value.backdrop_tile = nullptr;
    if (!value.gpu) {
        ensure_gpu(pointer);
        return;
    }

    if (value.replaced) {
        revert_texture(pointer);
        return;
    }

    if (value.scale > 1) {
        ++hd_stats_.scaled_uploads;
    }

    host_->render_posted(value.window, [pointer, rect, rgba = unpack(value.image, rect)](Renderer& renderer) {
        renderer.upload(pointer, rgba, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
    });
}

// Full unlock of a texture: dump it, and put the pack's replacement on the GPU if it is decoded already. A
// replacement still being decoded leaves the guest's pixels up for now; hd_present() applies it as soon as the
// background decoder has it, if the surface still holds that content. True when replaced here.
bool DirectDraw::replace_texture(uint32_t pointer) {
    auto& value = surface(pointer);
    const uint64_t hash = texture_hash(value.image);
    if (dump_) {
        dump_->write(value.image, hash);
    }

    if (!pack_ || !pack_->contains(hash)) {
        return false;
    }

    if (auto replacement = pack_->ready(hash, &value.image)) {
        return apply_replacement(pointer, hash, std::move(replacement));
    }

    pack_->request(hash, &value.image);
    pending_[pointer] = hash; // content_hash is set by the caller after the guest pixels are up
    return false;
}

bool DirectDraw::apply_replacement(uint32_t pointer, uint64_t hash, std::shared_ptr<const TextureReplacement> shared) {
    const TextureReplacement& replacement = *shared;
    auto& value = surface(pointer);
    const unsigned k = texture_scale(value.image.width, value.image.height, replacement.width, replacement.height);
    const uint32_t limit = k ? texture_limit() : 0;
    if (!k || k > 16 || replacement.width > limit || replacement.height > limit) {
        if (rejected_.insert(hash).second) {
            std::fprintf(stderr,
                         "SACRED_TEXTURE_PACK %s: replacement %ux%u is not an integer multiple of %ux%u "
                         "(or exceeds the GPU limit); ignored\n",
                         texture_file_name(value.image, hash).c_str(), replacement.width, replacement.height,
                         value.image.width, value.image.height);
        }

        ++hd_stats_.rejected;
        return false;
    }

    // The game has no alpha channel to show in X8R8G8B8 / R5G6B5 textures: their replacement is opaque too.
    // Queued, not waited for (see ensure_gpu); the replacement's pixels stay shared until the GL thread has them.
    const bool opaque = !describe(value.image.format).alpha;
    host_->render_posted(value.window, [pointer, k, opaque, create = !value.gpu, width = value.image.width,
                                        height = value.image.height, format = value.render_format,
                                        shared = std::move(shared)](Renderer& renderer) {
        if (create) {
            renderer.create_surface(pointer, width, height, format, 1);
        }

        if (!opaque) {
            renderer.replace(pointer, k, shared->rgba);
            return;
        }

        std::vector<uint8_t> forced = shared->rgba;
        for (size_t i = 3; i < forced.size(); i += 4) {
            forced[i] = 255;
        }

        renderer.replace(pointer, k, forced);
    });
    value.gpu = true;
    value.replaced = k;
    value.content_hash = hash;
    ++hd_stats_.replaced;
    return true;
}

void DirectDraw::revert_texture(uint32_t pointer) {
    auto& value = surface(pointer);
    if (!value.replaced) {
        return;
    }

    host_->render_posted(value.window, [pointer, rgba = unpack(value.image)](Renderer& renderer) {
        renderer.replace(pointer, 1, rgba);
    });
    value.replaced = 0;
    ++hd_stats_.reverted;
}

// One stderr line per 300 presents while an HD option is on: the readback cost of the render scale in the frames
// just shown.
// Thread CPU time of the caller in ns.
static uint64_t thread_cpu_ns() {
    timespec t{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return uint64_t(t.tv_sec) * 1000000000ull + uint64_t(t.tv_nsec);
}

void DirectDraw::perf_log() {
    if (perf_log_ < 0) {
        const char* on = std::getenv("SACRED_PERF_LOG");
        perf_log_ = on && *on == '1';
    }

    if (!perf_log_) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const uint64_t cpu = thread_cpu_ns();
    if (!perf_frames_++) {
        perf_start_ = now;
        perf_cpu_ns_ = cpu;
        perf_draws_ = stat_draws_total_;
        return;
    }

    const double seconds = std::chrono::duration<double>(now - perf_start_).count();
    if (seconds < 1.0) {
        return;
    }

    const double frames = double(perf_frames_ - 1);
    const std::time_t wall = std::time(nullptr);
    char clock[16];
    std::strftime(clock, sizeof clock, "%H:%M:%S", std::localtime(&wall));
    std::fprintf(stderr, "PERF %s fps=%.1f game_cpu_ms=%.2f game_busy=%.0f%% draws=%.0f\n", clock, frames / seconds,
                 double(cpu - perf_cpu_ns_) / 1e6 / frames, double(cpu - perf_cpu_ns_) / 1e7 / seconds,
                 double(stat_draws_total_ - perf_draws_) / frames);
    perf_frames_ = 1;
    perf_start_ = now;
    perf_cpu_ns_ = cpu;
    perf_draws_ = stat_draws_total_;
}

void DirectDraw::hd_present() {
    ++presents_;
    if (skin_ && presents_ % 600 == 0) {
        auto& st = skin_->stats;
        std::fprintf(stderr,
                     "GPU_SKIN per frame: %.1f strided draws, %.1f skinned (%.0f vertices); declined %.1f meshes, %.1f "
                     "draws, %.1f materialized, %.1f uploads\n",
                     st.draws / 600.0, st.skinned / 600.0, st.vertices / 600.0, st.declined_mesh / 600.0,
                     st.declined_draw / 600.0, st.materialized / 600.0, st.uploads / 600.0);
        st = {};
        std::fprintf(stderr, "GPU_SKIN deforms per frame: %.1f other, %.1f rendering, %.1f for mesh draws\n",
                     granny_skin_calls[0] / 600.0, granny_skin_calls[1] / 600.0, granny_skin_calls[2] / 600.0);
        granny_skin_calls[0] = granny_skin_calls[1] = granny_skin_calls[2] = 0;
        skin_->report_traps();
    }

    perf_log();
    if (draw_stats_ < 0) {
        const char* stats = std::getenv("SACRED_DRAW_STATS");
        draw_stats_ = stats && *stats == '1';
    }

    if (draw_stats_ && presents_ % 300 == 0) {
        std::fprintf(stderr, "DRAW_STATS per frame: %.0f draw calls, %.0f texture changes, %.0f state changes\n",
                     stat_draws_ / 300.0, stat_texture_runs_ / 300.0, stat_state_runs_ / 300.0);
        stat_draws_ = stat_texture_runs_ = stat_state_runs_ = 0;
    }

    // Replacements the background decoder has finished since the last frame, for surfaces that still hold them.
    for (auto it = pending_.begin(); it != pending_.end();) {
        auto found = surfaces_.find(it->first);
        if (found == surfaces_.end() || found->second.content_hash != it->second || found->second.replaced) {
            it = pending_.erase(it);
            continue;
        }

        auto replacement = pack_->ready(it->second);
        if (!replacement) {
            ++it;
            continue;
        }

        if (apply_replacement(it->first, it->second, std::move(replacement))) {
            ++hd_stats_.deferred;
        }

        it = pending_.erase(it);
    }

    if (render_scale_ <= 1 && !pack_) {
        return;
    }

    if (++hd_presents_ % 300) {
        return;
    }

    const HdStats& last = hd_reported_;
    std::fprintf(stderr,
                 "SACRED_HD presents=%llu scale=%u last300: scaled_reads=%llu scaled_uploads=%llu "
                 "replaced=%llu deferred=%llu rejected=%llu reverted=%llu pending=%zu\n",
                 static_cast<unsigned long long>(hd_presents_), render_scale_,
                 static_cast<unsigned long long>(hd_stats_.scaled_reads - last.scaled_reads),
                 static_cast<unsigned long long>(hd_stats_.scaled_uploads - last.scaled_uploads),
                 static_cast<unsigned long long>(hd_stats_.replaced - last.replaced),
                 static_cast<unsigned long long>(hd_stats_.deferred - last.deferred),
                 static_cast<unsigned long long>(hd_stats_.rejected - last.rejected),
                 static_cast<unsigned long long>(hd_stats_.reverted - last.reverted), pending_.size());
    if (pack_) {
        const auto pack = pack_->stats();
        std::fprintf(stderr, "SACRED_TEXTURE_PACK decoded=%llu avg_ms=%.2f max_ms=%.2f evicted=%llu failed=%llu\n",
                     static_cast<unsigned long long>(pack.decoded),
                     pack.decoded ? double(pack.decode_us) / double(pack.decoded) / 1000 : 0.0,
                     double(pack.max_decode_us) / 1000, static_cast<unsigned long long>(pack.evicted),
                     static_cast<unsigned long long>(pack.failed));
        if (pack.upscaled) {
            std::fprintf(stderr, "SACRED_TEXTURE_UPSCALE upscaled=%llu avg_ms=%.2f max_ms=%.2f\n",
                         static_cast<unsigned long long>(pack.upscaled),
                         double(pack.upscale_us) / double(pack.upscaled) / 1000, double(pack.max_upscale_us) / 1000);
        }
    }

    hd_reported_ = hd_stats_;
}

constexpr int inset_width = 1024, inset_height = 768;
bool DirectDraw::next_inset() {
    // The word is a set of flags the hooks raise (tools/lift/resolution_sites.json): bit 0 raises the inset (the
    // frame's top-left 1024x768); bit 16, raised by a window that is already centred in the frame (the save game
    // window, from the main menu or the game menu), takes the frame's centre instead; bits 8..15 name the screen
    // (inset_screen_). Which presents show it: InsetSchedule.
    bool raised = false;
    inset_raised_ = 0;
    if (inset_word) {
        if (const uint32_t word = control_.memory.load32(inset_word)) {
            control_.memory.store32(inset_word, 0);
            inset_raised_ = word;
            if (word & 0xffffu) {
                raised = true;
                inset_centred_ = word >> 16;
                inset_screen_ = (word >> 8) & 0xffu;
            }
        }
    }

    const InsetSchedule::Present present = inset_schedule_.present(raised);
    inset_fresh_ = present.fresh;
    return present.on;
}

DirectDraw::Inset DirectDraw::inset_view(int32_t width, int32_t height) {
    Inset inset;
    // A client area smaller than the screen shows the frame as it is.
    if (width < inset_width || height < inset_height) {
        return inset;
    }

    inset.on = true;
    // The screen fills the client area's height (its width for an area narrower than 4:3), aspect kept, centred.
    // A window that is already centred in the frame is taken from the frame's centre; the main menu and the
    // character select stand in the middle of the menu canvas (platform/letterbox.h), and without a wide picture
    // (inset_backdrop) only that middle is shown.
    const int extension = menu_canvas_extension(int(configured_width_), int(configured_height_));
    const bool canvas = extension && (inset_screen_ & (inset_screen_main | inset_screen_character)) &&
                        !(inset_screen_ & (inset_screen_options | inset_screen_loading | inset_screen_world_map));
    inset.view = fit_inset(inset_centred_ ? (width - inset_width) / 2
                           : canvas       ? extension
                                          : 0,
                           inset_width, inset_height, width, height);
    inset.canvas = canvas && !inset_centred_ ? extension : 0;
    inset.width = width;
    inset.height = height;
    return inset;
}

// Presents the frame of a width x height client area at (x, y) of the surface, or the inset of it.
void DirectDraw::present_inset(Renderer& renderer, uint32_t pointer, int32_t x, int32_t y, uint32_t width,
                               uint32_t height, const Inset& inset) {
    if (!inset.on) {
        renderer.present(pointer, x, y, width, height);
        return;
    }

    const InsetView& view = inset.view;
    renderer.present(pointer, x + view.source_x, y, uint32_t(view.source_width), uint32_t(view.source_height), width,
                     height, view.box.x, view.box.y, uint32_t(view.box.width), uint32_t(view.box.height),
                     inset.backdrop.id ? &inset.backdrop : nullptr);
}

void DirectDraw::inset_backdrop(uint32_t window, Inset& inset) {
    if (!backdrops_) {
        return;
    }

    const size_t marked = backdrops_->frame;
    backdrops_->frame = 0;
    const std::array<float, 4> shade = backdrops_->shade;
    backdrops_->shade = {0, 0, 0, 1};
    if (!inset.on) {
        backdrop_shown_ = 0;
        return;
    }

    // A frame presented twice (InsetSchedule) marks its first present only: the repeat shows the same image. A screen
    // without recognised background (the world map) shows its own image around it, if there is one.
    size_t index = marked ? marked : !inset_fresh_ ? backdrop_shown_ : 0;
    if (marked || inset_fresh_) {
        backdrop_shade_shown_ = shade;
    }

    if (!index && (inset_screen_ & inset_screen_world_map)) {
        index = size_t(backdrops_->find("worldmap") + 1);
    }

    backdrop_shown_ = index;
    if (!index) {
        return;
    }

    Backdrop& image = backdrops_->at(index - 1);
    if (backdrop_canvas(image)) {
        // The guest drew the whole menu canvas, the picture's foreground included: show all of it.
        if (inset.canvas) {
            inset.view = fit_inset(0, inset_width + 2 * inset.canvas, inset_height, inset.width, inset.height);
        }

        return;
    }

    // The image's centre 1024x768 covers the inset box; its sides reach out from there.
    const InsetView& view = inset.view;
    const double scale = double(view.box.height) / 768.0;
    inset.backdrop.id = backdrop_surface(window, index - 1);
    inset.backdrop.x = int32_t(std::lround(view.box.x - image.centre_left() * scale));
    inset.backdrop.y = view.box.y;
    inset.backdrop.width = int32_t(std::lround(image.logical_width() * scale));
    inset.backdrop.height = view.box.height;
    inset.backdrop.shade = backdrop_shade_shown_;
}

void DirectDraw::backdrop_shade(const void* vertices, const FvfLayout& layout, uint32_t count, uint32_t width,
                                uint32_t height) {
    RenderVertex corners[6];
    decode_fvf(static_cast<const uint8_t*>(vertices), layout, count, corners);
    float left = corners[0].position[0], top = corners[0].position[1], right = left, bottom = top;
    for (uint32_t i = 1; i < count; ++i) {
        const RenderVertex& vertex = corners[i];
        for (int c = 0; c < 4; ++c) {
            if (vertex.diffuse[c] != corners[0].diffuse[c]) {
                return;
            }
        }

        left = std::min(left, vertex.position[0]);
        right = std::max(right, vertex.position[0]);
        top = std::min(top, vertex.position[1]);
        bottom = std::max(bottom, vertex.position[1]);
    }

    if (left > 0 || top > 0 || right < float(width) || bottom < float(height)) {
        return;
    }

    // SRCALPHA / INVSRCALPHA: beneath * (1 - a) + colour * a, on top of what the earlier quads did.
    const float* colour = corners[0].diffuse;
    const float alpha = std::clamp(colour[3], 0.0f, 1.0f);
    std::array<float, 4>& shade = backdrops_->shade;
    for (int c = 0; c < 3; ++c) {
        shade[c] = shade[c] * (1 - alpha) + colour[c] * alpha;
    }

    shade[3] *= 1 - alpha;
}

uint32_t DirectDraw::backdrop_surface(uint32_t window, size_t index) {
    Backdrop& image = backdrops_->at(index);
    if (!image.surface) {
        const uint32_t id = 0xfff00000u + uint32_t(index);
        auto upload = [&](Renderer& renderer) {
            renderer.create_surface(id, image.width, image.height, RenderFormat::Rgba8, 1);
            renderer.upload(id, image.rgba);
        };
        host_->render(window, upload);
        image.surface = id;
    }

    return image.surface;
}

bool DirectDraw::backdrop_canvas(const Backdrop& image) const {
    return image.key.rfind("main-", 0) == 0 || image.key.rfind("charselect-", 0) == 0
               ? menu_canvas_extension(int(configured_width_), int(configured_height_)) > 0
               : false;
}

void DirectDraw::backdrop_substitute(uint32_t window, uint32_t texture, uint32_t coordinate_set, DrawItem& item) {
    const auto found = surfaces_.find(texture);
    if (found == surfaces_.end() || !found->second.backdrop_tile) {
        return;
    }

    const BackdropTile& tile = *found->second.backdrop_tile;
    const int extension = menu_canvas_extension(int(configured_width_), int(configured_height_));
    item.remap.set = coordinate_set;
    if (tile.role == BackdropTile::Sky) {
        // The scrolling sky of the main menu reaches over the whole canvas, its texture continued (it wraps).
        if (!extension) {
            return;
        }

        item.remap.on = true;
        item.remap.edge[0] = item.remap.edge[1] = float(extension);
        return;
    }

    if (tile.role == BackdropTile::Floor || tile.role == BackdropTile::Reflection) {
        // The water grid of the menu canvas is 128x128 cells where the original has 64x64 (resolution_sites.json,
        // FUN_0075e6e0): the floor's coordinates follow the plane position and need nothing; the reflection's are
        // fractions of the grid, (127 g - 32) / 63 back on the original's. With the screen's wide picture both draw
        // from wide textures (wide_water) whose middle half is the original.
        if (!extension) {
            return;
        }

        const int index = tile.backdrop;
        const bool wide = index >= 0 && backdrops_->wide;
        const float to_original = 127.0f / 63.0f, origin = -32.0f / 63.0f;
        item.remap.on = true;
        if (tile.role == BackdropTile::Reflection) {
            item.remap.scale[0] = item.remap.scale[1] = to_original;
            item.remap.offset[0] = item.remap.offset[1] = origin;
        }

        if (wide) {
            const uint32_t surface = wide_water(window, texture, tile);
            if (surface) {
                // Wide texture: original u in [-0.5, 1.5] across, v as it was.
                item.textures[0] = surface;
                item.remap.scale[0] *= 0.5f;
                item.remap.offset[0] = item.remap.offset[0] * 0.5f + 0.25f;
            }
        }

        return;
    }

    if (tile.backdrop < 0) {
        return;
    }

    Backdrop& image = backdrops_->at(size_t(tile.backdrop));
    // The main menu and the character select draw the whole picture on their canvas: the outer tile columns reach
    // out to its edges (as far as the picture does). Other pictures with alpha have no screen to go on.
    const bool canvas = backdrop_canvas(image);
    if (image.alpha && !canvas) {
        return;
    }

    if (canvas) {
        const double reach = std::min(double(extension), std::floor(image.centre_left()));
        if (tile.column == 0) {
            item.remap.edge[0] = float(reach);
        }

        if (tile.column == 3) {
            item.remap.edge[1] = float(reach);
        }
    }

    // The tile's texel (u * 256, v * 256) is the image's screen pixel (column * 256 + u * 256, row * 256 + v * 256)
    // of the centred 1024x768, in image texture coordinates.
    const double logical = image.logical_width();
    item.textures[0] = backdrop_surface(window, size_t(tile.backdrop));
    item.remap.on = true;
    item.remap.scale[0] = float(256.0 / logical);
    item.remap.scale[1] = float(256.0 / 768.0);
    item.remap.offset[0] = float((image.centre_left() + tile.column * 256.0) / logical);
    item.remap.offset[1] = float(tile.row * 256.0 / 768.0);
    if (canvas) {
        // On the canvas the picture is drawn one pixel per canvas pixel: u from the vertex x, the canvas pixel k (its
        // sample point at x = k, Direct3D's pixel centres) on the centre of the picture's pixel k - D + centre_left.
        // The guest's tiles run 257 texels over 256 pixels; continued over a stretched column that rate drifts by a
        // pixel within the first 256, which showed as a doubled or missing column at the old 4:3 border.
        item.remap.screen[0] = float(1.0 / logical);
        item.remap.screen[1] = float((image.centre_left() - extension + 0.5) / logical);
        item.remap.scale[0] = 1;
        item.remap.offset[0] = 0;
    }

    backdrops_->frame = size_t(tile.backdrop) + 1;
}

// The wide water textures of a menu canvas screen with a picture: twice as wide (original u -0.5 .. 1.5), the
// original texture in the middle half (wide_water_texture, menu_backdrops.h).
uint32_t DirectDraw::wide_water(uint32_t window, uint32_t texture, const BackdropTile& tile) {
    const std::string key = tile.screen + (tile.role == BackdropTile::Floor ? "/floor" : "/reflection");
    auto known = backdrops_->wide_water.find(key);
    if (known != backdrops_->wide_water.end()) {
        return known->second;
    }

    const PixelImage& original = surface(texture).image;
    if (original.format != PixelFormat::Argb8888 || !original.width || !original.height) {
        return 0;
    }

    const uint32_t w = original.width, h = original.height;
    const Backdrop* picture = tile.backdrop >= 0 ? &backdrops_->at(size_t(tile.backdrop)) : nullptr;
    const bool character = tile.screen.rfind("charselect-", 0) == 0, floor = tile.role == BackdropTile::Floor;
    const std::vector<uint8_t> pixels = unpack(original, bounds(original));
    // Whether the sides show the picture's painted water is decided on the floor (picture_shows_water); a
    // reflection drawn before its floor waits a frame for it.
    auto decided = backdrops_->painted_water.find(tile.screen);
    if (floor && decided == backdrops_->painted_water.end()) {
        decided = backdrops_->painted_water
                      .emplace(tile.screen, picture && picture_shows_water(*picture, pixels, w, h, character))
                      .first;
    }

    if (decided == backdrops_->painted_water.end()) {
        return 0;
    }

    const std::vector<uint8_t> rgba = wide_water_texture(pixels, w, h, floor, picture, character, decided->second);
    const uint32_t id = 0xfff10000u + uint32_t(backdrops_->wide_water.size());
    host_->render(window, [&](Renderer& renderer) {
        renderer.create_surface(id, 2 * w, h, RenderFormat::Rgba8, 1);
        renderer.upload(id, rgba);
    });
    backdrops_->wide_water[key] = id;
    return id;
}

// The word after the inset word tells the guest whether the last present showed an inset (1), one of the menu
// canvas (3: main menu, character select) or none (0): 3D scene draws map their projection into the inset only then,
// message boxes are centred on the canvas (tools/lift/resolution_sites.json, FUN_0075ed40, FUN_00722500).
void DirectDraw::shown_inset(const Inset& inset) {
    if (inset_word) {
        control_.memory.store32(inset_word + 4, inset.on ? inset.canvas ? 3u : 1u : 0u);
    }
}

void DirectDraw::trace_inset(const char* kind, const Inset& inset) {
    ++traced_;
    if (!trace_present) {
        scenes_ = 0;
        return;
    }

    char line[224];
    std::snprintf(line, sizeof line,
                  "INSET_TRACE %llu %s raised=%#x on=%d fresh=%d screen=%#x centred=%d canvas=%d "
                  "backdrop=%zu scenes=%u",
                  static_cast<unsigned long long>(traced_), kind, inset_raised_, int(inset.on), int(inset_fresh_),
                  inset_screen_, int(inset_centred_), int(inset.canvas), backdrop_shown_, scenes_);
    scenes_ = 0;
    inset_raised_ = 0;
    trace_present(line);
}

void DirectDraw::present_surface(uint32_t pointer, const char* kind) {
    auto& value = surface(pointer);
    if (!(value.caps & 0x200)) {
        return;
    }

    ensure_gpu(pointer);
    hd_present();
    if (observe_present) {
        observe_present(value.window);
    }

    // As in the GPU blit's present: only an inset needs the guest client size here; the present itself is queued,
    // since any round trip to the host would wait for the whole frame's GL work.
    Inset inset;
    if (next_inset()) {
        const auto window = host_->window_state(value.window);
        inset = inset_view(window.width, window.height);
    }

    inset_backdrop(value.window, inset);
    host_->render_posted(value.window, [host = host_, window = value.window, pointer, inset](Renderer& renderer) {
        const auto state = host->host_window_state(window);
        present_inset(renderer, pointer, state.x, state.y, uint32_t(state.width), uint32_t(state.height), inset);
    });
    shown_inset(inset);
    if (observe_inset) {
        observe_inset(inset.on, inset.view);
    }

    trace_inset(kind, inset);
}

// A blit whose source has its newest pixels on the GPU (a render target after drawing: the frame the game copies
// to the primary surface) stays on the GPU. The read-back into guest memory, the copy there and the upload of the
// result are skipped; the destination becomes GPU-newest and is read back only if the guest locks it. The copy
// and the presentation of a primary surface are queued behind the frame's draw calls and the guest does not wait
// for them, so the clipper's window rectangle is read on the host thread. Returns false for everything else (other
// pixel formats, scaling, CPU-side sources, invalid rectangles): the caller copies in guest memory as before.
bool DirectDraw::gpu_blit(uint32_t pointer, PixelRect target, uint32_t source_pointer, PixelRect area,
                          uint32_t clipper) {
    auto& value = surface(pointer);
    auto& source = surface(source_pointer);
    const auto format = value.image.format;
    if (!source.gpu || !source.gpu_dirty || source_pointer == pointer || source.window != value.window ||
        source.image.format != format || (format != PixelFormat::Xrgb8888 && format != PixelFormat::Argb8888)) {
        return false;
    }

    if (target.left >= target.right || target.top >= target.bottom || area.left < 0 || area.top < 0 ||
        area.left >= area.right || area.top >= area.bottom || uint32_t(area.right) > source.image.width ||
        uint32_t(area.bottom) > source.image.height ||
        int64_t(target.right) - target.left != int64_t(area.right) - area.left ||
        int64_t(target.bottom) - target.top != int64_t(area.bottom) - area.top) {
        return false;
    }

    const uint32_t clip_window = clipper ? clipper_window(clipper) : 0;
    ensure_gpu(pointer);
    revert_texture(pointer); // the GPU now writes into it: the guest-size storage holds the truth again
    value.content_hash = 0;
    value.backdrop_tile = nullptr;
    const bool primary = value.caps & 0x200;
    if (primary) {
        hd_present();
    }

    if (primary && observe_present) {
        observe_present(value.window);
    }

    // An inset screen needs the guest client size for its offset; ordinary frames do not wait for the host.
    Inset inset;
    if (primary && next_inset()) {
        const auto window = host_->window_state(value.window);
        inset = inset_view(window.width, window.height);
    }

    if (primary) {
        inset_backdrop(value.window, inset);
    }

    host_->render_posted(value.window, [host = host_, window = value.window, pointer, source_pointer, target, area,
                                        clip_window, limit = bounds(value.image), primary, inset,
                                        opaque = format == PixelFormat::Xrgb8888](Renderer& renderer) {
        PixelRect visible = {std::max(target.left, limit.left), std::max(target.top, limit.top),
                             std::min(target.right, limit.right), std::min(target.bottom, limit.bottom)};
        if (clip_window) {
            const auto state = host->host_window_state(clip_window);
            visible = {std::max(visible.left, state.x), std::max(visible.top, state.y),
                       std::min(visible.right, state.x + state.width),
                       std::min(visible.bottom, state.y + state.height)};
        }

        if (visible.left < visible.right && visible.top < visible.bottom) {
            renderer.copy(pointer, visible.left, visible.top, source_pointer, area.left + (visible.left - target.left),
                          area.top + (visible.top - target.top), uint32_t(visible.right - visible.left),
                          uint32_t(visible.bottom - visible.top), opaque);
        }

        if (primary) {
            const auto state = host->host_window_state(window);
            present_inset(renderer, pointer, state.x, state.y, uint32_t(state.width), uint32_t(state.height), inset);
        }
    });
    if (primary) {
        shown_inset(inset);
    }

    if (primary && observe_inset) {
        observe_inset(inset.on, inset.view);
    }

    if (primary) {
        trace_inset("blt-gpu", inset);
    }

    value.gpu_dirty = true;
    return true;
}

Result DirectDraw::surface_method(unsigned slot, const std::vector<uint64_t>& a) {
    uint32_t pointer = uint32_t(a[0]);
    auto alias = surface_aliases_.find(pointer);
    if (alias != surface_aliases_.end()) {
        pointer = alias->second;
    }

    auto& value = surface(pointer);
    auto& memory = control_.memory;
    // Blit sources may be legacy IDirectDrawSurface pointers (the DirectShow frame surface).
    auto resolve = [this](uint64_t source) {
        auto found = surface_aliases_.find(uint32_t(source));
        return found == surface_aliases_.end() ? uint32_t(source) : found->second;
    };
    if (slot == 24) {
        return Result{0, {}}; // IsLost: GL-backed surfaces survive focus and mode changes.
    }

    if (slot == 0) {
        const std::array<uint8_t, 16> own{0x80, 0x5a, 0x67, 6,    0x9b, 0x3b, 0xd2, 0x11,
                                          0xb9, 0x2f, 0,    0x60, 0x97, 0x97, 0xea, 0x5b};
        const std::array<uint8_t, 16> identity{0, 0, 0, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46};
        const std::array<uint8_t, 16> legacy{0x81, 0xdb, 0x14, 0x6c, 0x33, 0xa7, 0xce, 0x11,
                                             0xa5, 0x21, 0,    0x20, 0xaf, 0x0b, 0xe5, 0x60};
        std::array<uint8_t, 16> iid{};
        memory.read(uint32_t(a[1]), iid.data(), iid.size());
        bool known = iid == own || iid == identity || iid == legacy;
        uint32_t result = known ? pointer : 0;
        if (iid == legacy) {
            if (!value.legacy) {
                value.legacy = control_.heap.allocate(4);
                memory.store32(value.legacy, table(Interface::Surface1));
                surface_aliases_.emplace(value.legacy, pointer);
            }

            result = value.legacy;
        }

        memory.store32(uint32_t(a[2]), result);
        if (known) {
            ++value.references;
        }

        return Result{known ? 0u : 0x80004002u, {}};
    }

    if (slot == 1) {
        return Result{++value.references, {}};
    }

    if (slot == 2) {
        return Result{release_surface(pointer), {}};
    }

    if (slot == 3) {
        auto& attached = surface(uint32_t(a[1]));
        if (value.render_format != RenderFormat::Rgba8 || attached.render_format == RenderFormat::Rgba8 ||
            value.parent != attached.parent || value.image.width != attached.image.width ||
            value.image.height != attached.image.height) {
            throw GuestFault("unqualified surface attachment");
        }

        if (value.attached) {
            return Result{0x8876019a, {}};
        }

        ensure_gpu(pointer);
        ensure_gpu(uint32_t(a[1]));
        host_->render(value.window, [&](Renderer& renderer) { renderer.attach_depth(pointer, uint32_t(a[1])); });
        value.attached = uint32_t(a[1]);
        ++attached.references;
        return Result{0, {}};
    }

    if (slot == 12) {
        memory.check(uint32_t(a[1]), 16, Read);
        for (unsigned i = 1; i < 4; ++i) {
            if (memory.load32(uint32_t(a[1]) + i * 4)) {
                throw GuestFault("unqualified attached-surface capabilities");
            }
        }

        uint32_t requested = memory.load32(uint32_t(a[1]));
        if (value.attached && (surface(value.attached).caps & requested) == requested) {
            memory.store32(uint32_t(a[2]), value.attached);
            ++surface(value.attached).references;
            return Result{0, {}};
        }

        memory.store32(uint32_t(a[2]), 0);
        return Result{0x887600ff, {}};
    }

    if (slot == 22) {
        surface_description(pointer, uint32_t(a[1]), false);
        return Result{0, {}};
    }

    if (slot == 17) {
        if (value.render_format != RenderFormat::Rgba8 || value.image.format != PixelFormat::Xrgb8888) {
            throw GuestFault("unqualified surface DC pixel format");
        }

        if (value.locked) {
            return Result{0x887601ae, {}};
        }

        memory.check(uint32_t(a[1]), 4, Write);
        sync_surface(pointer);
        uint32_t dc = menu_->surface_dc(value.data, value.image.width, value.image.height, value.image.pitch);
        memory.store32(uint32_t(a[1]), dc);
        value.dc = dc;
        value.locked = true;
        return Result{0, {}};
    }

    if (slot == 26) {
        if (!value.dc || a[1] != value.dc) {
            throw GuestFault("invalid surface DC release");
        }

        if (!(value.caps & 0x200)) {
            inset_schedule_.drew();
        }

        menu_->release_surface_dc(value.dc);
        value.dc = 0;
        value.locked = false;
        memory.read(value.data, value.image.pixels.data(), value.image.pixels.size());
        upload_surface(pointer, bounds(value.image));
        present_surface(pointer, "release-dc");
        return Result{0, {}};
    }

    if (slot == 28) {
        uint32_t clipper = uint32_t(a[1]), old = value.clipper;
        if (clipper != old) {
            if (clipper) {
                clipper_method(1, {clipper});
            }

            value.clipper = clipper;
            if (old) {
                clipper_method(2, {old});
            }
        }

        return Result{0, {}};
    }

    if (slot == 25) {
        if (value.render_format != RenderFormat::Rgba8) {
            throw GuestFault("unqualified depth lock/readback");
        }

        // Lock is synchronous, so WAIT, DONOTWAIT, NOSYSLOCK and NOOVERWRITE need no extra work.
        const uint32_t flags = uint32_t(a[3]);
        if ((flags & ~LockSupported) || (flags & LockEvent) || a[4]) {
            throw GuestFault("unqualified surface lock flags or event");
        }

        if ((flags & LockReadOnly) && (flags & LockWriteOnly)) {
            return Result{0x80070057, {}};
        }

        PixelRect area = rectangle(memory, uint32_t(a[1]), bounds(value.image));
        if (area.left < 0 || area.top < 0 || area.left >= area.right || area.top >= area.bottom ||
            uint32_t(area.right) > value.image.width || uint32_t(area.bottom) > value.image.height) {
            return Result{0x88760096, {}};
        }

        if (value.locked) {
            return Result{0x887601ae, {}};
        }

        if (!(flags & LockDiscard)) {
            sync_surface(pointer);
        }

        surface_description(pointer, uint32_t(a[2]), true);
        memory.store32(uint32_t(a[2]) + 36, value.data + uint32_t(area.top) * value.image.pitch +
                                                uint32_t(area.left) * (describe(value.image.format).bits / 8));
        value.locked = true;
        value.lock_rect = area;
        value.lock_flags = flags;
        return Result{0, {}};
    }

    if (slot == 32) {
        if (value.dc) {
            throw GuestFault("surface DC requires ReleaseDC");
        }

        if (a[1]) {
            memory.check(uint32_t(a[1]), 16, Read); // DirectDraw7 ignores the Unlock rectangle.
        }

        if (!value.locked) {
            return Result{0x88760248, {}};
        }

        value.locked = false;
        const PixelRect area = value.lock_rect;
        const size_t bytes = describe(value.image.format).bits / 8, row = size_t(area.right - area.left) * bytes;
        const bool readonly = value.lock_flags & LockReadOnly;
        for (int32_t y = area.top; y < area.bottom; ++y) {
            size_t at = size_t(y) * value.image.pitch + size_t(area.left) * bytes;
            if (readonly) {
                memory.write(value.data + uint32_t(at), value.image.pixels.data() + at, row);
            } else {
                memory.read(value.data + uint32_t(at), value.image.pixels.data() + at, row);
            }
        }

        if (readonly) {
            return Result{0, {}}; // guest writes under READONLY are dropped
        }

        if (!(value.caps & (0x200 | 0x1000))) {
            inset_schedule_.drew(); // a frame drawn on the CPU, not a texture
        }

        // Texture pack / dump: a texture filled in one piece is identified by its content.
        if ((value.caps & 0x1000) && (pack_ || dump_ || backdrops_) && area.left == 0 && area.top == 0 &&
            uint32_t(area.right) == value.image.width && uint32_t(area.bottom) == value.image.height) {
            // Wide screen images and the menu canvas: background tiles (256x256), the sky (1024x256) and the water
            // textures (1024x1024), all A8R8G8B8. The role goes with the content whether or not the pack replaces
            // it: a replacement already decoded (every load after the first of the same texture, e.g. the menu
            // after a game, or the game menu's options after the main menu's) is put up right here.
            const uint32_t tw = value.image.width, th = value.image.height;
            const BackdropTile* role = nullptr;
            if (backdrops_ && value.image.format == PixelFormat::Argb8888 && value.image.pitch == tw * 4 &&
                ((tw == 256 && th == 256) || (tw == 1024 && (th == 256 || th == 1024))) &&
                backdrops_->candidate(texture_prefix_hash(value.image.pixels.data(), value.image.pixels.size()))) {
                role = backdrops_->tile(texture_hash(value.image));
            }

            if ((pack_ || dump_) && replace_texture(pointer)) {
                value.backdrop_tile = role;
                return Result{0, {}};
            }

            upload_surface(pointer, area);
            auto waiting = pending_.find(pointer);
            if (waiting != pending_.end()) {
                value.content_hash = waiting->second;
            }

            value.backdrop_tile = role;
            return Result{0, {}};
        }

        upload_surface(pointer, area);
        present_surface(pointer, "unlock");
        return Result{0, {}};
    }

    if (slot == 5 || slot == 7) {
        if (value.render_format != RenderFormat::Rgba8) {
            throw GuestFault("unqualified depth blit");
        }

        if (value.locked) {
            return Result{0x887601ae, {}};
        }

        if (!(value.caps & 0x200)) {
            inset_schedule_.drew();
        }

        PixelRect target{};
        bool on_gpu = false;
        if (slot == 5) {
            target = rectangle(memory, uint32_t(a[1]), bounds(value.image));
            if (a[4] & 0x400) {
                if (a[2] || a[3] || (a[4] & ~0x01000400u) || memory.load32(uint32_t(a[5])) != 100) {
                    throw GuestFault("unqualified surface color-fill operation");
                }

                sync_surface(pointer);
                auto clip = value.clipper ? clip_region(value.clipper) : bounds(value.image);
                fill(value.image,
                     {std::max(target.left, clip.left), std::max(target.top, clip.top),
                      std::min(target.right, clip.right), std::min(target.bottom, clip.bottom)},
                     memory.load32(uint32_t(a[5]) + 80));
            } else {
                if (!a[2] || (a[4] & ~0x01000000u) || a[5]) {
                    throw GuestFault("unqualified surface copy operation");
                }

                const uint32_t source_pointer = resolve(a[2]);
                auto& source = surface(source_pointer);
                if (source.render_format != RenderFormat::Rgba8) {
                    throw GuestFault("unqualified depth blit source");
                }

                if (source.locked) {
                    return Result{0x887601ae, {}};
                }

                const auto area = rectangle(memory, uint32_t(a[3]), bounds(source.image));
                on_gpu = gpu_blit(pointer, target, source_pointer, area, value.clipper);
                if (!on_gpu) {
                    sync_surface(pointer);
                    sync_surface(source_pointer);
                    blit(value.image, target, source.image, area,
                         value.clipper ? clip_region(value.clipper) : bounds(value.image));
                }
            }
        } else {
            if (value.clipper) {
                throw GuestFault("unqualified BltFast with clipper");
            }

            if (a[5] & ~0x10u) {
                throw GuestFault("unqualified BltFast flags");
            }

            const uint32_t source_pointer = resolve(a[3]);
            auto& source = surface(source_pointer);
            if (source.render_format != RenderFormat::Rgba8) {
                throw GuestFault("unqualified depth blit source");
            }

            if (source.locked) {
                return Result{0x887601ae, {}};
            }

            auto src = rectangle(memory, uint32_t(a[4]), bounds(source.image));
            int64_t width = int64_t(src.right) - src.left, height = int64_t(src.bottom) - src.top;
            if (width <= 0 || height <= 0 || a[1] + uint64_t(width) > value.image.width ||
                a[2] + uint64_t(height) > value.image.height) {
                throw GuestFault("unqualified BltFast destination rectangle");
            }

            target = {int32_t(a[1]), int32_t(a[2]), int32_t(a[1] + width), int32_t(a[2] + height)};
            on_gpu = gpu_blit(pointer, target, source_pointer, src, 0);
            if (!on_gpu) {
                sync_surface(pointer);
                sync_surface(source_pointer);
                blit(value.image, target, source.image, src, bounds(value.image));
            }
        }

        if (!on_gpu) {
            memory.write(value.data, value.image.pixels.data(), value.image.pixels.size());
            upload_surface(pointer, target);
            present_surface(pointer, "blt-cpu");
        }

        return Result{0, {}};
    }

    throw GuestFault("unimplemented surface COM method");
}

uint32_t DirectDraw::movie_surface_create(uint32_t width, uint32_t height) {
    if (adapters_.empty()) {
        throw GuestFault("movie surface requires a DirectDraw adapter");
    }

    uint32_t descriptor = control_.heap.allocate(124), pointer = 0;
    std::array<uint32_t, 31> fields{};
    fields[0] = 124;
    fields[1] = 7;
    fields[2] = height;
    fields[3] = width;
    fields[26] = 0x40 | 0x800; // DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY
    control_.memory.write(descriptor, fields.data(), sizeof fields);
    try {
        pointer = create_surface(adapters_.begin()->second.draw, descriptor);
    } catch (...) {
        control_.heap.free(descriptor);
        throw;
    }

    control_.heap.free(descriptor);
    if (!pointer) {
        throw GuestFault("movie surface exceeds the adapter memory budget");
    }

    auto& value = surface(pointer);
    if (!value.legacy) {
        value.legacy = control_.heap.allocate(4);
        control_.memory.store32(value.legacy, table(Interface::Surface1));
        surface_aliases_.emplace(value.legacy, pointer);
    }

    return value.legacy; // the creation reference belongs to the caller
}

void DirectDraw::movie_surface_retain(uint32_t pointer) {
    auto alias = surface_aliases_.find(pointer);
    ++surface(alias == surface_aliases_.end() ? pointer : alias->second).references;
}

void DirectDraw::movie_surface_release(uint32_t pointer) {
    auto alias = surface_aliases_.find(pointer);
    release_surface(alias == surface_aliases_.end() ? pointer : alias->second);
}

void DirectDraw::movie_surface_write(uint32_t pointer, PixelRect area, const uint8_t* bgra, size_t stride) {
    auto alias = surface_aliases_.find(pointer);
    if (alias != surface_aliases_.end()) {
        pointer = alias->second;
    }

    auto& value = surface(pointer);
    if (value.render_format != RenderFormat::Rgba8 ||
        (value.image.format != PixelFormat::Xrgb8888 && value.image.format != PixelFormat::Argb8888)) {
        throw GuestFault("unqualified movie surface pixel format");
    }

    if (value.locked) {
        throw GuestFault("movie frame for a locked surface");
    }

    inset_schedule_.drew();
    if (area.left < 0 || area.top < 0 || area.left >= area.right || area.top >= area.bottom ||
        uint32_t(area.right) > value.image.width || uint32_t(area.bottom) > value.image.height) {
        throw GuestFault("movie sample rectangle outside its surface");
    }

    sync_surface(pointer);
    const size_t row = size_t(area.right - area.left) * 4;
    for (int32_t y = area.top; y < area.bottom; ++y) {
        size_t at = size_t(y) * value.image.pitch + size_t(area.left) * 4;
        const uint8_t* source = bgra + size_t(y - area.top) * stride;
        std::copy(source, source + row, value.image.pixels.begin() + std::ptrdiff_t(at));
        control_.memory.write(value.data + uint32_t(at), source, row);
    }

    upload_surface(pointer, area);
}

uint32_t DirectDraw::clipper_window(uint32_t pointer) {
    auto found = clippers_.find(pointer);
    if (found == clippers_.end()) {
        throw GuestFault("invalid or released clipper COM pointer");
    }

    if (!found->second.window) {
        throw GuestFault("clipper has no qualified window region");
    }

    return menu_->native_window(found->second.window);
}

PixelRect DirectDraw::clip_region(uint32_t pointer) {
    auto state = host_->window_state(clipper_window(pointer));
    return {state.x, state.y, state.x + int32_t(state.width), state.y + int32_t(state.height)};
}

Result DirectDraw::clipper_method(unsigned slot, const std::vector<uint64_t>& a) {
    auto found = clippers_.find(uint32_t(a[0]));
    if (found == clippers_.end()) {
        throw GuestFault("invalid or released clipper COM pointer");
    }

    auto& clipper = found->second;
    if (slot == 1) {
        return Result{++clipper.references, {}};
    }

    if (slot == 2) {
        uint32_t references = --clipper.references;
        if (!references) {
            control_.heap.free(found->first);
            clippers_.erase(found);
        }

        return Result{references, {}};
    }

    if (slot == 4) {
        control_.memory.store32(uint32_t(a[1]), clipper.window);
        return Result{0, {}};
    }

    if (slot == 8) {
        if (a[1]) {
            throw GuestFault("unqualified clipper window flags");
        }

        if (a[2]) {
            menu_->native_window(uint32_t(a[2]));
        }

        clipper.window = uint32_t(a[2]);
        return Result{0, {}};
    }

    throw GuestFault("unimplemented clipper COM method");
}
}
