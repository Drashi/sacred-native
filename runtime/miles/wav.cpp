#include "wav.h"

#define DR_WAV_NO_STDIO
#define DR_WAV_IMPLEMENTATION
#include <dr_wav.h>

namespace sacred {
bool decode_wav_s16(const uint8_t* data, size_t size, std::vector<int16_t>& pcm, unsigned& channels, unsigned& rate) {
    drwav wav;
    if (!drwav_init_memory(&wav, data, size, nullptr)) {
        return false;
    }

    channels = wav.channels;
    rate = wav.sampleRate;
    pcm.assign(size_t(wav.totalPCMFrameCount) * wav.channels, 0);
    const drwav_uint64 got = drwav_read_pcm_frames_s16(&wav, wav.totalPCMFrameCount, pcm.data());
    pcm.resize(size_t(got) * wav.channels);
    drwav_uninit(&wav);
    return true;
}
}
