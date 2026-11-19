#pragma once
#include "../../platform/renderer.h"
#include "vertices.h"
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace sacred {
class Memory;
// Granny's skinned meshes deformed on the GPU (on by default, SACRED_GPU_SKIN=0 turns it off). The deform stand-in
// (runtime/granny/native.c, 1001e660) still computes the bone matrices on the guest thread and reports them with its
// output buffers; a strided draw whose position (and normal) stream is such an output is then drawn from the mesh's
// static vertex data (source position, source normal, four bones and weights: granny_skin_template) with the bone rows
// as uniforms, and the vertex shader does the weighted sum that skin_positions / skin_normals do on the CPU. Only the
// deforms for FUN_00405c90's mesh draws skip the CPU vertices (the stand-in finds its call on the guest stack); the
// game reads a dozen positions of those for a bounding box, which deformed() writes. The shadow passes read every
// deformed position and stay on the CPU, as does the ray intersection (picking). A record whose buffer was written by
// anything else since (vertex 0 differs from the one written here) is dropped, and a draw the shader cannot do gets the
// buffers filled on the CPU first (materialize).
class GpuSkin {
public:
    explicit GpuSkin(Memory& memory);
    ~GpuSkin();
    // The deform hook (granny_skin_hook).
    void deformed(uint32_t mesh, uint32_t positions, uint32_t normals, uint32_t vertices, bool normalize,
                  const float* rows, uint32_t bones, bool cpu_skipped);
    // granny_skin_take / granny_skin_forget.
    bool take(uint32_t mesh, uint32_t vertices);
    void forget(uint32_t positions);
    // For a strided draw (D3D7 DrawIndexedPrimitiveStrided): true when it draws a deform's output; `out` gets the
    // vertices (source position and normal, bone indices in diffuse, weights in specular, the guest's texture
    // coordinates) and `rows` / `normalize` what the item needs.
    struct Draw {
        const std::vector<float>* rows = nullptr;
        bool normalize = false;
        // Nonzero: the vertices are the renderer mesh `mesh` (Renderer::mesh_write), uploaded first when `upload`
        // is not empty, instead of `out`; a diffuse colour the same for every vertex comes as `color`.
        uint32_t mesh = 0;
        std::vector<RenderVertex> upload;
        std::vector<uint32_t> retired; // renderer meshes to destroy
        uint8_t clamp = 3;
        bool color_on = false;
        float color[4] = {1, 1, 1, 1};
    };
    bool draw(const Memory& memory, uint32_t fvf, uint32_t strided, uint32_t count, DecodedVertices& out, Draw& result);
    static constexpr uint32_t max_bones = 64; // the shader's uniform array: 3 rows per bone
    struct Stats {
        uint64_t draws = 0, skinned = 0, vertices = 0, declined_mesh = 0, declined_draw = 0, materialized = 0,
                 uploads = 0;
    };
    Stats stats;
    void report_traps();

private:
    struct Mesh {
        uint32_t pos_from = 0, nrm_from = 0, vertices = 0, bones = 0;
        bool built = false, ok = false;
        std::vector<float> data;
        // The renderer mesh made from data with the texture coordinates at (uv, uv_stride); 0 when none yet.
        uint32_t gpu = 0, uv = 0, uv_stride = 0;
        uint8_t clamp = 3;
    };
    uint32_t next_gpu_mesh_ = 0x80000000u; // above the ground mesh ids
    std::vector<uint32_t> retired_;
    struct Record {
        uint32_t mesh = 0, normals = 0, vertices = 0;
        bool normalize = false, cpu_skipped = false;
        float canary[3] = {}; // vertex 0's position as written here (cpu_skipped)
        std::vector<float> rows;
    };
    Memory& memory_;
    std::unordered_map<uint32_t, Mesh> meshes_;    // by Granny mesh address
    std::unordered_map<uint32_t, Record> records_; // by position output buffer
    Mesh* mesh(uint32_t address, uint32_t vertices, uint32_t bones);
    void materialize(uint32_t positions, Record& record, const Mesh& m);
    void store_float(uint32_t address, float value) {
        uint32_t bits;
        std::memcpy(&bits, &value, 4);
        memory_.store32(address, bits);
    }
};
}
