#include "runtime/ddraw/vertices.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>

using namespace sacred;
static unsigned checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x))                                                                                                      \
            throw GuestFault(#x);                                                                                      \
    } while (0)
static uint32_t bits(float value) {
    uint32_t result;
    std::memcpy(&result, &value, sizeof result);
    return result;
}

static void reject(const std::function<void()>& call) {
    bool rejected = false;
    try {
        call();
    } catch (const GuestFault&) {
        rejected = true;
    }

    CHECK(rejected);
}

int main() {
    try {
        Memory memory;
        const uint32_t base = 0x03000000;
        memory.reserve(base, 8192, "vertices");
        memory.commit(base, 4096, Read | Write, "vertices");
        auto put = [&](std::initializer_list<uint32_t> values) {
            uint32_t at = base;
            for (uint32_t value : values) {
                memory.store32(at, value);
                at += 4;
            }
        };
        put({bits(1), bits(2), bits(3), 0, 0, bits(1), bits(0.25f), bits(0.75f), bits(4), bits(5), bits(6), bits(-1), 0,
             0, bits(0.5f), bits(1)});
        auto normal = decode_vertices(memory, 0x112, base, 2);
        CHECK(!normal.transformed && !normal.diffuse && normal.coordinates == 1);
        CHECK(normal.vertices.size() == 2 && normal.vertices[1].position[0] == 4);
        CHECK(normal.vertices[0].position[3] == 1 && normal.vertices[0].normal[2] == 1);
        CHECK(normal.vertices[1].normal[0] == -1 && normal.vertices[1].uv[0][0] == 0.5f);
        CHECK(normal.vertices[0].diffuse[0] == 1 && normal.vertices[0].specular[0] == 0);
        put({bits(1), bits(2), bits(3), 0xdeadbeef, 0x80402010, 0xff010203, bits(0.25f), bits(0.75f)});
        auto lit = decode_vertices(memory, 0x1e2, base, 1);
        CHECK(!lit.transformed && lit.diffuse && lit.specular);
        CHECK(lit.vertices[0].position[2] == 3 && lit.vertices[0].uv[0][0] == 0.25f);
        CHECK(std::abs(lit.vertices[0].diffuse[0] - 0.2509804f) < 0.0000001f);
        CHECK(std::abs(lit.vertices[0].diffuse[3] - 0.5019608f) < 0.0000001f);
        CHECK(lit.vertices[0].specular[3] == 1);
        put({bits(-0.5f), bits(2), bits(0.25f), bits(0.5f), 0xffffffff, 0, bits(0.25f), bits(0.75f)});
        auto transformed = decode_vertices(memory, 0x1c4, base, 1);
        CHECK(transformed.transformed && transformed.diffuse && transformed.specular);
        CHECK(transformed.vertices[0].position[0] == -0.5f && transformed.vertices[0].position[3] == 0.5f);
        put({0, 0, bits(0.5f), bits(1), 0xffffffff, 0, bits(1), bits(0.25f), bits(0.75f)});
        auto two = decode_vertices(memory, 0x244, base, 1);
        CHECK(two.transformed && two.coordinates == 2 && !two.specular);
        CHECK(two.vertices[0].uv[1][0] == 0.25f && two.vertices[0].uv[1][1] == 0.75f);
        memory.store32(base + 512, 0x00010000);
        memory.store32(base + 516, 0);
        const std::vector<uint16_t> indices = {0, 1, 0};
        CHECK(decode_indices(memory, base + 512, 3, 2) == indices);
        memory.store32(base + 512, 0x00020000);
        reject([&] { decode_indices(memory, base + 512, 3, 2); });
        reject([&] { decode_vertices(memory, 0x10000 | 0x1c4, base, 1); });
        reject([&] { decode_vertices(memory, 0x116, base, 1); });
        reject([&] { decode_vertices(memory, 0x244, base + 4092, 1); });
        memory.protect(base, 4096, Read, "vertices");
        CHECK(decode_vertices(memory, 0x244, base, 1).vertices[0].uv[1][0] == 0.25f);
        { // UV remaps run after the expansion of raw items, on the texture coordinate set they name.
            DrawBatch batch;
            batch.states.emplace_back();
            RenderVertex vertex{};
            vertex.uv[0][0] = 1;
            vertex.uv[0][1] = 0.5f;
            vertex.uv[1][0] = 1;
            batch.vertices = {vertex, vertex};
            DrawItem plain;
            plain.vertex_count = 1;
            plain.remap.on = true;
            plain.remap.set = 0;
            plain.remap.scale[0] = 0.25f;
            plain.remap.scale[1] = 0.5f;
            plain.remap.offset[0] = 0.5f;
            plain.remap.offset[1] = 0.125f;
            DrawItem second = plain;
            second.first_vertex = 1;
            second.remap.set = 1;
            batch.items = {plain, second};
            expand_raw_vertices(batch);
            CHECK(batch.vertices[0].uv[0][0] == 0.75f && batch.vertices[0].uv[0][1] == 0.375f &&
                  batch.vertices[0].uv[1][0] == 1);
            CHECK(batch.vertices[1].uv[1][0] == 0.75f && batch.vertices[1].uv[0][0] == 1);
            CHECK(!batch.items[0].remap.on);
            expand_raw_vertices(batch); // applied once
            CHECK(batch.vertices[0].uv[0][0] == 0.75f);
        }

        { // Edge extension: a 256-pixel quad with u 0.5 .. 1.5 (a scrolled sky) stretched 100 pixels to the left and 50
            // to the right keeps its texels per pixel; then the UV remap applies.
            DrawBatch batch;
            batch.states.emplace_back();
            auto corner = [](float x, float y, float u, float v) {
                RenderVertex vertex{};
                vertex.position[0] = x;
                vertex.position[1] = y;
                vertex.uv[0][0] = u;
                vertex.uv[0][1] = v;
                return vertex;
            };
            // Bottom-left first, so the leftmost vertex is not on the top row.
            batch.vertices = {corner(10, 20, 0.5f, 1), corner(10, 0, 0.5f, 0), corner(266, 0, 1.5f, 0),
                              corner(266, 20, 1.5f, 1)};
            DrawItem quad;
            quad.vertex_count = 4;
            quad.remap.on = true;
            quad.remap.edge[0] = 100;
            quad.remap.edge[1] = 50;
            quad.remap.offset[0] = 1;
            batch.items = {quad};
            expand_raw_vertices(batch);
            const auto& v = batch.vertices;
            CHECK(v[0].position[0] == -90 && v[1].position[0] == -90 && v[2].position[0] == 316 &&
                  v[3].position[0] == 316);
            CHECK(std::fabs(v[1].uv[0][0] - (0.5f - 100.0f / 256 + 1)) < 1e-6f && v[0].uv[0][1] == 1 &&
                  v[1].uv[0][1] == 0);
            CHECK(std::fabs(v[2].uv[0][0] - (1.5f + 50.0f / 256 + 1)) < 1e-6f && v[2].uv[0][1] == 0 &&
                  v[3].uv[0][1] == 1);
        }

        { // Screen-anchored u: a menu tile at x 406 with the guest's 257/256 texel rate (u 0 .. 1.001953), stretched
            // 406 pixels to the left, takes u from the vertex x (one texel of an 1836-wide image per pixel), so the
            // pixel at x = 405 samples the image's pixel 405 exactly and v keeps the guest's coordinates.
            DrawBatch batch;
            batch.states.emplace_back();
            auto corner = [](float x, float y, float u, float v) {
                RenderVertex vertex{};
                vertex.position[0] = x;
                vertex.position[1] = y;
                vertex.uv[0][0] = u;
                vertex.uv[0][1] = v;
                return vertex;
            };
            const float rate = 257.0f / 256.0f;
            batch.vertices = {corner(406, 0, 0, 0), corner(662, 0, rate, 0), corner(406, 256, 0, rate),
                              corner(662, 256, rate, rate)};
            DrawItem tile;
            tile.vertex_count = 4;
            tile.remap.on = true;
            tile.remap.edge[0] = 406;
            tile.remap.screen[0] = 1.0f / 1836;
            tile.remap.screen[1] = 0.5f / 1836;
            tile.remap.scale[1] = 256.0f / 768;
            batch.items = {tile};
            expand_raw_vertices(batch);
            const auto& v = batch.vertices;
            CHECK(v[0].position[0] == 0 && v[1].position[0] == 662);
            auto texel = [&](float x) { // u at x by the quad's own interpolation, in image pixels
                return (v[0].uv[0][0] + (v[1].uv[0][0] - v[0].uv[0][0]) * (x - v[0].position[0]) / 662.0f) * 1836;
            };
            CHECK(std::fabs(texel(405) - 405.5f) < 1e-3f && std::fabs(texel(406) - 406.5f) < 1e-3f);
            CHECK(std::fabs(texel(0) - 0.5f) < 1e-3f && std::fabs(texel(661) - 661.5f) < 1e-3f);
            CHECK(std::fabs(v[2].uv[0][1] - rate * 256 / 768) < 1e-6f);
        }

        std::printf("VERTICES_PASS checks=%u FVF_112_1e2_1c4_244=1 indices=1 checked_memory=1\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "VERTICES_FAILURE %s\n", error.what());
        return 1;
    }
}
