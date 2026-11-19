#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace sacred {
// SHA-256 (FIPS 180-4) of a byte range; our own implementation, no crypto library.
std::array<uint8_t, 32> host_sha256(const uint8_t* bytes, size_t size);
}
