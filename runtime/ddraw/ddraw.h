#pragma once
#include <chrono>
#include "../process/control.h"
#include <unordered_map>
#include "surface_pixels.h"
#include "vertices.h"
#include "texture_pack.h"
#include "menu_backdrops.h"
#include "ground_mesh.h"
#include "gpu_skin.h"
#include "inset_schedule.h"
#include "../../platform/renderer.h"
#include "../../platform/frame_clock.h"
#include "../../platform/letterbox.h"

namespace sacred {
class Menu;
class WindowHost;
class Renderer;
class GroundMesh;
// Guest COM pointers and vtables never contain native pointers.
class DirectDraw {
public:
    DirectDraw(Control& control, const Loader& loader, Menu* menu = nullptr, WindowHost* host = nullptr)
        : control_(control), loader_(loader), menu_(menu), host_(host) {}
    ~DirectDraw();
    void bind();
    std::function<void(uint32_t)> observe_present;
    // Frame rate limit (Settings.cfg FPS_LIMIT): the vertical blank the game waits on before a flip ticks at this
    // rate; the advertised display mode stays 60 Hz. The wait of a fade step (dxDriver7::flip, return address
    // 0x645ea8) keeps the 60 Hz clock, so fades last as long as in the original.
    void scanout_rate(uint32_t hertz) { frame_clock_.scanout_rate(hertz); }
    // Settings.cfg has no WAITRETRACE: the flip's vertical-blank waits (the game's default 1) return at once, as
    // with WAITRETRACE 0. The wait is a clock of FPS_LIMIT ticks, not the display's, so it only rounds every frame
    // up to the next tick (an 18 ms frame zoomed out becomes 25 ms); the game's own limiter still caps the rate.
    bool frame_vblank_default_off = false;
    // Opt-in screen mode: a guest word that hooks raise while a screen designed for 1024x768 draws. A present that
    // finds it raised shows only the 1024x768 corner, centred in the frame, clears the word and reports the offset
    // the mouse must be moved by (0 for an ordinary frame); a repeat present of that frame shows it too
    // (InsetSchedule).
    uint32_t inset_word = 0;
    // (on, view): see Menu::set_pointer_inset.
    std::function<void(bool, const InsetView&)> observe_inset;
    // Diagnostics (SACRED_INSET_TRACE): one line per present of the primary surface, how it showed the frame.
    std::function<void(const std::string&)> trace_present;
    // An opt-in game screen size (HD mode) enumerated after the original 1024x768 mode; 0 x 0 for none.
    void configured_mode(uint32_t width, uint32_t height) {
        configured_width_ = width;
        configured_height_ = height;
    }

    // Internal render scale (RENDER_SCALE / SACRED_RENDER_SCALE, 1 = off): render targets, the primary surface and
    // depth buffers are k x their guest size on the GPU only; the guest keeps seeing its own sizes. Set before the
    // first surface is created.
    void render_scale(uint32_t k) { render_scale_ = k ? k : 1; }
    // Opt-in texture replacement and dump (texture_pack.h); null / empty for none.
    void texture_pack(std::shared_ptr<TexturePack> pack) { pack_ = std::move(pack); }
    // Opt-in wide backgrounds of the HD mode's 1024x768 screens (menu_backdrops.h); null for none.
    void menu_backdrops(std::shared_ptr<MenuBackdrops> backdrops) { backdrops_ = std::move(backdrops); }
    void texture_dump(const std::string& directory) {
        dump_ = directory.empty() ? nullptr : std::make_unique<TextureDump>(directory);
    }

    // Counts for the render-scale report: guest reads of a scaled surface (each one a GPU downsample), guest
    // uploads into one (each one a GPU magnification) and textures replaced from the pack.
    struct HdStats {
        uint64_t scaled_reads = 0, scaled_uploads = 0, replaced = 0, deferred = 0, rejected = 0, reverted = 0;
    };
    const HdStats& hd_stats() const { return hd_stats_; }
    // The menu canvas / wide image role of a texture's current content (menu_backdrops.h), or null.
    const BackdropTile* texture_backdrop(uint32_t pointer) const {
        const auto found = surfaces_.find(pointer);
        return found == surfaces_.end() ? nullptr : found->second.backdrop_tile;
    }

