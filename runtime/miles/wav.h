#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sacred {
// Decodes a complete RIFF/WAVE image (PCM, MS-ADPCM, IMA-ADPCM) to interleaved signed 16-bit PCM.
bool decode_wav_s16(const uint8_t* data, size_t size, std::vector<int16_t>& pcm, unsigned& channels, unsigned& rate);
}
