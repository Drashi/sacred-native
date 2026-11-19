#include "gpu_skin.h"
#include "../granny/native.h"
#include "../process/memory.h"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <sys/mman.h>
#include <unistd.h>

extern "C" {
GrannySkinHook granny_skin_hook = nullptr;
int (*granny_skin_template_fn)(uint32_t, uint32_t, float*) = nullptr;
int (*granny_skin_take)(uint32_t, uint32_t) = nullptr;
void (*granny_skin_forget)(uint32_t) = nullptr;
unsigned long granny_skin_calls[3];
}

extern uint8_t* g_mem;
extern bool (*sacred_guest_fault_hook)(uint32_t address, uint32_t source_pc);
namespace sacred {
namespace {
GpuSkin* g_skin = nullptr;
// SACRED_SKIN_TRAP=1 (debugging): the pages of deform outputs are made inaccessible after the deform; the first access
// to each is logged with the guest code address that made it (SKIN_TRAP lines), to find what reads the outputs.
constexpr size_t trap_pages = 4096;
struct TrapPage {
    uintptr_t page;
    uint32_t low, high;
};
TrapPage g_trap[trap_pages];
size_t g_trap_count = 0;
struct TrapHit {
    uint32_t pc;
    uint32_t count;
};
TrapHit g_hits[256];
size_t g_hit_count = 0;
long host_page() {
    static const long size = sysconf(_SC_PAGESIZE);
    return size;
}

bool trap_fault(uint32_t address, uint32_t pc) {
    const uintptr_t host = reinterpret_cast<uintptr_t>(g_mem) + address;
    const uintptr_t page = host & ~uintptr_t(host_page() - 1);
    for (size_t i = 0; i < g_trap_count; ++i) {
        if (g_trap[i].page != page) {
            continue;
        }

        if (address >= g_trap[i].low && address < g_trap[i].high) {
            size_t h = 0;
            while (h < g_hit_count && g_hits[h].pc != pc) {
                ++h;
            }

            if (h == g_hit_count && g_hit_count < 256) {
                g_hits[g_hit_count++] = {pc, 0};
            }

            if (h < g_hit_count) {
                ++g_hits[h].count;
            }
        }

        mprotect(reinterpret_cast<void*>(page), size_t(host_page()), PROT_READ | PROT_WRITE);
        g_trap[i] = g_trap[--g_trap_count];
        return true;
    }

    return false;
}

void trap_release_all() {
    for (size_t i = 0; i < g_trap_count; ++i) {
        mprotect(reinterpret_cast<void*>(g_trap[i].page), size_t(host_page()), PROT_READ | PROT_WRITE);
    }

    g_trap_count = 0;
}

void trap_range(uint32_t low, uint32_t high) {
    const uintptr_t first = (reinterpret_cast<uintptr_t>(g_mem) + low) & ~uintptr_t(host_page() - 1);
    const uintptr_t last = (reinterpret_cast<uintptr_t>(g_mem) + high - 1) & ~uintptr_t(host_page() - 1);
    for (uintptr_t page = first; page <= last; page += uintptr_t(host_page())) {
        bool known = false;
        for (size_t i = 0; i < g_trap_count; ++i) {
            known |= g_trap[i].page == page;
        }

        if (known || g_trap_count == trap_pages) {
            continue;
        }

        g_trap[g_trap_count++] = {page, low, high};
        mprotect(reinterpret_cast<void*>(page), size_t(host_page()), PROT_NONE);
    }
}

int trap_mode() {
    static const int on = [] {
        const char* e = std::getenv("SACRED_SKIN_TRAP");
        return e && *e == '1';
    }();
    return on;
}

float scalar(const uint8_t* bytes) {
    float f;
    std::memcpy(&f, bytes, 4);
    return f;
}
}

extern "C" {
static void skin_hook(uint32_t mesh, uint32_t positions, uint32_t normals, uint32_t vertices, int normalize,
                      const float* rows, uint32_t bones, int cpu_skipped) {
    if (g_skin) {
        g_skin->deformed(mesh, positions, normals, vertices, normalize != 0, rows, bones, cpu_skipped != 0);
    }
}

static int skin_take(uint32_t mesh, uint32_t vertices) {
    return g_skin && g_skin->take(mesh, vertices);
}

static void skin_forget(uint32_t positions) {
    if (g_skin) {
        g_skin->forget(positions);
    }
}
}

GpuSkin::GpuSkin(Memory& memory) : memory_(memory) {
    g_skin = this;
    granny_skin_hook = skin_hook;
    granny_skin_take = skin_take;
    granny_skin_forget = skin_forget;
}

GpuSkin::~GpuSkin() {
    granny_skin_hook = nullptr;
    granny_skin_take = nullptr;
    granny_skin_forget = nullptr;
    g_skin = nullptr;
}

GpuSkin::Mesh* GpuSkin::mesh(uint32_t address, uint32_t vertices, uint32_t bones) {
    if (bones > max_bones || !vertices) {
        return nullptr;
    }

    // A mesh keeps its source arrays for its lifetime; one loaded again at the same address has other ones.
    Mesh& m = meshes_[address];
    const uint32_t pos_from = memory_.load32(address + 0x1c), nrm_from = memory_.load32(address + 0x24);
    if (!m.built || m.vertices != vertices || m.bones != bones || m.pos_from != pos_from || m.nrm_from != nrm_from) {
        m.built = true;
        m.pos_from = pos_from;
        m.nrm_from = nrm_from;
        m.vertices = vertices;
        m.bones = bones;
        m.data.resize(size_t(vertices) * 14);
        m.ok = granny_skin_template_fn && granny_skin_template_fn(address, vertices, m.data.data()) != 0;
        if (!m.ok) {
            m.data.clear();
        }
    }

    return m.ok ? &m : nullptr;
}

// The deform of one vertex from the mesh's template, in floats (what the shader does).
static void skin_vertex(const float* v, const float* rows, float position[3], float normal[3]) {
    for (int r = 0; r < 3; ++r) {
        position[r] = normal[r] = 0;
    }

    for (int k = 0; k < 4; ++k) {
        const float w = v[10 + k];
        if (w == 0) {
            continue;
        }

        const float* b = rows + 12 * size_t(v[6 + k] + 0.5f);
        for (int r = 0; r < 3; ++r) {
            const float* row = b + 4 * r;
            position[r] += w * (row[0] * v[0] + row[1] * v[1] + row[2] * v[2] + row[3]);
            normal[r] += w * (row[0] * v[3] + row[1] * v[4] + row[2] * v[5]);
        }
    }
}

bool GpuSkin::take(uint32_t mesh_address, uint32_t vertices) {
    // The bones count is read the way the stand-in reads it.
    return mesh(mesh_address, vertices, memory_.load32(mesh_address + 0xc)) != nullptr;
}

void GpuSkin::forget(uint32_t positions) {
    records_.erase(positions);
}

void GpuSkin::deformed(uint32_t mesh_address, uint32_t positions, uint32_t normals, uint32_t vertices, bool normalize,
                       const float* rows, uint32_t bones, bool cpu_skipped) {
    Record& record = records_[positions];
    record.mesh = 0;
    Mesh* m = mesh(mesh_address, vertices, bones);
    if (!m) {
        ++stats.declined_mesh;
        return;
    }

    record.mesh = mesh_address;
    record.normals = normals;
    record.vertices = vertices;
    record.normalize = normalize;
    record.rows.assign(rows, rows + size_t(bones) * 12);
    record.cpu_skipped = cpu_skipped;
    if (cpu_skipped) {
        // The game reads a dozen of the positions right after the deform for the mesh's bounding box
        // (FUN_00405c90, from 0x405f71: every n/11th vertex, at least every third): those are done here.
        const uint32_t step = vertices <= 11 ? 1 : vertices <= 22 ? 2 : vertices < 33 ? 3 : vertices / 11;
        for (uint32_t i = 0; i < vertices; i += step) {
            float position[3], normal[3];
            skin_vertex(&m->data[size_t(i) * 14], record.rows.data(), position, normal);
            for (int r = 0; r < 3; ++r) {
                store_float(positions + i * 12 + 4 * r, position[r]);
            }

            if (i == 0) {
                std::memcpy(record.canary, position, sizeof record.canary);
            }
        }
    }

    if (trap_mode()) {
        sacred_guest_fault_hook = trap_fault;
        trap_range(positions, positions + vertices * 12);
        if (normals) {
            trap_range(normals, normals + vertices * 12);
        }
    }
}

// What the CPU deform would have left in the buffers, for a draw the GPU does not take.
void GpuSkin::materialize(uint32_t positions, Record& record, const Mesh& m) {
    for (uint32_t i = 0; i < record.vertices; ++i) {
        float position[3], normal[3];
        skin_vertex(&m.data[size_t(i) * 14], record.rows.data(), position, normal);
        if (record.normalize) {
            const float l = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
            if (l > 0) {
                for (float& c : normal) {
                    c /= l;
                }
            }
        }

        for (int r = 0; r < 3; ++r) {
            store_float(positions + i * 12 + 4 * r, position[r]);
            if (record.normals) {
                store_float(record.normals + i * 12 + 4 * r, normal[r]);
            }
        }
    }

    record.cpu_skipped = false;
    ++stats.materialized;
}

void GpuSkin::report_traps() {
    if (!trap_mode()) {
        return;
    }

    for (size_t h = 0; h < g_hit_count; ++h) {
        std::fprintf(stderr, "SKIN_TRAP pc=%08x reads=%u\n", g_hits[h].pc, g_hits[h].count);
    }

    g_hit_count = 0;
}

bool GpuSkin::draw(const Memory& memory, uint32_t fvf, uint32_t strided, uint32_t count, DecodedVertices& out,
                   Draw& result) {
    ++stats.draws;
    if (trap_mode()) {
        trap_release_all(); // the draw itself reads them (decode_strided)
    }

    // XYZ, optionally NORMAL and DIFFUSE, at most one texture coordinate set: the specular slot carries the weights
    // and the second coordinate set the four bone indices (6 bits each).
    if ((fvf & ~0xff2u) || (fvf & 0xe) != 2 || !count) {
        return false;
    }

    const unsigned coordinates = (fvf >> 8) & 15;
    const uint32_t positions = memory.load32(strided), position_stride = memory.load32(strided + 4);
    auto found = records_.find(positions);
    if (found == records_.end() || !found->second.mesh) {
        return false;
    }

    Record& record = found->second;
    auto m = meshes_.find(record.mesh);
    if (m == meshes_.end() || !m->second.ok || m->second.vertices != record.vertices) {
        records_.erase(found);
        return false;
    }

    if (record.cpu_skipped) {
        // The buffer still holds this deform's first vertex: nothing else wrote it since.
        float now[3];
        for (int r = 0; r < 3; ++r) {
            const uint32_t bits = memory.load32(positions + 4 * r);
            std::memcpy(&now[r], &bits, 4);
        }

        if (std::memcmp(now, record.canary, sizeof now)) {
            records_.erase(found);
            return false;
        }
    }

    const bool normal = fvf & 0x10, diffuse = fvf & 0x40;
    uint32_t uv_data = 0, uv_stride = 0, color_data = 0, color_stride = 0;
    bool fits = position_stride == 12 && count <= record.vertices && !(fvf & 0x80) && coordinates <= 1 &&
                (!normal ||
                 (record.normals && memory.load32(strided + 8) == record.normals && memory.load32(strided + 12) == 12));
    if (fits && coordinates) {
        uv_data = memory.load32(strided + 32), uv_stride = memory.load32(strided + 36);
        fits = uv_data && uv_stride >= 8;
    }

    if (fits && diffuse) {
        color_data = memory.load32(strided + 16), color_stride = memory.load32(strided + 20);
        fits = color_data && (!color_stride || color_stride >= 4);
    }

    if (!fits) {
        // Not a draw the shader can do: the CPU's buffers as the deform would have left them.
        if (record.cpu_skipped) {
            materialize(positions, record, m->second);
        }

        ++stats.declined_draw;
        return false;
    }

    result.rows = &record.rows;
    result.normalize = record.normalize;
    result.retired.swap(retired_);
    ++stats.skinned;
    stats.vertices += count;
    if (!diffuse || !color_stride) {
        // The mesh's vertices kept on the renderer: built once per mesh and texture coordinate array.
        Mesh& mesh = m->second;
        if (!mesh.gpu || mesh.uv != uv_data || mesh.uv_stride != uv_stride) {
            // The texture coordinates of all the mesh's vertices (the draw may use fewer).
            if (coordinates && !memory.readable(uv_data, uint64_t(uv_stride) * (mesh.vertices - 1) + 8)) {
                if (record.cpu_skipped) {
                    materialize(positions, record, mesh);
                }

                --stats.skinned;
                stats.vertices -= count;
                ++stats.declined_draw;
                return false;
            }

            if (mesh.gpu) {
                result.retired.push_back(mesh.gpu);
            }

            mesh.gpu = ++next_gpu_mesh_;
            mesh.uv = uv_data;
            mesh.uv_stride = uv_stride;
            result.upload.resize(mesh.vertices);
            const float* data = mesh.data.data();
            uint8_t inside = 3;
            for (uint32_t i = 0; i < mesh.vertices; ++i, data += 14) {
                RenderVertex& v = result.upload[i];
                v = RenderVertex{};
                v.position[0] = data[0];
                v.position[1] = data[1];
                v.position[2] = data[2];
                v.position[3] = 1;
                v.normal[0] = data[3];
                v.normal[1] = data[4];
                v.normal[2] = data[5];
                for (unsigned k = 0; k < 4; ++k) {
                    v.specular[k] = data[10 + k];
                    v.diffuse[k] = 1;
                }

                const uint32_t packed =
                    uint32_t(data[6]) | uint32_t(data[7]) << 6 | uint32_t(data[8]) << 12 | uint32_t(data[9]) << 18;
                v.uv[1][0] = float(packed);
                if (coordinates) {
                    const uint8_t* uv = reinterpret_cast<const uint8_t*>(memory.host_address(uv_data + i * uv_stride));
                    v.uv[0][0] = scalar(uv);
                    v.uv[0][1] = scalar(uv + 4);
                    // As the renderer's ItemFit: all coordinates within a texel of 0..1 (ground_mesh.cpp's rule).
                    for (unsigned axis = 0; axis < 2; ++axis) {
                        if (!(v.uv[0][axis] >= -1.0f / 256 && v.uv[0][axis] <= 1.0f + 1.0f / 256)) {
                            inside &= uint8_t(~(1u << axis));
                        }
                    }
                }
            }

            mesh.clamp = inside;
            ++stats.uploads;
        }

        result.mesh = mesh.gpu;
        result.clamp = mesh.clamp;
        if (diffuse) {
            const uint8_t* c = reinterpret_cast<const uint8_t*>(memory.host_address(color_data));
            const uint32_t value = uint32_t(c[0]) | uint32_t(c[1]) << 8 | uint32_t(c[2]) << 16 | uint32_t(c[3]) << 24;
            result.color_on = true;
            result.color[0] = float((value >> 16) & 255) / 255;
            result.color[1] = float((value >> 8) & 255) / 255;
            result.color[2] = float(value & 255) / 255;
            result.color[3] = float(value >> 24) / 255;
        }

        out.transformed = false;
        out.diffuse = diffuse;
        out.specular = false;
        out.coordinates = coordinates;
        out.vertices.clear();
        return true;
    }

    if (coordinates) {
        memory.check(uv_data, uint64_t(uv_stride) * (count - 1) + 8, Read);
    }

    if (diffuse) {
        memory.check(color_data, uint64_t(color_stride) * (count - 1) + 4, Read);
    }

    out.transformed = false;
    out.diffuse = diffuse;
    out.specular = false;
    out.coordinates = coordinates;
    out.vertices.resize(count);
    const float* data = m->second.data.data();
    for (uint32_t i = 0; i < count; ++i, data += 14) {
        RenderVertex& v = out.vertices[i];
        v.position[0] = data[0];
        v.position[1] = data[1];
        v.position[2] = data[2];
        v.position[3] = 1;
        v.normal[0] = data[3];
        v.normal[1] = data[4];
        v.normal[2] = data[5];
        for (unsigned k = 0; k < 4; ++k) {
            v.specular[k] = data[10 + k];
        }

        const uint32_t packed =
            uint32_t(data[6]) | uint32_t(data[7]) << 6 | uint32_t(data[8]) << 12 | uint32_t(data[9]) << 18;
        v.uv[1][0] = float(packed);
        v.uv[1][1] = 0;
        if (diffuse) {
            const uint8_t* c = reinterpret_cast<const uint8_t*>(memory.host_address(color_data + i * color_stride));
            const uint32_t value = uint32_t(c[0]) | uint32_t(c[1]) << 8 | uint32_t(c[2]) << 16 | uint32_t(c[3]) << 24;
            v.diffuse[0] = float((value >> 16) & 255) / 255;
            v.diffuse[1] = float((value >> 8) & 255) / 255;
            v.diffuse[2] = float(value & 255) / 255;
            v.diffuse[3] = float(value >> 24) / 255;
        } else {
            for (float& channel : v.diffuse) {
                channel = 1;
            }
        }

        if (coordinates) {
            const uint8_t* uv = reinterpret_cast<const uint8_t*>(memory.host_address(uv_data + i * uv_stride));
            v.uv[0][0] = scalar(uv);
            v.uv[0][1] = scalar(uv + 4);
        }
    }

    return true;
}
}
