# The ground in vertex buffers, and Granny skinning on the GPU

Two renderer-side changes that take work off the game thread in the world view. Both are on by default and can be
turned off for comparison (`SACRED_GROUND_MESH=0`, `SACRED_GPU_SKIN=0`).

## Ground mesh (`runtime/ddraw/ground_mesh.cpp`)

How the game draws the ground and how the cache stands in for it is described in `ground_mesh.h`. Hooks: stand-ins
(`generate.py` NATIVE_CALLS) for renderTileRow (0x62ae90), drawTileLayers (0x62d3c0), `cQuadBatcher::add` and
`::setTexture`; a native jump (`NATIVE_JUMPS`) over renderTileRow's quad build (0x62b103 -> 0x62b7ff) except for
water and lava tiles. Guest functions the cache needs while it builds a tile (the layer record cache 0x635fe0) or
draws (the texture manager's get 0x65ed90) run through `Control::invoke` from the stand-in; the renderTileRow
stand-in runs the original through `Control::run_call` with its own code around it.

- A sector's quads are grouped by pass, by place in the tile's layer chain and by textures. The place matters:
  grouping by texture alone draws two records of one pass on the same tile in the wrong order.
- The base pass draws in the device state of the first quad the walk added (the cTileRenderer passes between the
  rows may change it); the layer passes mirror `render_state_cache_set(0x2000)` (0x643470) on the device state.
- Night and caves: the game lights the ground (world state flags 0x20 at +0x10, or the byte at +0x9e0c). The row
  walk then passes each quad through cTileRenderer (0x61ed70), which colours the four corners with the world state's
  light lookup (0x41b510): the frame's light sources (`{x, y, radius}` in the walk's unscaled screen space, the list
  the previous frame collected, swapped in by 0x418fb0) within reach of the corner add `radius - distance` to 32
  (distance from a one-step fast inverse square root), the sum clamped to 255 indexes a colour ramp, and the tile's
  own light byte scales that colour. The cached quads carry their tile's place and corner in the otherwise unused
  normal, and the vertex shader does the lookup from a small float texture (ramp and lights, `GroundLight` in
  `platform/renderer.h`), so the cache stays on (`SACRED_GROUND_MESH_LIT=0` leaves the lit ground to the game). Night
  town, zoomed out: about 8500 draw calls and 72 FPS without it, 1900 and 100 with it. cTileRenderer shares corner
  colours between neighbouring tiles through two row buffers that expect every tile of a row; for the quads that
  still pass through it (water) the stand-in puts it in the state where it looks every corner up itself.
- The renderer keeps all mesh vertices in one arena buffer with one vertex array (ranges drawn with a base vertex,
  packed into a new buffer when the top reaches the end). Groups of one pass and place with the same textures, clamp
  and camera transform go out as one `glMultiDrawElementsBaseVertex` (DrawItem::ranges).
- Apple's OpenGL does not implement glCopyBufferSubData (the copy is dropped), so the arena keeps each mesh's
  vertices on the CPU and uploads them again when a range grows or the arena is packed.

Pixel diffs against the game's own ground differ only in moving characters and single pixels along some tile edges
(the vertex positions are computed in another order). In a dense town the game's draw calls fall from about 8000 to
about 1700 per frame.

## Granny skinning on the GPU (`runtime/ddraw/gpu_skin.cpp`)

The deform stand-in (granny.dll 0x1001e660) is turned into per-vertex terms (bone, weight, source vertex: per-bone
groups add, skinned vertices replace, split copies take another vertex's list, a one-bone mesh has weight 1).
`SACRED_SKIN_CHECK=1` compares them with the CPU deform (at most 4 terms, positions and normals share them). The
template is kept per mesh; a strided draw of a deform's output is drawn from it with the bone rows as uniforms
(diffuse = bone indices, specular = weights).

Readers of the deform outputs on the CPU (`SACRED_SKIN_TRAP=1` protects the output pages and logs the first reader):
the mesh draw 0x405c90 samples about a dozen positions for a bounding box (from 0x405f71); cGranny::renderShadow draws
the same outputs strided with a diffuse colour (FVF 0x142), so its deforms go to the GPU too (the bone indices travel
in the second texture coordinate, 6 bits each, so the diffuse slot stays free). Deforms called under 0x405c90 or
renderShadow (return addresses 0x405e20, 0x40721c on the guest stack) skip the CPU vertices; the bounding box
samples are still written, vertex 0 doubles as a check that nothing else wrote the buffer since, and a draw the
shader cannot do gets the buffers filled first.

## Not done

- Texture atlases and draw-call merging: the window thread is not the limit after the ground mesh; worth revisiting
  once the game thread is faster.
- Animation sampling on worker threads: guest code runs one thread at a time, so granny.dll's control walk cannot be
  split; only a fully native sampling path could.