    // DirectShow movie access (runtime/ole). `surface` is a guest IDirectDrawSurface/Surface7 pointer.
    // Stream-owned frame surface (the game passes no surface to CreateSample): returns its IDirectDrawSurface pointer.
    uint32_t movie_surface_create(uint32_t width, uint32_t height);
    void movie_surface_retain(uint32_t surface);
    void movie_surface_release(uint32_t surface);
    // Copies a decoded B,G,R,A frame (exactly `area` in size, `stride` bytes per row) into an X8R8G8B8/A8R8G8B8
    // surface.
    void movie_surface_write(uint32_t surface, PixelRect area, const uint8_t* bgra, size_t stride);

private:
    friend class GroundMesh;
    std::unique_ptr<GroundMesh> ground_; // SACRED_GROUND_MESH (ground_mesh.cpp), null when off
    std::unique_ptr<GpuSkin> skin_;      // SACRED_GPU_SKIN (gpu_skin.cpp), null when off
    GpuSkin::Draw skin_draw_;            // the current strided draw's (reused)
    uint64_t presents_ = 0;              // presents of the primary surface (hd_present)
    // SACRED_DRAW_STATS=1: draw calls and batches per frame, every 300 presents.
    int draw_stats_ = -1;
    // SACRED_PERF_LOG=1: once a second a PERF line (local time, FPS, game-thread CPU per frame, draw calls).
    void perf_log();
    int perf_log_ = -1;
    uint64_t stat_draws_total_ = 0;
    uint64_t perf_frames_ = 0, perf_draws_ = 0, perf_cpu_ns_ = 0;
    std::chrono::steady_clock::time_point perf_start_{};
    uint64_t stat_draws_ = 0, stat_texture_runs_ = 0, stat_state_runs_ = 0;
    uint32_t stat_last_texture_ = 0;
    InsetSchedule inset_schedule_; // which presents show the raised inset
    uint32_t inset_raised_ = 0;    // the word as the last next_inset found it (trace_present)
    uint32_t scenes_ = 0;          // BeginScene calls since the last present (trace_present)
    uint64_t traced_ = 0;          // presents of the primary surface so far (trace_present)
    bool inset_centred_ = false;   // that inset takes the frame's centre (see next_inset)
    bool inset_fresh_ = false;     // the last present found the word raised (not the repeat present of a frame)
    uint32_t inset_screen_ = 0;    // screen flags of the last raise (bits 8..15 of the word, shifted down)
    static constexpr uint32_t inset_screen_world_map = 1, inset_screen_options = 2, inset_screen_character = 4,
                              inset_screen_main = 8, inset_screen_loading = 0x10;
    Control& control_;
    const Loader& loader_;
    Menu* menu_;
    WindowHost* host_;
    FrameClock frame_clock_;
    FrameClock fade_clock_;
    uint32_t vblank_return_ = 0; // guest return address of the WaitForVerticalBlank being served
    uint32_t call_return_ = 0;   // guest return address of the COM call being served
    uint64_t com_calls_ = 0;     // COM calls served so far
    // Surfaces freed by their last Release (pointer -> return address of that Release, com_calls_ then), kept until
    // the pointer is allocated again, so a fault on a stale pointer can say where it was released.
    struct Released {
        uint32_t caller;
        uint64_t call;
    };
    std::unordered_map<uint32_t, Released> released_;
    uint32_t configured_width_ = 0, configured_height_ = 0;
    uint32_t render_scale_ = 1;
    std::shared_ptr<TexturePack> pack_;
    std::shared_ptr<MenuBackdrops> backdrops_;
    size_t backdrop_shown_ = 0; // image of the last present that showed one (index + 1), for its repeat present
    // The renderer surface of a backdrop image, uploaded on first use.
    uint32_t backdrop_surface(uint32_t window, size_t index);
    // A draw call whose first texture is a background tile of a screen with an opaque wide image draws that tile
    // from the image instead (menu_backdrops.h).
    // (Tiles, the sky and the water of the menu canvas too: menu_backdrops.h BackdropTile.)
    void backdrop_substitute(uint32_t window, uint32_t texture, uint32_t coordinate_set, DrawItem& item);
    uint32_t wide_water(uint32_t window, uint32_t texture, const BackdropTile& tile);
    // A translucent untextured quad of `count` raw vertices: when it covers the whole width x height target in one
    // colour, folds it into the frame's shade of the picture's sides (MenuBackdrops::shade).
    void backdrop_shade(const void* vertices, const FvfLayout& layout, uint32_t count, uint32_t width, uint32_t height);
    std::array<float, 4> backdrop_shade_shown_{0, 0, 0, 1}; // the shade of the last present, for its repeat present
    std::unique_ptr<TextureDump> dump_;
    HdStats hd_stats_, hd_reported_;
    std::map<uint32_t, uint64_t> pending_; // texture surface -> replacement hash the decoder is working on
    std::set<uint64_t> rejected_;          // replacements of the wrong size, logged once
    std::set<uint64_t> scale_capped_;      // surface sizes whose render scale the GPU limit lowered, logged once
    uint64_t hd_presents_ = 0;
    enum class Interface : unsigned { Draw7, D3d7, Surface7, Device7, Clipper, Draw1, Surface1 };
    struct Adapter {
        uint32_t references, draw, d3d;
        uint32_t draw1 = 0, window = 0, used = 0;
    };
    std::map<uint32_t, Adapter> adapters_;
    struct Surface {
        uint32_t parent, references, data, window, caps, caps2, charge;
        PixelImage image;
        RenderFormat render_format = RenderFormat::Rgba8;
        std::array<uint32_t, 8> depth_pixel{};
        uint32_t attached = 0;
        uint32_t clipper = 0;
        uint32_t legacy = 0;
        uint32_t dc = 0;
        bool locked = false, gpu = false, gpu_dirty = false;
        uint32_t scale = 1;        // GPU storage scale chosen at creation (render scale; 1 for everything else)
        uint32_t replaced = 0;     // scale of the pack replacement now on the GPU, 0 for the guest's own pixels
        uint64_t content_hash = 0; // hash of the last full unlock while unchanged since (texture pack), else 0
        const BackdropTile* backdrop_tile = nullptr; // background tile of a wide screen image, while unchanged
        PixelRect lock_rect{};
        uint32_t lock_flags = 0;
    };
    std::map<uint32_t, Surface> surfaces_;
    std::map<uint32_t, uint32_t> surface_aliases_;
    struct Device {
        uint32_t parent, references, target;
        RenderState state;
        bool scene = false;
        uint32_t lights_set = 0;
        bool state_sent = false; // The pending draw batch already holds this state (cleared by every change).
    };
    std::map<uint32_t, Device> devices_;
    // Draw calls are decoded into reused storage and appended to the host's pending batch.
    DecodedVertices draw_vertices_;
    std::vector<uint16_t> draw_indices_;
    uint32_t batch_device_ = 0;
    struct Clipper {
        uint32_t references = 1, window = 0;
    };
    std::map<uint32_t, Clipper> clippers_;
    uint32_t tables_[7]{};
    uint32_t table(Interface type);
    Adapter& adapter(uint32_t pointer);
    Result unknown(unsigned slot, const std::vector<uint64_t>& arguments);
    Result method(Interface type, unsigned slot, const std::vector<uint64_t>& arguments);
    void render(const std::function<void(Renderer&)>& command);
    // The renderer's texture size limit, asked once (a synchronous round trip waits for all queued GL work).
    uint32_t texture_limit();
    uint32_t texture_limit_ = 0;
    Surface& surface(uint32_t pointer);
    uint32_t create_surface(uint32_t adapter, uint32_t descriptor);
    uint32_t release_surface(uint32_t pointer);
    void surface_description(uint32_t pointer, uint32_t destination, bool locked);
    void ensure_gpu(uint32_t pointer);
    void sync_surface(uint32_t pointer);
    void upload_surface(uint32_t pointer, PixelRect rectangle);
    // Full unlock of a texture: dump it and/or put the pack's replacement on the GPU. True when replaced.
    bool replace_texture(uint32_t pointer);
    bool apply_replacement(uint32_t pointer, uint64_t hash, std::shared_ptr<const TextureReplacement> replacement);
    // Back to the guest's own pixels on the GPU after a replacement (refill, GPU write).
    void revert_texture(uint32_t pointer);
    // Once per present of the primary surface: applies finished replacements, reports the HD counters.
    void hd_present();
    void present_surface(uint32_t pointer, const char* kind);
    // How one present shows the frame: on = the view's rectangle of the frame (a 1024x768 screen at (source_x, 0))
    // scaled into the view's box of the client area.
    // canvas: the menu canvas extension (platform/letterbox.h) of a main menu or character select frame, else 0.
    // width x height: the client area it was placed in.
    struct Inset {
        bool on = false;
        InsetView view;
        PresentBackdrop backdrop;
        int32_t canvas = 0, width = 0, height = 0;
    };
    // Whether a picture is drawn by the guest on the menu canvas (main menu, character select) rather than around
    // a still screen.
    bool backdrop_canvas(const Backdrop& image) const;
    static void present_inset(Renderer& renderer, uint32_t pointer, int32_t x, int32_t y, uint32_t width,
                              uint32_t height, const Inset& inset);
    // Picks the wide image to show around this present's inset (consumes the frame's mark).
    void inset_backdrop(uint32_t window, Inset& inset);
    // Consumes the inset word for one present: whether it shows the inset (InsetSchedule).
    bool next_inset();
    // The inset of a width x height client area (for a present next_inset said shows one).
    Inset inset_view(int32_t width, int32_t height);
    void shown_inset(const Inset& inset);
    void trace_inset(const char* kind, const Inset& inset);
    bool gpu_blit(uint32_t pointer, PixelRect target, uint32_t source_pointer, PixelRect source_area, uint32_t clipper);
    Result surface_method(unsigned slot, const std::vector<uint64_t>& arguments);
    uint32_t create_device(uint32_t parent, uint32_t target);
    uint32_t release_device(uint32_t pointer);
    Result device_method(unsigned slot, const std::vector<uint64_t>& arguments);
    // SetTextureStageState and SetTexture as the guest's calls make them (ground_mesh.cpp).
    void device_texture_state(uint32_t device, uint32_t stage, uint32_t key, uint32_t value);
    void device_texture(uint32_t device, uint32_t stage, uint32_t texture);
    Result clipper_method(unsigned slot, const std::vector<uint64_t>& arguments);
    uint32_t clipper_window(uint32_t pointer);
    PixelRect clip_region(uint32_t pointer);
};
}
