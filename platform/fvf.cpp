#include "renderer.h"
#include <cstring>

namespace sacred {
FvfLayout fvf_layout(uint32_t fvf) {
    FvfLayout l;
    const uint32_t position = fvf & 0xe;
    l.normal = fvf & 0x10;
    l.reserved = fvf & 0x20;
    l.transformed = position == 4;
    l.diffuse = fvf & 0x40;
    l.specular = fvf & 0x80;
    l.coordinates = (fvf >> 8) & 15;
    if ((fvf & ~0xffeu) || (position != 2 && position != 4) || l.coordinates > 2 ||
        (l.transformed && (l.normal || l.reserved)) || (l.normal && l.reserved)) {
        return FvfLayout{};
    }

    l.stride = (l.transformed ? 16 : 12) + (l.normal ? 12 : 0) + (l.reserved ? 4 : 0) + (l.diffuse ? 4 : 0) +
               (l.specular ? 4 : 0) + l.coordinates * 8;
    return l;
}

namespace {
inline void color(float* result, uint32_t value) {
    result[0] = float((value >> 16) & 255) / 255;
    result[1] = float((value >> 8) & 255) / 255;
    result[2] = float(value & 255) / 255;
    result[3] = float(value >> 24) / 255;
}
}

void decode_fvf(const uint8_t* bytes, const FvfLayout& l, uint32_t count, RenderVertex* out) {
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* input = bytes + size_t(i) * l.stride;
        RenderVertex& output = out[i];
        output = RenderVertex{};
        output.position[3] = 1;
        const unsigned n = l.transformed ? 4u : 3u;
        std::memcpy(output.position, input, n * 4);
        input += n * 4;
        if (l.normal) {
            std::memcpy(output.normal, input, 12);
            input += 12;
        }

        if (l.reserved) {
            input += 4; // D3DLVERTEX's dwReserved is present in its FVF stride.
        }

        for (float& channel : output.diffuse) {
            channel = 1;
        }

        uint32_t word;
        if (l.diffuse) {
            std::memcpy(&word, input, 4);
            color(output.diffuse, word);
            input += 4;
        }

        if (l.specular) {
            std::memcpy(&word, input, 4);
            color(output.specular, word);
            input += 4;
        }

        if (l.coordinates) {
            std::memcpy(output.uv, input, l.coordinates * 8);
        }
    }
}

void expand_raw_vertices(DrawBatch& batch) {
    bool any = false;
    size_t total = 0;
    for (const DrawItem& item : batch.items) {
        any |= item.raw_fvf != 0;
        if (!item.mesh && !item.skin_mesh) {
            total += item.vertex_count; // mesh items draw renderer-side vertices
        }
    }

    if (any) {
        std::vector<RenderVertex> vertices(total);
        size_t at = 0;
        for (DrawItem& item : batch.items) {
            if (item.mesh || item.skin_mesh) {
                continue;
            }

            if (item.raw_fvf) {
                decode_fvf(batch.raw.data() + item.raw_offset, fvf_layout(item.raw_fvf), item.vertex_count,
                           &vertices[at]);
            } else {
                std::memcpy(&vertices[at], batch.vertices.data() + item.first_vertex,
                            item.vertex_count * sizeof(RenderVertex));
            }

            item.first_vertex = uint32_t(at);
            item.raw_fvf = 0;
            at += item.vertex_count;
        }

        batch.vertices.swap(vertices);
    }

    for (DrawItem& item : batch.items) {
        if (!item.remap.on || item.mesh || item.skin_mesh || item.remap.set > 1 ||
            uint64_t(item.first_vertex) + item.vertex_count > batch.vertices.size()) {
            continue;
        }

        RenderVertex* vertices = &batch.vertices[item.first_vertex];
        if ((item.remap.edge[0] || item.remap.edge[1]) && item.vertex_count) {
            uint32_t left = 0, right = 0;
            for (uint32_t i = 1; i < item.vertex_count; ++i) {
                if (vertices[i].position[0] < vertices[left].position[0]) {
                    left = i;
                }

                if (vertices[i].position[0] > vertices[right].position[0]) {
                    right = i;
                }
            }

            const float x0 = vertices[left].position[0], x1 = vertices[right].position[0];
            // The texels per pixel along x: from the leftmost vertex to a rightmost one on the same row.
            for (uint32_t i = 0; i < item.vertex_count; ++i) {
                if (vertices[i].position[0] == x1 && vertices[i].position[1] == vertices[left].position[1]) {
                    right = i;
                }
            }

            if (x1 > x0) {
                const float* a = vertices[left].uv[item.remap.set];
                const float* b = vertices[right].uv[item.remap.set];
                const float du = (b[0] - a[0]) / (x1 - x0), dv = (b[1] - a[1]) / (x1 - x0);
                for (uint32_t i = 0; i < item.vertex_count; ++i) {
                    float* position = vertices[i].position;
                    float* uv = vertices[i].uv[item.remap.set];
                    const float move = position[0] == x0   ? -item.remap.edge[0]
                                       : position[0] == x1 ? item.remap.edge[1]
                                                           : 0;
                    position[0] += move;
                    uv[0] += move * du;
                    uv[1] += move * dv;
                }
            }
        }

        for (uint32_t i = 0; i < item.vertex_count; ++i) {
            float* uv = vertices[i].uv[item.remap.set];
            if (item.remap.screen[0] != 0) {
                uv[0] = vertices[i].position[0] * item.remap.screen[0] + item.remap.screen[1];
            }

            uv[0] = uv[0] * item.remap.scale[0] + item.remap.offset[0];
            uv[1] = uv[1] * item.remap.scale[1] + item.remap.offset[1];
        }

        item.remap.on = false;
    }
}
}
