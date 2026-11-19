#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <array>
#include <vector>
#include <string>

struct SDL_Window;
namespace sacred {
enum class RenderFormat { Rgba8, Depth16, Depth24Stencil8 };
struct RenderVertex {
    float position[4], normal[3], diffuse[4], specular[4], uv[2][2];
};
struct RenderLight {
    uint32_t type = 0;
    std::array<float, 4> diffuse{}, specular{}, ambient{};
    std::array<float, 3> position{}, direction{};
    float range = 0, falloff = 0, attenuation[3]{}, theta = 0, phi = 0;
    bool enabled = false;
};
struct RenderState {
    std::array<uint32_t, 153> render{};
    std::array<std::array<uint32_t, 25>, 2> stages{};
    std::array<std::array<float, 16>, 5> matrices{};
    std::array<uint32_t, 2> textures{};
    uint32_t x = 0, y = 0, width = 0, height = 0;
    float min_z = 0, max_z = 1;
    std::array<std::array<float, 4>, 4> material{};
    float power = 0;
    std::array<RenderLight, 8> lights{};
    bool transformed = false, diffuse = false, specular = false;
};
// Draw calls reach the renderer in batches. The vertices and indices of a batch are shared; the indices of an item
// are relative to its first vertex. An item takes its textures and vertex-format flags from itself, everything else
// from states[state] (the same fields inside that RenderState are ignored).
// Texture coordinates rewritten on the renderer thread after the vertices are expanded: uv[set] = uv[set] * scale +
// offset (a texture drawn from a part of a larger one, runtime/ddraw/menu_backdrops.h).
// edge: before that, the item's leftmost vertices (pre-transformed positions) move edge[0] pixels left and its
// rightmost edge[1] pixels right, their texture coordinates along at the item's own texels per pixel (a quad
// stretched out, its texture continued).
// screen: then, when screen[0] is not 0, u = x * screen[0] + screen[1] from each vertex's pre-transformed x (an image
// drawn one texel per pixel, anchored to the screen rather than continued at the guest's own texel rate, which for
// the menu tiles is 257 texels over 256 pixels).
struct UvRemap {
    bool on = false;
    uint32_t set = 0;
    float scale[2] = {1, 1}, offset[2] = {0, 0};
    float edge[2] = {0, 0};
    float screen[2] = {0, 0};
};
struct DrawItem {
    uint32_t target = 0, state = 0, primitive = 0;
    std::array<uint32_t, 2> textures{};
    uint32_t first_vertex = 0, vertex_count = 0, first_index = 0, index_count = 0;
    bool transformed = false, diffuse = false, specular = false;
    // Nonzero: the vertices are still the guest's bytes in this flexible vertex format, at raw_offset in the batch's
    // raw bytes; expand_raw_vertices() turns them into RenderVertex on the renderer thread.
    uint32_t raw_fvf = 0, raw_offset = 0;
    UvRemap remap;
    // Nonzero: the item draws quads [first_quad, first_quad + quads) of a vertex buffer kept on the renderer
    // (Renderer::mesh_write) instead of batch vertices. Quads are 4 transformed vertices drawn as the triangles
    // 0 1 2 and 2 1 3; their positions move to x * mesh_transform[0] + mesh_transform[1],
    // y * mesh_transform[0] + mesh_transform[2], with z mesh_transform[3] and rhw 1 (the ground's sector caches,
    // runtime/ddraw/ground_mesh.cpp). mesh_clamp: the ItemFit clamp per stage (all coordinates within a texel of 0..1).
    uint32_t mesh = 0, first_quad = 0, quads = 0;
    // ranges > 0: the item draws DrawBatch::mesh_ranges [first_range, first_range + ranges) instead (mesh is the
    // first range's), all with this transform, clamp and state: one draw call for many meshes.
    uint32_t first_range = 0, ranges = 0;
    // skin_bones > 0: a Granny mesh skinned in the vertex shader (runtime/ddraw/gpu_skin.cpp). Position and normal
    // are the mesh's sources, specular holds four weights and uv[1][0] their bone indices (6 bits each); the bones'
    // rows (three vec4 per bone: matrix row and translation) are DrawBatch::skin_rows [skin_first, skin_first + 3 *
    // skin_bones).
    uint32_t skin_first = 0, skin_bones = 0;
    bool skin_normalize = false;
    // skin_mesh != 0: the skinned vertices are that renderer mesh (vertices 0..vertex_count, mesh_clamp as the
    // texture clamp), indexed by the item's indices; skin_color_on: one diffuse colour for all of them.
    uint32_t skin_mesh = 0;
    bool skin_color_on = false;
    float skin_color[4] = {1, 1, 1, 1};
    float mesh_transform[4] = {1, 0, 0, 0};
    std::array<uint8_t, 2> mesh_clamp{};
};
struct MeshRange {
    uint32_t mesh = 0, first_quad = 0, quads = 0;
};
struct DrawBatch {
    std::vector<MeshRange> mesh_ranges;
    std::vector<float> skin_rows; // vec4 each
    std::vector<RenderState> states;
    std::vector<DrawItem> items;
    std::vector<RenderVertex> vertices;
    std::vector<uint16_t> indices;
    std::vector<uint8_t> raw;
};
// The layout of a D3D7 flexible vertex format the renderer takes: its stride in bytes, 0 for one it does not.
struct FvfLayout {
    unsigned stride = 0, coordinates = 0;
    bool transformed = false, normal = false, reserved = false, diffuse = false, specular = false;
};
FvfLayout fvf_layout(uint32_t fvf);
// Expands `count` vertices of `layout` from little-endian guest bytes.
void decode_fvf(const uint8_t* bytes, const FvfLayout& layout, uint32_t count, RenderVertex* out);
// Expands the raw items of a batch into its vertices, in item order, and applies the items' UV remaps (renderer
// thread).
void expand_raw_vertices(DrawBatch& batch);
// An image drawn behind an inset at presentation: the whole surface `id`, stretched over (x, y, width, height) of the
// frame (frame units, may reach past the frame, which clips it).
// `shade` darkens or tints it as the frame's own translucent overlays did the inset: colour = image * shade[3] +
// shade[0..2].
struct PresentBackdrop {
    uint32_t id = 0;
    int32_t x = 0, y = 0, width = 0, height = 0;
    std::array<float, 4> shade{0, 0, 0, 1};
};
// SDL owns the window; a backend owns only its presentation resources.
class Renderer {
public:
    virtual ~Renderer() = default;
    virtual void erase(uint32_t argb) = 0;
    virtual uint32_t max_texture_size() = 0;
    // Every surface is addressed in guest units (width x height) by every entry point below. `scale` (internal render
    // scale, 1 = off) allocates the GPU storage at scale x the guest size: uploads are magnified into it, reads,
    // copies and presents of it are resampled, and draws into it rasterize at that resolution.
    virtual void create_surface(uint32_t id, uint32_t width, uint32_t height, RenderFormat format,
                                uint32_t scale = 1) = 0;
    // Replaces the storage of an existing color surface with `rgba`, (scale x width) x (scale x height) pixels, top
    // row first (texture replacement; scale 1 with the guest pixels reverts it). A scale above 1 keeps floor(log2
    // scale) mipmap levels below the replacement and no deeper, so minification never mixes texels across atlas
    // gutters of the original size.
    virtual void replace(uint32_t id, uint32_t scale, const std::vector<uint8_t>& rgba) = 0;
    virtual void destroy_surface(uint32_t id) = 0;
    virtual void attach_depth(uint32_t target, uint32_t depth) = 0;
    virtual void upload(uint32_t id, const std::vector<uint8_t>& rgba, uint32_t x = 0, uint32_t y = 0,
                        uint32_t width = 0, uint32_t height = 0) = 0;
    virtual std::vector<uint8_t> read(uint32_t id) = 0;
    // Copies an unscaled rectangle between two different color surfaces on the GPU, top-left origin. `opaque`
    // stores full alpha in the destination rectangle, as an upload of X8R8G8B8 pixels does.
    virtual void copy(uint32_t destination, int32_t x, int32_t y, uint32_t source, int32_t source_x, int32_t source_y,
                      uint32_t width, uint32_t height, bool opaque) = 0;
    // A rectangle with width 0 clears the whole target; otherwise only [x, x+width) x [y, y+height), top-left origin.
    virtual void clear(uint32_t target, uint32_t flags, uint32_t argb, float depth, uint32_t stencil = 0, int x = 0,
                       int y = 0, int width = 0, int height = 0) = 0;
    virtual void draw(const DrawBatch& batch) = 0;
    // Vertex buffers kept on the renderer for DrawItem::mesh: writes `vertices` at vertex `first` of mesh `id`
    // (created or grown as needed, keeping what it held); destroy frees it.
    virtual void mesh_write(uint32_t id, uint32_t first, const std::vector<RenderVertex>& vertices) = 0;
    virtual void mesh_destroy(uint32_t id) = 0;
    // One draw call with its own state, textures and flags taken from `state`.
    void draw(uint32_t target, const RenderState& state, uint32_t primitive, const std::vector<RenderVertex>& vertices,
              const std::vector<uint16_t>& indices) {
        DrawBatch batch;
        batch.states.push_back(state);
        batch.vertices = vertices;
        batch.indices = indices;
        DrawItem item;
        item.target = target;
        item.primitive = primitive;
        item.textures = state.textures;
        item.vertex_count = uint32_t(vertices.size());
        item.index_count = uint32_t(indices.size());
        item.transformed = state.transformed;
        item.diffuse = state.diffuse;
        item.specular = state.specular;
        batch.items.push_back(item);
        draw(batch);
    }

