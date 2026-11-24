#include "platform/window_host.h"
#include <SDL.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <exception>
#include <filesystem>
#include <thread>

using namespace sacred;
static void require(bool value, const char* reason) {
    if (!value) {
        throw std::runtime_error(reason);
    }
}

static RenderState state() {
    RenderState result;
    result.width = result.height = 64;
    result.render[8] = 3;
    result.render[22] = 1;
    result.render[23] = 4;
    result.render[25] = 8;
    result.render[19] = 2;
    result.render[20] = 1;
    for (auto& matrix : result.matrices) {
        for (unsigned i = 0; i < 4; ++i) {
            matrix[i * 5] = 1;
        }
    }

    for (unsigned i = 0; i < 2; ++i) {
        result.stages[i][1] = i ? 1 : 2;
        result.stages[i][2] = 0;
        result.stages[i][4] = i ? 1 : 2;
        result.stages[i][5] = 0;
        result.stages[i][11] = i;
        result.stages[i][13] = result.stages[i][14] = 3;
        result.stages[i][16] = result.stages[i][17] = 1;
    }

    return result;
}

static RenderVertex vertex(float x, float y, float z, float w, float u, float v) {
    RenderVertex result{};
    result.position[0] = x;
    result.position[1] = y;
    result.position[2] = z;
    result.position[3] = w;
    for (float& channel : result.diffuse) {
        channel = 1;
    }

    result.uv[0][0] = u;
    result.uv[0][1] = v;
    return result;
}

