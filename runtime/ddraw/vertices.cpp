#include "vertices.h"
#include <cstring>

namespace sacred {
namespace {
uint32_t word(const uint8_t* value) {
    return uint32_t(value[0]) | uint32_t(value[1]) << 8 | uint32_t(value[2]) << 16 | uint32_t(value[3]) << 24;
}

float scalar(const uint8_t* value) {
    uint32_t bits = word(value);
    float result;
    std::memcpy(&result, &bits, sizeof result);
    return result;
}

void color(float* result, uint32_t value) {
    result[0] = float((value >> 16) & 255) / 255;
    result[1] = float((value >> 8) & 255) / 255;
    result[2] = float(value & 255) / 255;
    result[3] = float(value >> 24) / 255;
}
}

void decode_vertices(const Memory& memory, uint32_t fvf, uint32_t address, uint32_t count, DecodedVertices& result) {
    uint32_t position = fvf & 0xe;
    bool normal = fvf & 0x10, reserved = fvf & 0x20;
    result.vertices.clear();
    result.transformed = position == 4;
    result.diffuse = fvf & 0x40;
    result.specular = fvf & 0x80;
    result.coordinates = (fvf >> 8) & 15;
    if ((fvf & ~0xffeu) || (position != 2 && position != 4) || result.coordinates > 2 ||
        (result.transformed && (normal || reserved)) || (normal && reserved)) {
        throw GuestFault("unqualified D3D7 FVF");
    }

    unsigned stride = result.transformed ? 16 : 12;
    stride += (normal ? 12 : 0) + (reserved ? 4 : 0) + (result.diffuse ? 4 : 0) + (result.specular ? 4 : 0) +
              result.coordinates * 8;
    if (!count) {
        return;
    }

    uint64_t length = uint64_t(stride) * count;
    memory.check(address, length, Read);
    static thread_local std::vector<uint8_t> bytes;
    bytes.resize(length);
    memory.read(address, bytes.data(), bytes.size());
    result.vertices.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* input = bytes.data() + size_t(i) * stride;
        auto& output = result.vertices[i];
        output.position[3] = 1;
        for (unsigned n = 0; n < (result.transformed ? 4u : 3u); ++n, input += 4) {
            output.position[n] = scalar(input);
        }

        if (normal) {
            for (unsigned n = 0; n < 3; ++n, input += 4) {
                output.normal[n] = scalar(input);
            }
        }

        if (reserved) {
            input += 4; // D3DLVERTEX's dwReserved is present in its FVF stride.
        }

        for (float& channel : output.diffuse) {
            channel = 1;
        }

        if (result.diffuse) {
            color(output.diffuse, word(input));
            input += 4;
        }

        if (result.specular) {
            color(output.specular, word(input));
            input += 4;
        }

        for (unsigned stage = 0; stage < result.coordinates; ++stage) {
            for (unsigned n = 0; n < 2; ++n, input += 4) {
                output.uv[stage][n] = scalar(input);
            }
        }
    }
}

void decode_strided(const Memory& memory, uint32_t fvf, uint32_t address, uint32_t count, DecodedVertices& result) {
    const bool normal = fvf & 0x10;
    result.vertices.clear();
    result.transformed = false;
    result.diffuse = fvf & 0x40;
    result.specular = fvf & 0x80;
    result.coordinates = (fvf >> 8) & 15;
    if ((fvf & ~0xff2u) || (fvf & 0xe) != 2 || result.coordinates > 2) {
        throw GuestFault("unqualified D3D7 strided FVF");
    }

    memory.check(address, 96, Read);
    auto stream = [&](unsigned index, unsigned size) {
        const uint32_t data = memory.load32(address + index * 8), stride = memory.load32(address + index * 8 + 4);
        if (!data || (stride && stride < size)) {
            throw GuestFault("invalid D3D7 strided stream " + std::to_string(index) + " data=" + std::to_string(data) +
                             " stride=" + std::to_string(stride));
        }

        memory.check(data, uint64_t(stride) * (count - 1) + size, Read);
        return std::pair<uint32_t, uint32_t>{data, stride};
    };
    result.vertices.resize(count);
    if (!count) {
        return;
    }

    auto read_floats = [&](std::pair<uint32_t, uint32_t> source, uint32_t i, float* output, unsigned n) {
        uint8_t bytes[12];
        memory.read(source.first + i * source.second, bytes, n * 4);
        for (unsigned k = 0; k < n; ++k) {
            output[k] = scalar(bytes + k * 4);
        }
    };
    const auto position = stream(0, 12);
    const auto normals = normal ? stream(1, 12) : std::pair<uint32_t, uint32_t>{};
    const auto diffuse = result.diffuse ? stream(2, 4) : std::pair<uint32_t, uint32_t>{};
    const auto specular = result.specular ? stream(3, 4) : std::pair<uint32_t, uint32_t>{};
    std::pair<uint32_t, uint32_t> coordinates[2];
    for (unsigned stage = 0; stage < result.coordinates; ++stage) {
        coordinates[stage] = stream(4 + stage, 8);
    }

    for (uint32_t i = 0; i < count; ++i) {
        auto& output = result.vertices[i];
        output.position[3] = 1;
        read_floats(position, i, output.position, 3);
        if (normal) {
            read_floats(normals, i, output.normal, 3);
        }

        for (float& channel : output.diffuse) {
            channel = 1;
        }

        if (result.diffuse) {
            color(output.diffuse, memory.load32(diffuse.first + i * diffuse.second));
        }

        if (result.specular) {
            color(output.specular, memory.load32(specular.first + i * specular.second));
        }

        for (unsigned stage = 0; stage < result.coordinates; ++stage) {
            read_floats(coordinates[stage], i, output.uv[stage], 2);
        }
    }
}

void decode_indices(const Memory& memory, uint32_t address, uint32_t count, uint32_t vertices,
                    std::vector<uint16_t>& result) {
    result.clear();
    if (!count) {
        return;
    }

    memory.check(address, uint64_t(count) * 2, Read);
    static thread_local std::vector<uint8_t> bytes;
    bytes.resize(size_t(count) * 2);
    memory.read(address, bytes.data(), bytes.size());
    result.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        result[i] = uint16_t(bytes[i * 2]) | uint16_t(bytes[i * 2 + 1]) << 8;
        if (result[i] >= vertices) {
            throw GuestFault("D3D index exceeds vertex count");
        }
    }
}

DecodedVertices decode_vertices(const Memory& memory, uint32_t fvf, uint32_t address, uint32_t count) {
    DecodedVertices result;
    decode_vertices(memory, fvf, address, count, result);
    return result;
}

DecodedVertices decode_strided(const Memory& memory, uint32_t fvf, uint32_t address, uint32_t count) {
    DecodedVertices result;
    decode_strided(memory, fvf, address, count, result);
    return result;
}

std::vector<uint16_t> decode_indices(const Memory& memory, uint32_t address, uint32_t count, uint32_t vertices) {
    std::vector<uint16_t> result;
    decode_indices(memory, address, count, vertices, result);
    return result;
}
}
