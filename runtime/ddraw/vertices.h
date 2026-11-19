#pragma once
#include "../../platform/renderer.h"
#include "../process/memory.h"

namespace sacred {
struct DecodedVertices {
    bool transformed = false, diffuse = false, specular = false;
    unsigned coordinates = 0;
    std::vector<RenderVertex> vertices;
};
// The forms taking `result` reuse its storage (one draw call after another); the others return a fresh value.
void decode_vertices(const Memory& memory, uint32_t fvf, uint32_t address, uint32_t count, DecodedVertices& result);
void decode_strided(const Memory& memory, uint32_t fvf, uint32_t address, uint32_t count, DecodedVertices& result);
void decode_indices(const Memory& memory, uint32_t address, uint32_t count, uint32_t vertices,
                    std::vector<uint16_t>& result);
DecodedVertices decode_vertices(const Memory& memory, uint32_t fvf, uint32_t address, uint32_t count);
DecodedVertices decode_strided(const Memory& memory, uint32_t fvf, uint32_t address, uint32_t count);
std::vector<uint16_t> decode_indices(const Memory& memory, uint32_t address, uint32_t count, uint32_t vertices);
}