static void qualify(Renderer& renderer) {
    renderer.create_surface(1, 64, 64, RenderFormat::Rgba8);
    renderer.create_surface(2, 2, 2, RenderFormat::Rgba8);
    renderer.create_surface(3, 64, 64, RenderFormat::Depth16);
    renderer.attach_depth(1, 3);
    const std::vector<uint8_t> tiles = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    renderer.upload(2, tiles);
    require(renderer.read(2) == tiles, "RGBA upload/readback changed rows or channels");
    auto draw = state();
    draw.transformed = true;
    draw.textures[0] = 2;
    draw.stages[0][2] = draw.stages[0][5] = 2;
    const std::vector<RenderVertex> quad = {vertex(-0.5f, -0.5f, 0.5f, 1, 0, 0), vertex(63.5f, -0.5f, 0.5f, 1, 1, 0),
                                            vertex(63.5f, 63.5f, 0.5f, 1, 1, 1), vertex(-0.5f, 63.5f, 0.5f, 1, 0, 1)};
    renderer.clear(1, 3, 0xff000000, 1);
    renderer.draw(1, draw, 6, quad, {});
    auto pixels = renderer.read(1);
    auto pixel = [&](unsigned x, unsigned y, uint8_t r, uint8_t g, uint8_t b) {
        size_t at = (y * 64 + x) * 4;
        return pixels[at] == r && pixels[at + 1] == g && pixels[at + 2] == b;
    };
    require(pixel(8, 8, 255, 0, 0) && pixel(56, 8, 0, 255, 0) && pixel(8, 56, 0, 0, 255) &&
                pixel(56, 56, 255, 255, 255),
            "texture origin, packed channels or transformed pixel centers changed");
    std::filesystem::create_directories("build/tests/menu");
    renderer.capture_next("build/tests/menu/present-fixture.bmp");
    renderer.present(1);
    SDL_Surface* capture = SDL_LoadBMP("build/tests/menu/present-fixture.bmp");
    require(capture != nullptr && capture->w == 64 && capture->h == 64, "native drawable capture dimensions changed");
    SDL_Surface* captured_rgba = SDL_ConvertSurfaceFormat(capture, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(capture);
    require(captured_rgba != nullptr, "cannot decode native drawable capture");
    const uint8_t* captured = static_cast<const uint8_t*>(captured_rgba->pixels);
    auto captured_pixel = [&](unsigned x, unsigned y, uint8_t r, uint8_t g, uint8_t b) {
        const uint8_t* at = captured + y * captured_rgba->pitch + x * 4;
        return at[0] == r && at[1] == g && at[2] == b;
    };
    bool presented = captured_pixel(8, 8, 255, 0, 0) && captured_pixel(56, 8, 0, 255, 0) &&
                     captured_pixel(8, 56, 0, 0, 255) && captured_pixel(56, 56, 255, 255, 255);
    SDL_FreeSurface(captured_rgba);
    require(presented, "native drawable differs from presented quadrants");
    auto* image = SDL_CreateRGBSurfaceWithFormatFrom(pixels.data(), 64, 64, 32, 256, SDL_PIXELFORMAT_RGBA32);
    require(image != nullptr, "cannot create renderer QA image");
    int saved = SDL_SaveBMP(image, "build/tests/menu/render-fixture.bmp");
    SDL_FreeSurface(image);
    require(saved == 0, "cannot save renderer QA image");
    std::vector<RenderVertex> rhw = {vertex(0, 0, 0.5f, 1, 0, 0), vertex(64, 0, 0.5f, 0.25f, 1, 0),
                                     vertex(0, 64, 0.5f, 1, 0, 1)};
    renderer.clear(1, 1, 0xff000000, 1);
    renderer.draw(1, draw, 4, rhw, {});
    pixels = renderer.read(1);
    require(pixel(40, 8, 255, 0, 0), "RHW texture interpolation became affine");
    draw.render[22] = 2;
    renderer.clear(1, 1, 0xff000000, 1);
    renderer.draw(1, draw, 4, rhw, {});
    pixels = renderer.read(1);
    require(pixel(40, 8, 0, 0, 0), "D3D clockwise culling changed winding");
    draw.render[22] = 3;
    renderer.draw(1, draw, 4, rhw, {});
    pixels = renderer.read(1);
    require(pixel(40, 8, 255, 0, 0), "D3D counterclockwise culling removed clockwise face");

    draw = state();
    draw.render[7] = draw.render[14] = 1;
    std::vector<RenderVertex> triangle = {vertex(-0.75f, 0.75f, 0.25f, 1, 0, 0), vertex(0.75f, 0.75f, 0.25f, 1, 0, 0),
                                          vertex(-0.75f, -0.75f, 0.25f, 1, 0, 0)};
    for (auto& point : triangle) {
        point.diffuse[0] = point.diffuse[1] = 0;
    }

    renderer.clear(1, 3, 0xff000000, 1);
    renderer.draw(1, draw, 4, triangle, {});
    for (auto& point : triangle) {
        point.position[2] = 0.75f;
        point.diffuse[0] = 1;
        point.diffuse[2] = 0;
    }

    renderer.draw(1, draw, 4, triangle, {});
    pixels = renderer.read(1);
    require(pixel(16, 16, 0, 0, 255), "D3D depth order changed");
    renderer.clear(1, 3, 0xff000000, 1);
    for (auto& point : triangle) {
        point.position[2] = -0.25f;
    }

    renderer.draw(1, draw, 4, triangle, {});
    pixels = renderer.read(1);
    require(pixel(16, 16, 0, 0, 0), "D3D negative clip depth was accepted");
    draw.render[7] = 0;
    draw.render[15] = 1;
    draw.render[25] = 5;
    draw.render[24] = 96;
    auto translucent = tiles;
    for (size_t i = 3; i < translucent.size(); i += 4) {
        translucent[i] = 128;
    }

    renderer.upload(2, translucent);
    draw.textures[0] = 2;
    draw.stages[0][4] = 4;
    draw.stages[0][5] = 2;
    draw.stages[0][6] = 0;
    for (auto& point : triangle) {
        point.position[2] = 0.5f;
        point.diffuse[3] = 0.5f;
    }

    renderer.clear(1, 1, 0xff0000ff, 1);
    renderer.draw(1, draw, 4, triangle, {});
    pixels = renderer.read(1);
    require(pixel(16, 16, 0, 0, 255), "alpha test ran before texture alpha combination");
    draw.render[15] = 0;
    draw.render[27] = 1;
    draw.render[19] = 5;
    draw.render[20] = 6;
    draw.textures[0] = 0;
    draw.stages[0][4] = 2;
    draw.stages[0][5] = 0;
    renderer.draw(1, draw, 4, triangle, {});
    pixels = renderer.read(1);
    size_t at = (16 * 64 + 16) * 4;
    require(pixels[at] >= 127 && pixels[at] <= 128 && pixels[at + 1] == 0 && pixels[at + 2] >= 127 &&
                pixels[at + 2] <= 128,
            "source-alpha blend changed");
    renderer.upload(2, {255, 255, 0, 255}, 1, 0, 1, 1);
    auto updated = translucent;
    updated[4] = updated[5] = updated[7] = 255;
    updated[6] = 0;
    require(renderer.read(2) == updated, "partial upload changed pixels outside its dirty rectangle");
    renderer.create_surface(4, 64, 64, RenderFormat::Depth24Stencil8);
    renderer.attach_depth(1, 4);
    renderer.clear(1, 7, 0xff000000, 1, 0);
    draw = state();
    draw.render[52] = 1;
    draw.render[53] = draw.render[54] = 1;
    draw.render[55] = 3;
    draw.render[56] = 8;
    draw.render[57] = 7;
    draw.render[58] = draw.render[59] = 255;
    for (auto& point : triangle) {
        point.diffuse[3] = 1;
    }

    renderer.draw(1, draw, 4, triangle, {});
    draw.render[56] = 3;
    draw.render[55] = 1;
    for (auto& point : triangle) {
        point.diffuse[0] = 0;
        point.diffuse[1] = 1;
    }

    renderer.draw(1, draw, 4, triangle, {});
    draw.render[57] = 8;
    for (auto& point : triangle) {
        point.diffuse[0] = 1;
        point.diffuse[1] = 0;
    }

    renderer.draw(1, draw, 4, triangle, {});
    pixels = renderer.read(1);
    require(pixel(16, 16, 0, 255, 0), "packed depth/stencil replacement or comparison changed");
    renderer.destroy_surface(1);
    renderer.destroy_surface(2);
    renderer.destroy_surface(3);
    renderer.destroy_surface(4);
}

// Internal render scale and texture replacement: every entry point stays in guest units, so a surface stored at
// 2x reads back (2x2 box filter) exactly like the same work done at 1x wherever the scaled image has no edges
// inside a guest pixel.
static void qualify_scale(Renderer& renderer) {
    renderer.create_surface(10, 64, 64, RenderFormat::Rgba8);
    renderer.create_surface(11, 64, 64, RenderFormat::Depth16);
    renderer.attach_depth(10, 11);
    renderer.create_surface(12, 64, 64, RenderFormat::Rgba8, 2);
    renderer.create_surface(13, 64, 64, RenderFormat::Depth16, 2);
    renderer.attach_depth(12, 13);
    bool mismatch = false;
    try {
        renderer.attach_depth(10, 13);
    } catch (const std::exception&) {
        mismatch = true;
    }

    require(mismatch, "depth buffer of another scale attached");
    renderer.create_surface(14, 2, 2, RenderFormat::Rgba8);
    const std::vector<uint8_t> tiles = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    renderer.upload(14, tiles);
    auto draw = state();
    draw.transformed = true;
    draw.textures[0] = 14;
    draw.stages[0][2] = draw.stages[0][5] = 2;
    const std::vector<RenderVertex> quad = {vertex(-0.5f, -0.5f, 0.5f, 1, 0, 0), vertex(63.5f, -0.5f, 0.5f, 1, 1, 0),
                                            vertex(63.5f, 63.5f, 0.5f, 1, 1, 1), vertex(-0.5f, 63.5f, 0.5f, 1, 0, 1)};
    for (uint32_t target : {10u, 12u}) {
        renderer.clear(target, 3, 0xff000000, 1);
        renderer.draw(target, draw, 6, quad, {});
    }

    std::vector<uint8_t> pixels = renderer.read(12);
    require(pixels == renderer.read(10), "scale 2 textured quad differs from scale 1 after the 2x2 reduction");
    auto pixel = [&](unsigned x, unsigned y, uint8_t r, uint8_t g, uint8_t b) {
        size_t at = (y * 64 + x) * 4;
        return pixels[at] == r && pixels[at + 1] == g && pixels[at + 2] == b && pixels[at + 3] == 255;
    };
    // One guest pixel (10, 20) and (10, 21) of a transformed quad: D3D pixel centres at integers map to the centre
    // of each 2x2 block, so nothing spills into the neighbours.
    auto plain = state();
    plain.transformed = true;
    auto box = [](float x0, float y0, float x1, float y1) {
        std::vector<RenderVertex> result = {vertex(x0, y0, 0.5f, 1, 0, 0), vertex(x1, y0, 0.5f, 1, 0, 0),
                                            vertex(x1, y1, 0.5f, 1, 0, 0), vertex(x0, y1, 0.5f, 1, 0, 0)};
        for (auto& point : result) {
            point.diffuse[1] = point.diffuse[2] = 0;
        }

        return result;
    };
    renderer.clear(12, 3, 0xff000000, 1);
    renderer.draw(12, plain, 6, box(9.5f, 19.5f, 10.5f, 21.5f), {});
    pixels = renderer.read(12);
    require(pixel(10, 20, 255, 0, 0) && pixel(10, 21, 255, 0, 0) && pixel(9, 20, 0, 0, 0) && pixel(11, 20, 0, 0, 0) &&
                pixel(10, 19, 0, 0, 0) && pixel(10, 22, 0, 0, 0),
            "scale 2 moved transformed pixel centres");
    // A viewport away from the origin: transformed positions stay screen positions, clipped to the viewport.
    plain.x = 16;
    plain.y = 8;
    plain.width = 32;
    plain.height = 32;
    renderer.clear(12, 3, 0xff000000, 1);
    renderer.draw(12, plain, 6, box(19.5f, 19.5f, 20.5f, 20.5f), {});
    renderer.draw(12, plain, 6, box(9.5f, 19.5f, 10.5f, 20.5f), {});
    pixels = renderer.read(12);
    require(pixel(20, 20, 255, 0, 0) && pixel(19, 20, 0, 0, 0) && pixel(21, 20, 0, 0, 0) && pixel(10, 20, 0, 0, 0),
            "scale 2 viewport mapping changed");
    // Clear rectangles in guest units.
    renderer.clear(12, 1, 0xff000000, 1);
    renderer.clear(12, 1, 0xff00ff00, 1, 0, 5, 6, 3, 2);
    pixels = renderer.read(12);
    require(pixel(5, 6, 0, 255, 0) && pixel(7, 7, 0, 255, 0) && pixel(4, 6, 0, 0, 0) && pixel(8, 6, 0, 0, 0) &&
                pixel(5, 5, 0, 0, 0) && pixel(5, 8, 0, 0, 0),
            "scale 2 clear rectangle changed");
    // A guest upload is magnified, a copy into a 1x surface reduced: both exact.
    renderer.upload(12, tiles, 3, 3, 2, 2);
    pixels = renderer.read(12);
    require(pixel(3, 3, 255, 0, 0) && pixel(4, 3, 0, 255, 0) && pixel(3, 4, 0, 0, 255) && pixel(4, 4, 255, 255, 255) &&
                pixel(5, 6, 0, 255, 0) && pixel(2, 3, 0, 0, 0),
            "scale 2 upload changed pixels");
    renderer.copy(10, 0, 0, 12, 0, 0, 64, 64, false);
    require(renderer.read(10) == pixels, "copy from a scale 2 surface changed pixels");
    renderer.copy(12, 32, 32, 10, 3, 3, 2, 2, true);
    pixels = renderer.read(12);
    require(pixel(32, 32, 255, 0, 0) && pixel(33, 33, 255, 255, 255), "copy into a scale 2 surface changed pixels");
    // Present samples the scaled surface into the 64x64 window.
    renderer.capture_next("build/tests/menu/present-scale2.bmp");
    renderer.present(12);
    SDL_Surface* capture = SDL_LoadBMP("build/tests/menu/present-scale2.bmp");
    require(capture != nullptr && capture->w == 64 && capture->h == 64, "scale 2 capture dimensions changed");
    SDL_Surface* captured = SDL_ConvertSurfaceFormat(capture, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(capture);
    require(captured != nullptr, "cannot decode scale 2 capture");
    const uint8_t* at = static_cast<const uint8_t*>(captured->pixels) + 3 * captured->pitch + 4 * 4;
    const bool shown = at[0] == 0 && at[1] == 255 && at[2] == 0;
    SDL_FreeSurface(captured);
    require(shown, "scale 2 present changed pixels");
    // Linear filtering under WRAP at scale 2: a quad that maps a whole texture 1:1 must not blend in the opposite
    // edge at its border (the seams between the menu backdrop tiles); coordinates outside [0,1] keep repeating.
    {
        renderer.create_surface(15, 4, 1, RenderFormat::Rgba8);
        renderer.upload(15, {0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255, 255});
        auto wrap = state();
        wrap.transformed = true;
        wrap.textures[0] = 15;
        wrap.stages[0][2] = wrap.stages[0][5] = 2;
        wrap.stages[0][13] = wrap.stages[0][14] = 1;
        wrap.stages[0][16] = wrap.stages[0][17] = 2;
        auto strip = [](float x0, float x1, float u1) {
            return std::vector<RenderVertex>{vertex(x0, -0.5f, 0.5f, 1, 0, 0), vertex(x1, -0.5f, 0.5f, 1, u1, 0),
                                             vertex(x1, 0.5f, 0.5f, 1, u1, 1), vertex(x0, 0.5f, 0.5f, 1, 0, 1)};
        };
        renderer.clear(12, 3, 0xff000000, 1);
        renderer.draw(12, wrap, 6, strip(-0.5f, 3.5f, 1), {}); // pixels 0..3: one whole texture
        renderer.draw(12, wrap, 6, strip(3.5f, 7.5f, 1), {});  // pixels 4..7: the neighbouring tile
        renderer.draw(12, wrap, 6, strip(7.5f, 15.5f, 2), {}); // pixels 8..15: u 0..2, repeats
        pixels = renderer.read(12);
        auto red = [&](unsigned x) {
            return pixels[x * 4];
        };
        require(red(0) == 0 && red(4) == 0, "scale 2 tile edge blended the opposite edge (seam)");
        require(red(3) >= 220 && red(7) >= 220, "scale 2 tile edge lost its own texel");
        require(red(11) >= 180 && red(12) <= 40 && red(15) >= 180, "u outside [0,1] stopped repeating at scale 2");
        // The menu backdrop's tiles: vertices on integer coordinates (edges through pixel centres), u from 0 to
        // 1 + half a texel, NEAREST under WRAP. Each tile must cover exactly the guest pixels it covers at scale 1,
        // without a half-pixel sliver of its neighbour or of the clear colour, and without wrapping.
        renderer.create_surface(16, 4, 1, RenderFormat::Rgba8);
        renderer.upload(16, std::vector<uint8_t>(16, 255));
        wrap.stages[0][16] = wrap.stages[0][17] = 1;
        auto tile = [&](uint32_t texture, float x0, float x1) {
            wrap.textures[0] = texture;
            const float u1 = 4.5f / 4;
            std::vector<RenderVertex> quad = {vertex(x0, 0, 0.5f, 1, 0, 0), vertex(x1, 0, 0.5f, 1, u1, 0),
                                              vertex(x1, 1, 0.5f, 1, u1, 1), vertex(x0, 1, 0.5f, 1, 0, 1)};
            renderer.draw(12, wrap, 6, quad, {});
        };
        renderer.clear(12, 3, 0xff0000ff, 1);
        tile(15, 0, 4);
        tile(16, 4, 8);
        pixels = renderer.read(12);
        for (unsigned x = 0; x < 3; ++x) {
            require(pixels[x * 4] == 0 && pixels[x * 4 + 1] == 0 && pixels[x * 4 + 2] == 0,
                    "scale 2 integer-aligned tile left a sliver of the clear colour");
        }

        require(pixels[3 * 4] == 255 && pixels[3 * 4 + 2] == 255, "scale 2 integer-aligned tile lost its last texel");
        for (unsigned x = 4; x < 8; ++x) {
            require(pixels[x * 4] == 255 && pixels[x * 4 + 1] == 255 && pixels[x * 4 + 2] == 255,
                    "scale 2 integer-aligned tiles show a seam");
        }

        require(pixels[8 * 4] == 0 && pixels[8 * 4 + 2] == 255, "scale 2 integer-aligned tile spilled past its edge");
        renderer.destroy_surface(15);
        renderer.destroy_surface(16);
    }

    // Texture replacement: 4x4 for the 2x2 texture, sampled with the same UVs; reverting restores the original.
    std::vector<uint8_t> high(4 * 4 * 4);
    for (unsigned y = 0; y < 4; ++y) {
        for (unsigned x = 0; x < 4; ++x) {
            uint8_t* texel = &high[(y * 4 + x) * 4];
            texel[0] = uint8_t(x * 80);
            texel[1] = uint8_t(y * 80);
            texel[2] = 7;
            texel[3] = 255;
        }
    }

    renderer.replace(14, 2, high);
    renderer.clear(10, 3, 0xff000000, 1);
    renderer.draw(10, draw, 6, quad, {});
    pixels = renderer.read(10);
    require(pixel(4, 4, 0, 0, 7) && pixel(20, 4, 80, 0, 7) && pixel(60, 60, 240, 240, 7),
            "replaced texture sampled wrongly");
    require(renderer.read(14).size() == 2 * 2 * 4, "replaced texture read back at the replacement size");
    bool rejected = false;
    try {
        renderer.replace(14, 2, tiles);
    } catch (const std::exception&) {
        rejected = true;
    }

    require(rejected, "replacement of the wrong size accepted");
    renderer.replace(14, 1, tiles);
    renderer.clear(10, 3, 0xff000000, 1);
    renderer.draw(10, draw, 6, quad, {});
    pixels = renderer.read(10);
    require(pixel(8, 8, 255, 0, 0) && pixel(56, 56, 255, 255, 255), "reverted texture differs from the original");
    for (uint32_t id : {10u, 11u, 12u, 13u, 14u}) {
        renderer.destroy_surface(id);
    }
}

// HD mode inset: a 16x12 rectangle of a frame whose other pixels are magenta, shown scaled into a 64x48 box of the
// 64x64 window. The box keeps its place, the bars stay black and nothing of the frame outside the rectangle bleeds
// into the resampled edges; with render scale 2 the same.
static void qualify_inset(Renderer& renderer) {
    std::filesystem::create_directories("build/tests/menu");
    for (uint32_t scale : {1u, 2u}) {
        const uint32_t id = 30 + scale;
        renderer.create_surface(id, 64, 64, RenderFormat::Rgba8, scale);
        std::vector<uint8_t> frame(64 * 64 * 4);
        for (unsigned y = 0; y < 64; ++y) {
            for (unsigned x = 0; x < 64; ++x) {
                uint8_t* at = &frame[(y * 64 + x) * 4];
                at[3] = 255;
                if (x >= 16 || y >= 12) {
                    at[0] = 255;
                    at[2] = 255;
                    continue;
                }

                const unsigned quadrant = (x >= 8 ? 1 : 0) + (y >= 6 ? 2 : 0);
                at[0] = quadrant == 0 || quadrant == 3 ? 255 : 0;
                at[1] = quadrant == 1 || quadrant == 3 ? 255 : 0;
                at[2] = quadrant == 2 || quadrant == 3 ? 255 : 0;
            }
        }

        renderer.upload(id, frame);
        const std::string path = "build/tests/menu/present-inset-" + std::to_string(scale) + ".bmp";
        renderer.capture_next(path);
        renderer.present(id, 0, 0, 16, 12, 64, 64, 0, 8, 64, 48);
        SDL_Surface* capture = SDL_LoadBMP(path.c_str());
        require(capture != nullptr && capture->w == 64 && capture->h == 64, "inset capture dimensions changed");
        SDL_Surface* rgba = SDL_ConvertSurfaceFormat(capture, SDL_PIXELFORMAT_RGBA32, 0);
        SDL_FreeSurface(capture);
        require(rgba != nullptr, "cannot decode the inset capture");
        auto near = [&](unsigned x, unsigned y, int r, int g, int b) {
            const uint8_t* at = static_cast<const uint8_t*>(rgba->pixels) + y * rgba->pitch + x * 4;
            return std::abs(at[0] - r) <= 8 && std::abs(at[1] - g) <= 8 && std::abs(at[2] - b) <= 8;
        };
        bool placed = near(10, 14, 255, 0, 0) && near(54, 14, 0, 255, 0) && near(10, 50, 0, 0, 255) &&
                      near(54, 50, 255, 255, 255) && near(30, 3, 0, 0, 0) && near(30, 60, 0, 0, 0);
        // The right and bottom edges of the box repeat the rectangle's last texels, never the magenta beyond (the
        // quadrant colours blend over a few pixels around the middle).
        bool clean = true;
        for (unsigned y = 8; y < 56; ++y) {
            clean = clean && (near(63, y, 0, 255, 0) || near(63, y, 255, 255, 255) || (y >= 28 && y < 36));
        }

        for (unsigned x = 0; x < 64; ++x) {
            clean = clean && (near(x, 55, 0, 0, 255) || near(x, 55, 255, 255, 255) || (x >= 28 && x < 36));
        }

        SDL_FreeSurface(rgba);
        require(placed, "scaled inset misplaced or bars not black");
        require(clean, "scaled inset sampled the frame outside its rectangle");
        renderer.destroy_surface(id);
    }

    // A backdrop behind an inset: a 16x1 image (4 cyan, 8 yellow, 4 cyan) stretched over 128x64 of a 64x64 frame
    // from x = -32; the inset (16x12 of red) covers 32x24 at (16, 20). The backdrop is clipped to the frame, the
    // inset is drawn over it.
    renderer.create_surface(40, 64, 64, RenderFormat::Rgba8);
    renderer.upload(40, std::vector<uint8_t>(64 * 64 * 4, 0));
    std::vector<uint8_t> red(16 * 12 * 4);
    for (size_t i = 0; i < red.size(); i += 4) {
        red[i] = 255;
        red[i + 3] = 255;
    }

    renderer.upload(40, red, 0, 0, 16, 12);
    renderer.create_surface(41, 16, 1, RenderFormat::Rgba8);
    std::vector<uint8_t> strip;
    for (unsigned x = 0; x < 16; ++x) {
        const bool yellow = x >= 4 && x < 12;
        strip.insert(strip.end(), {uint8_t(yellow ? 255 : 0), 255, uint8_t(yellow ? 0 : 255), 255});
    }

    renderer.upload(41, strip);
    PresentBackdrop backdrop;
    backdrop.id = 41;
    backdrop.x = -32;
    backdrop.y = 0;
    backdrop.width = 128;
    backdrop.height = 64;
    renderer.capture_next("build/tests/menu/present-backdrop.bmp");
    renderer.present(40, 0, 0, 16, 12, 64, 64, 16, 20, 32, 24, &backdrop);
    SDL_Surface* capture = SDL_LoadBMP("build/tests/menu/present-backdrop.bmp");
    require(capture != nullptr, "backdrop capture missing");
    SDL_Surface* rgba = SDL_ConvertSurfaceFormat(capture, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(capture);
    require(rgba != nullptr, "cannot decode the backdrop capture");
    auto near = [&](unsigned x, unsigned y, int r, int g, int b) {
        const uint8_t* at = static_cast<const uint8_t*>(rgba->pixels) + y * rgba->pitch + x * 4;
        return std::abs(at[0] - r) <= 8 && std::abs(at[1] - g) <= 8 && std::abs(at[2] - b) <= 8;
    };
    // Frame x 4 is image x 36 of 128 (the centre of texel 4: yellow), x 56 is image x 88 (texel 11: yellow), x 12
    // texel 5; the inset is red.
    const bool shown = near(4, 4, 255, 255, 0) && near(56, 60, 255, 255, 0) && near(32, 32, 255, 0, 0) &&
                       near(16, 20, 255, 0, 0) && near(47, 43, 255, 0, 0) && near(12, 32, 255, 255, 0);
    SDL_FreeSurface(rgba);
    require(shown, "backdrop not drawn around the inset");
    renderer.destroy_surface(40);
    renderer.destroy_surface(41);
}

// The lit ground (GroundLight): three mesh quads whose four corners lie 30 units from their tile, so each quad has
// one colour: within reach of a light, out of every light's reach, and with the sum past the ramp's end.
static void qualify_ground_light(Renderer& renderer) {
    renderer.create_surface(60, 64, 64, RenderFormat::Rgba8);
    GroundLight light;
    light.on = true;
    light.inverse_zoom = 0.5f;
    light.zoom = 2;
    light.origin[0] = light.origin[1] = 32; // a tile at t is looked up at t - 32
    const int32_t corners[8] = {-30, 0, 0, -30, 0, 30, 30, 0};
    std::copy(std::begin(corners), std::end(corners), light.corners);
    for (int i = 0; i < 256; ++i) {
        light.ramp.push_back(uint8_t(i));
        light.ramp.push_back(uint8_t(255 - i));
        light.ramp.push_back(uint8_t(i / 2));
    }

    light.lights = {100, 100, 200, 500, 500, 300, 100, 2000, 50};
    struct Quad {
        float x, y, tile_x, tile_y, grey;
        int light, radius; // the light at its tile, -1 for none
    };
    const Quad quads[3] = {{4, 4, 132, 132, 1.0f, 0, 200},
                           {24, 4, 2032, 2032, 128.0f / 255, -1, 0},
                           {44, 4, 532, 532, 200.0f / 255, 1, 300}};
    std::vector<RenderVertex> vertices;
    for (const Quad& quad : quads) {
        for (int i = 0; i < 4; ++i) {
            RenderVertex v = vertex(quad.x + float(i % 2) * 16, quad.y + float(i / 2) * 16, 0.5f, 1, 0, 0);
            v.diffuse[0] = v.diffuse[1] = v.diffuse[2] = quad.grey;
            v.normal[0] = quad.tile_x;
            v.normal[1] = quad.tile_y;
            v.normal[2] = float(i);
            vertices.push_back(v);
        }
    }

    renderer.mesh_write(900, 0, vertices);
    DrawBatch batch;
    batch.ground_light = light;
    batch.states.push_back(state());
    batch.states[0].transformed = true;
    DrawItem item;
    item.target = 60;
    item.primitive = 4;
    item.transformed = item.diffuse = true;
    item.mesh = 900;
    item.quads = 3;
    item.mesh_transform[3] = 0.5f;
    item.mesh_lit = true;
    batch.items.push_back(item);
    renderer.clear(60, 1, 0xff000000, 1);
    renderer.draw(batch);
    const auto pixels = renderer.read(60);
    for (const Quad& quad : quads) {
        int sum = 32;
        if (quad.light >= 0) {
            // The game's distance: one Newton step from the bit-trick inverse square root, truncated.
            const float square = 900;
            uint32_t bits;
            std::memcpy(&bits, &square, 4);
            bits = 0x5f3759df - (bits >> 1);
            float root;
            std::memcpy(&root, &bits, 4);
            const int reach = quad.radius - int(1.0f / (root * (1.5f - 0.5f * square * root * root)));
            sum += reach >= 224 ? 223 : reach;
        }

        sum = std::min(sum, 255);
        const int grey = int(quad.grey * 255 + 0.5f);
        const int expected[3] = {grey * sum / 256, grey * (255 - sum) / 256, grey * (sum / 2) / 256};
        const size_t at = (size_t(quad.y + 8) * 64 + size_t(quad.x + 8)) * 4;
        for (int c = 0; c < 3; ++c) {
            require(std::abs(int(pixels[at + c]) - expected[c]) <= 1,
                    "lit ground colour differs from the light lookup");
        }
    }

    // The same item unlit keeps its vertex colours.
    batch.items[0].mesh_lit = false;
    batch.ground_light = {};
    renderer.clear(60, 1, 0xff000000, 1);
    renderer.draw(batch);
    const auto plain = renderer.read(60);
    const size_t first = (size_t(12) * 64 + 12) * 4, second = (size_t(12) * 64 + 32) * 4;
    require(plain[first] == 255 && plain[first + 1] == 255 && plain[second] == 128 && plain[second + 2] == 128,
            "unlit mesh colours changed");
    renderer.mesh_destroy(900);
    renderer.destroy_surface(60);
}

int main() {
    WindowHost host;
    std::exception_ptr failure;
    std::thread worker([&] {
        try {
            auto window = host.create_window("Sacred renderer qualification", 0, 0, 64, 64, true, {}, 0, 0);
            host.render(window, qualify);
            host.render(window, qualify_scale);
            host.render(window, qualify_inset);
            host.render(window, qualify_ground_light);
            host.destroy_window(window);
        } catch (...) {
            failure = std::current_exception();
        }

        host.finish();
    });
    host.loop();
    worker.join();
    if (failure) {
        try {
            std::rethrow_exception(failure);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "RENDER_FAILURE %s\n", error.what());
        }

        return 1;
    }

    std::puts("RENDER_PASS rows=1 color=1 transformed=1 depth=1 alpha_test=1 blend=1 teardown=1 scale2=1 replace=1 "
              "inset=1 ground_light=1");
    return 0;
}