    // Shows [x, x+width) x [y, y+height) of the surface. With a frame size, that rectangle is an inset of a
    // frame_width x frame_height frame, shown in the box (inset_x, inset_y, inset_width, inset_height) of that frame
    // (a width or height of 0: the rectangle's own): it lands where that box would in the letterboxed full frame,
    // resampled when scaled, and the rest of the drawable stays black, or shows the backdrop.
    virtual void present(uint32_t id, int32_t x = 0, int32_t y = 0, uint32_t width = 0, uint32_t height = 0,
                         uint32_t frame_width = 0, uint32_t frame_height = 0, int32_t inset_x = 0, int32_t inset_y = 0,
                         uint32_t inset_width = 0, uint32_t inset_height = 0,
                         const PresentBackdrop* backdrop = nullptr) = 0;
    // Saves the next presented drawable to a BMP; with render scale, also the presented rectangle at storage size
    // ("<path>-x<k>.bmp") unless `storage` is false.
    virtual void capture_next(const std::string& path, bool storage = true) = 0;
};
// A QA status badge drawn over every presented frame, in its top-left corner: a square of colour `argb` (0: no
// badge) and, when not zero, `number` in decimal next to it. Set from any thread (runtime/modules/call_trace.cpp).
struct PresentBadge {
    std::atomic<uint32_t> argb{0}, number{0};
};
PresentBadge& present_badge();
// The frame rate counter (Settings.cfg SHOW_FPS : 1): frames presented per second, measured over half-second windows
// and drawn in the top-left corner of the game's image at every present, after the captures (screenshots never
// show it). Set from any thread.
std::atomic<bool>& present_fps_counter();
void configure_renderer_window();
std::unique_ptr<Renderer> make_renderer(SDL_Window* window);
}
