#include "miles.h"
#include "../../platform/window_host.h"
#include "wav.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <strings.h>

namespace sacred {
namespace {
struct WavInfo {
    uint32_t base = 0, file_size = 0, format = 0, rate = 0, align = 0, per_block = 0, data = 0, data_len = 0,
             samples = 0;
    uint16_t bits = 0, channels = 0;
};
// Walks the RIFF chunks of a WAV image in guest memory.
bool parse_wav(Control& c, uint32_t base, WavInfo& info) {
    unsigned char head[12];
    c.memory.read(base, head, sizeof head);
    if (std::memcmp(head, "RIFF", 4) || std::memcmp(head + 8, "WAVE", 4)) {
        return false;
    }

    uint32_t riff;
    std::memcpy(&riff, head + 4, 4);
    const uint32_t limit = std::min<uint32_t>(riff + 8, 0x4000000u);
    info = WavInfo{};
    info.base = base;
    info.file_size = limit;
    for (uint32_t at = 12; at + 8 <= limit;) {
        unsigned char chunk[8];
        c.memory.read(base + at, chunk, 8);
        uint32_t size;
        std::memcpy(&size, chunk + 4, 4);
        if (!std::memcmp(chunk, "fmt ", 4) && size >= 16) {
            unsigned char fmt[24] = {};
            c.memory.read(base + at + 8, fmt, std::min<uint32_t>(size, sizeof fmt));
            uint16_t tag;
            std::memcpy(&tag, fmt, 2);
            std::memcpy(&info.channels, fmt + 2, 2);
            std::memcpy(&info.rate, fmt + 4, 4);
            uint16_t align;
            std::memcpy(&align, fmt + 12, 2);
            info.align = align;
            std::memcpy(&info.bits, fmt + 14, 2);
            if (size >= 20) {
                uint16_t spb;
                std::memcpy(&spb, fmt + 18, 2);
                info.per_block = spb;
            }

            info.format = tag;
        } else if (!std::memcmp(chunk, "data", 4)) {
            info.data = base + at + 8;
            info.data_len = std::min<uint32_t>(size, limit - at - 8);
            break;
        }

        at += 8 + size + (size & 1);
    }

    if (!info.format || !info.data || !info.channels) {
        return false;
    }

    if (info.format == 1) {
        info.samples = info.data_len / (uint32_t(info.channels) * std::max<uint32_t>(info.bits / 8, 1));
    } else if (info.per_block && info.align) {
        info.samples = info.data_len / info.align * info.per_block;
    } else {
        throw GuestFault("unqualified WAV format tag " + std::to_string(info.format));
    }

    return true;
}
}

void Miles::reg(const char* symbol, Signature signature, Handler handler) {
    control_.host(
        loader_.resolve("mss32.dll", symbol), signature,
        [this, handler](Control& control, const std::vector<uint64_t>& arguments) {
            deliver_events(control);
            return handler(control, arguments);
        },
        std::string("mss32.dll!") + symbol);
}

void Miles::deliver_events(Control& control) {
    if (!mixer_) {
        return;
    }

    for (const auto& event : mixer_->take_events()) {
        log_("QA_AUDIO_EOS handle=" + std::to_string(event.handle) +
             " tee_frames=" + std::to_string(mixer_->frames_mixed()));
        control.invoke(event.callback, {Convention::Stdcall, ReturnChannel::Void, {{4}}, ""}, {event.handle});
    }
}

std::string Miles::guest_string(Control& control, uint32_t address) {
    std::string text;
    for (char value = 1; text.size() < 1024;) {
        control.memory.read(address + uint32_t(text.size()), &value, 1);
        if (!value) {
            return text;
        }

        text.push_back(value);
    }

    throw GuestFault("unterminated guest string");
}

Miles::~Miles() {
    mixer_.reset();
    for (const auto& driver : drivers_) {
        host_->close_audio(driver.second);
    }

    if (initialized_) {
        host_->stop_audio();
    }
}

void Miles::bind() {
    reg("_AIL_WAV_info@8", {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4, PointerDirection::Out}}, ""},
        [](Control& c, const std::vector<uint64_t>& a) {
            // Parses the RIFF/WAVE header in guest memory into AILSOUNDINFO (9 words); 0 when it is not a WAV.
            if (!a[0]) {
                return Result{0, {}};
            }

            WavInfo info;
            if (!parse_wav(c, uint32_t(a[0]), info)) {
                return Result{0, {}};
            }

            const uint32_t out = uint32_t(a[1]);
            const uint32_t words[9] = {info.format,   info.data,    info.data_len, info.rate, info.bits,
                                       info.channels, info.samples, info.align,    info.base};
            for (int i = 0; i < 9; ++i) {
                c.memory.store32(out + 4 * uint32_t(i), words[i]);
            }

            return Result{1, {}};
        });
    reg("_AIL_file_type@8", {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}}, ""},
        [](Control& c, const std::vector<uint64_t>& a) {
            // AILFILETYPE: 1 PCM WAV, 2 ADPCM WAV, 3 other WAV, 0 unknown (confirmed against the game's branch on 2 =
            // ADPCM).
            if (!a[0]) {
                return Result{0, {}};
            }

            WavInfo info;
            if (!parse_wav(c, uint32_t(a[0]), info)) {
                return Result{0, {}};
            }

            return Result{info.format == 1 ? 1u : (info.format == 2 || info.format == 0x11 ? 2u : 3u), {}};
        });
    reg("_AIL_decompress_ADPCM@12",
        {Convention::Stdcall,
         ReturnChannel::Eax,
         {{4, PointerDirection::In}, {4, PointerDirection::Out}, {4, PointerDirection::Out}},
         ""},
        [](Control& c, const std::vector<uint64_t>& a) {
            // Decodes an ADPCM WAV into a newly allocated 16-bit PCM WAV image owned by the guest (freed by
            // AIL_mem_free_lock).
            if (!a[0]) {
                return Result{0, {}};
            }

            const uint32_t info = uint32_t(a[0]), base = c.memory.load32(info + 32);
            WavInfo header;
            if (!parse_wav(c, base, header)) {
                return Result{0, {}};
            }

            std::vector<uint8_t> file(header.file_size);
            c.memory.read(base, file.data(), file.size());
            std::vector<int16_t> pcm;
            unsigned channels, rate;
            if (!decode_wav_s16(file.data(), file.size(), pcm, channels, rate)) {
                throw GuestFault("unqualified ADPCM WAV image");
            }

            const uint32_t bytes = uint32_t(pcm.size() * 2), total = 44 + bytes;
            std::vector<uint8_t> image(total);
            auto put32 = [&](size_t at, uint32_t v) {
                std::memcpy(&image[at], &v, 4);
            };
            auto put16 = [&](size_t at, uint16_t v) {
                std::memcpy(&image[at], &v, 2);
            };
            std::memcpy(&image[0], "RIFF", 4);
            put32(4, total - 8);
            std::memcpy(&image[8], "WAVEfmt ", 8);
            put32(16, 16);
            put16(20, 1);
            put16(22, uint16_t(channels));
            put32(24, rate);
            put32(28, rate * channels * 2);
            put16(32, uint16_t(channels * 2));
            put16(34, 16);
            std::memcpy(&image[36], "data", 4);
            put32(40, bytes);
            std::memcpy(&image[44], pcm.data(), bytes);
            const uint32_t block = c.heap.allocate(total);
            c.memory.write(block, image.data(), image.size());
            if (a[1]) {
                c.memory.store32(uint32_t(a[1]), block);
            }

            if (a[2]) {
                c.memory.store32(uint32_t(a[2]), total);
            }

            return Result{1, {}};
        });
    reg("_AIL_mem_free_lock@4", {Convention::Stdcall, ReturnChannel::Void, {{4}}, ""},
        [](Control& c, const std::vector<uint64_t>& a) {
            if (a[0]) {
                c.heap.free(uint32_t(a[0]));
            }

            return Result{};
        });
    reg("_AIL_open_stream@12", {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4, PointerDirection::In}, {4}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            if (!a[0] || !a[1]) {
                return Result{0, {}};
            }

            if (!mixer_ || !drivers_.count(uint32_t(a[0]))) {
                throw GuestFault("invalid digital driver handle");
            }

            const std::string name = guest_string(c, uint32_t(a[1]));
            std::filesystem::path path;
            try {
                path = resolver_(name);
            } catch (const GuestFault&) {
                error_ = "stream file not found: " + name;
                return Result{0, {}};
            }

            std::ifstream in(path, std::ios::binary);
            if (!in) {
                error_ = "stream file not found: " + name;
                return Result{0, {}};
            }

            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            // Codec recognition from content, not the extension.
            if (!Mixer::is_mp3(bytes)) {
                throw GuestFault("unqualified stream codec: " + name);
            }

            std::string error;
            const uint32_t handle = mixer_->open_mp3_stream(std::move(bytes), error);
            if (!handle) {
                error_ = error + ": " + name;
                return Result{0, {}};
            }

            log_("QA_AUDIO_STREAM_OPEN handle=" + std::to_string(handle) + " file=" + name);
            return Result{handle, {}};
        });
    reg("_AIL_close_stream@4", {Convention::Stdcall, ReturnChannel::Void, {{4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            if (!a[0]) {
                return Result{};
            }

            if (!mixer_ || !mixer_->close(uint32_t(a[0]))) {
                throw GuestFault("invalid stream handle");
            }

            return Result{};
        });
    reg("_AIL_start_stream@4", {Convention::Stdcall, ReturnChannel::Void, {{4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            if (!a[0]) {
                return Result{};
            }

            if (!mixer_ || !mixer_->valid(uint32_t(a[0]))) {
                throw GuestFault("invalid stream handle");
            }

            mixer_->start(uint32_t(a[0]));
            log_("QA_AUDIO_STREAM_START handle=" + std::to_string(a[0]) +
                 " tee_frames=" + std::to_string(mixer_->frames_mixed()));
            return Result{};
        });
    reg("_AIL_pause_stream@8", {Convention::Stdcall, ReturnChannel::Void, {{4}, {4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            if (!a[0]) {
                return Result{};
            }

            if (!mixer_ || !mixer_->valid(uint32_t(a[0]))) {
                throw GuestFault("invalid stream handle");
            }

            const uint32_t status = mixer_->status(uint32_t(a[0]));
            if (a[1] && status == Mixer::Playing) {
                mixer_->set_status(uint32_t(a[0]), Mixer::Stopped);
            } else if (!a[1] && status == Mixer::Stopped) {
                mixer_->start(uint32_t(a[0]));
            }

            return Result{};
        });
    reg("_AIL_stream_status@4", {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            if (!a[0]) {
                return Result{0xffffffffu, {}};
            }

            if (!mixer_ || !mixer_->valid(uint32_t(a[0]))) {
                throw GuestFault("invalid stream handle");
            }

            return Result{mixer_->status(uint32_t(a[0])), {}};
        });
    reg("_AIL_set_stream_loop_count@8", {Convention::Stdcall, ReturnChannel::Void, {{4}, {4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            if (!a[0]) {
                return Result{};
            }

            if (!mixer_ || !mixer_->valid(uint32_t(a[0]))) {
                throw GuestFault("invalid stream handle");
            }

            mixer_->set_loops(uint32_t(a[0]), int32_t(a[1]));
            return Result{};
        });
    reg("_AIL_set_stream_volume_pan@12", {Convention::Stdcall, ReturnChannel::Void, {{4}, {4}, {4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            if (!a[0]) {
                return Result{};
            }

            if (!mixer_ || !mixer_->valid(uint32_t(a[0]))) {
                throw GuestFault("invalid stream handle");
            }

            const uint32_t volume_bits = uint32_t(a[1]), pan_bits = uint32_t(a[2]);
            float volume, pan;
            std::memcpy(&volume, &volume_bits, 4);
            std::memcpy(&pan, &pan_bits, 4);
            mixer_->set_volume_pan(uint32_t(a[0]), volume, pan);
            return Result{};
        });
    reg("_AIL_stream_volume_pan@12",
        {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4, PointerDirection::Out}, {4, PointerDirection::Out}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            if (!a[0]) {
                if (a[1]) {
                    c.memory.store32(uint32_t(a[1]), 0xbf800000u);
                }

                if (a[2]) {
                    c.memory.store32(uint32_t(a[2]), 0xbf800000u);
                }

                return Result{0, {}};
            }

            if (!mixer_ || !mixer_->valid(uint32_t(a[0]))) {
                throw GuestFault("invalid stream handle");
            }

            float volume, pan;
            mixer_->volume_pan(uint32_t(a[0]), volume, pan);
            uint32_t volume_bits, pan_bits;
            std::memcpy(&volume_bits, &volume, 4);
            std::memcpy(&pan_bits, &pan, 4);
            if (a[1]) {
                c.memory.store32(uint32_t(a[1]), volume_bits);
            }

            if (a[2]) {
                c.memory.store32(uint32_t(a[2]), pan_bits);
            }

            return Result{1, {}};
        });
    reg("_AIL_stream_ms_position@12",
        {Convention::Stdcall, ReturnChannel::Void, {{4}, {4, PointerDirection::Out}, {4, PointerDirection::Out}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            if (!a[0]) {
                return Result{};
            }

            if (!mixer_ || !mixer_->valid(uint32_t(a[0]))) {
                throw GuestFault("invalid stream handle");
            }

            int32_t total, current;
            mixer_->position_ms(uint32_t(a[0]), total, current);
            if (a[1]) {
                c.memory.store32(uint32_t(a[1]), uint32_t(total));
            }

            if (a[2]) {
                c.memory.store32(uint32_t(a[2]), uint32_t(current));
            }

            return Result{};
        });
    reg("_AIL_set_stream_ms_position@8", {Convention::Stdcall, ReturnChannel::Void, {{4}, {4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            if (!a[0]) {
                return Result{};
            }

            if (!mixer_ || !mixer_->valid(uint32_t(a[0]))) {
                throw GuestFault("invalid stream handle");
            }

            mixer_->seek_ms(uint32_t(a[0]), int32_t(a[1]));
            return Result{};
        });
    auto sample_pcm = [this](Control& c, uint32_t handle, uint32_t image, uint32_t size) {
        if (!mixer_ || !mixer_->valid(handle)) {
            throw GuestFault("invalid sample handle");
        }

        WavInfo info;
        if (!parse_wav(c, image, info)) {
            // Not RIFF: recognise an MP3 image from its bytes (ID3 tag or MPEG frame sync), as the ASI plugin did.
            std::vector<uint8_t> bytes(size);
            c.memory.read(image, bytes.data(), bytes.size());
            if (!Mixer::is_mp3(bytes)) {
                throw GuestFault("unqualified sample codec (neither RIFF WAVE nor MP3)");
            }

            std::vector<int16_t> stereo;
            unsigned mp3_hz;
            if (!Mixer::decode_mp3(bytes, stereo, mp3_hz)) {
                throw GuestFault("unqualified MP3 sample image");
            }

            mixer_->set_sample_pcm(handle, std::move(stereo), mp3_hz);
            return Result{1, {}};
        }

        std::vector<uint8_t> file(info.file_size);
        c.memory.read(image, file.data(), file.size());
        std::vector<int16_t> pcm;
        unsigned channels, hz;
        if (!decode_wav_s16(file.data(), file.size(), pcm, channels, hz)) {
            throw GuestFault("unqualified sample WAV image");
        }

        if (channels > 2) {
            throw GuestFault("unqualified sample channel count");
        }

        std::vector<int16_t> stereo;
        stereo.reserve(pcm.size() / channels * 2);
        for (size_t k = 0; k + channels <= pcm.size(); k += channels) {
            stereo.push_back(pcm[k]);
            stereo.push_back(channels > 1 ? pcm[k + 1] : pcm[k]);
        }

        mixer_->set_sample_pcm(handle, std::move(stereo), hz);
        return Result{1, {}};
    };
    reg("_AIL_init_sample@4", {Convention::Stdcall, ReturnChannel::Void, {{4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            if (!a[0]) {
                return Result{};
            }

            if (!mixer_ || !mixer_->valid(uint32_t(a[0]))) {
                throw GuestFault("invalid sample handle");
            }

            mixer_->init_sample(uint32_t(a[0]));
            return Result{};
        });
    reg("_AIL_set_sample_file@12", {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}, {4}}, ""},
        [sample_pcm](Control& c, const std::vector<uint64_t>& a) {
            if (!a[0] || !a[1]) {
                return Result{0, {}};
            }

            return sample_pcm(c, uint32_t(a[0]), uint32_t(a[1]), 0);
        });
    reg("_AIL_set_named_sample_file@20", {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}, {4}, {4}, {4}}, ""},
        [this, sample_pcm](Control& c, const std::vector<uint64_t>& a) {
            if (!a[0]) {
                // The original reaches the ASI provider path (ID3 tag or ".mp3" name) with a NULL sample and writes
                // through it; any other input falls through to AIL_set_sample_file, which returns 0 for a NULL sample.
                unsigned char tag[3] = {};
                if (a[2] && a[3] >= 3) {
                    c.memory.read(uint32_t(a[2]), tag, 3);
                }

                const std::string name = a[1] ? guest_string(c, uint32_t(a[1])) : std::string();
                const bool mp3_name = name.size() >= 4 && !strcasecmp(name.c_str() + name.size() - 4, ".mp3");
                if (!std::memcmp(tag, "ID3", 3) || mp3_name) {
                    throw GuestFault("AIL_set_named_sample_file writes through a NULL sample on the ASI path");
                }

                return Result{0, {}};
            }

            return sample_pcm(c, uint32_t(a[0]), uint32_t(a[2]), uint32_t(a[3]));
        });
    reg("_AIL_start_sample@4", {Convention::Stdcall, ReturnChannel::Void, {{4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            if (!a[0]) {
                return Result{};
            }

            if (!mixer_ || !mixer_->valid(uint32_t(a[0]))) {
                throw GuestFault("invalid sample handle");
            }

            mixer_->start(uint32_t(a[0]));
            log_("QA_AUDIO_SAMPLE_START handle=" + std::to_string(a[0]) +
                 " tee_frames=" + std::to_string(mixer_->frames_mixed()));
            return Result{};
        });
    reg("_AIL_stop_sample@4", {Convention::Stdcall, ReturnChannel::Void, {{4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            if (!a[0]) {
                return Result{};
            }

            if (!mixer_ || !mixer_->valid(uint32_t(a[0]))) {
                throw GuestFault("invalid sample handle");
            }

            if (mixer_->status(uint32_t(a[0])) == Mixer::Playing) {
                mixer_->set_status(uint32_t(a[0]), Mixer::Stopped);
            }

            return Result{};
        });
    reg("_AIL_end_sample@4", {Convention::Stdcall, ReturnChannel::Void, {{4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            if (!a[0]) {
                return Result{};
            }

            if (!mixer_ || !mixer_->valid(uint32_t(a[0]))) {
                throw GuestFault("invalid sample handle");
            }

            mixer_->set_status(uint32_t(a[0]), Mixer::Done);
            return Result{};
        });
    reg("_AIL_set_sample_loop_count@8", {Convention::Stdcall, ReturnChannel::Void, {{4}, {4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            if (!a[0]) {
                return Result{};
            }

            if (!mixer_ || !mixer_->valid(uint32_t(a[0]))) {
                throw GuestFault("invalid sample handle");
            }

            mixer_->set_loops(uint32_t(a[0]), int32_t(a[1]));
            return Result{};
        });
    reg("_AIL_set_sample_playback_rate@8", {Convention::Stdcall, ReturnChannel::Void, {{4}, {4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            if (!a[0]) {
                return Result{};
            }

            if (!mixer_ || !mixer_->valid(uint32_t(a[0]))) {
                throw GuestFault("invalid sample handle");
            }

            mixer_->set_rate(uint32_t(a[0]), int32_t(a[1]));
            return Result{};
        });
    reg("_AIL_sample_playback_rate@4", {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            if (!a[0]) {
                return Result{0, {}};
            }

            if (!mixer_ || !mixer_->valid(uint32_t(a[0]))) {
                throw GuestFault("invalid sample handle");
            }

            return Result{uint32_t(mixer_->playback_rate(uint32_t(a[0]))), {}};
        });
    reg("_AIL_sample_status@4", {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            if (!a[0]) {
                return Result{0, {}};
            }

            if (!mixer_ || !mixer_->valid(uint32_t(a[0]))) {
                throw GuestFault("invalid sample handle");
            }

            return Result{mixer_->status(uint32_t(a[0])), {}};
        });
    reg("_AIL_sample_volume_pan@12",
        {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4, PointerDirection::Out}, {4, PointerDirection::Out}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            if (!a[0]) {
                return Result{};
            }

            if (!mixer_ || !mixer_->valid(uint32_t(a[0]))) {
                throw GuestFault("invalid sample handle");
            }

            float volume, pan;
            mixer_->volume_pan(uint32_t(a[0]), volume, pan);
            uint32_t volume_bits, pan_bits;
            std::memcpy(&volume_bits, &volume, 4);
            std::memcpy(&pan_bits, &pan, 4);
            if (a[1]) {
                c.memory.store32(uint32_t(a[1]), volume_bits);
            }

            if (a[2]) {
                c.memory.store32(uint32_t(a[2]), pan_bits);
            }

            return Result{1, {}};
        });
    reg("_AIL_set_sample_volume_pan@12", {Convention::Stdcall, ReturnChannel::Void, {{4}, {4}, {4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            if (!a[0]) {
                return Result{};
            }

            if (!mixer_ || !mixer_->valid(uint32_t(a[0]))) {
                throw GuestFault("invalid sample handle");
            }

            const uint32_t volume_bits = uint32_t(a[1]), pan_bits = uint32_t(a[2]);
            float volume, pan;
            std::memcpy(&volume, &volume_bits, 4);
            std::memcpy(&pan, &pan_bits, 4);
            mixer_->set_volume_pan(uint32_t(a[0]), volume, pan);
            return Result{};
        });
    reg("_AIL_register_EOS_callback@8", {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            if (!a[0]) {
                return Result{0, {}};
            }

            if (!mixer_ || !mixer_->valid(uint32_t(a[0]))) {
                throw GuestFault("invalid sample handle");
            }

            return Result{mixer_->set_eos(uint32_t(a[0]), uint32_t(a[1])), {}};
        });
    reg("_AIL_release_sample_handle@4", {Convention::Stdcall, ReturnChannel::Void, {{4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            if (!a[0]) {
                return Result{};
            }

            if (!mixer_ || !mixer_->close(uint32_t(a[0]))) {
                throw GuestFault("invalid sample handle");
            }

            return Result{};
        });
    reg("_AIL_waveOutClose@4", {Convention::Stdcall, ReturnChannel::Void, {{4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            // NULL, or no driver open at all, returns before the original touches the handle.
            if (!a[0] || drivers_.empty()) {
                return Result{};
            }

            auto driver = drivers_.find(uint32_t(a[0]));
            if (driver == drivers_.end()) {
                throw GuestFault("invalid digital driver handle");
            }

            mixer_.reset();
            host_->close_audio(driver->second);
            drivers_.erase(driver);
            return Result{};
        });
    reg("_AIL_shutdown@0", {Convention::Stdcall, ReturnChannel::Void, {}, ""},
        [this](Control&, const std::vector<uint64_t>&) {
            if (!initialized_) {
                throw GuestFault("unqualified AIL_shutdown state");
            }

            mixer_.reset();
            for (const auto& driver : drivers_) {
                host_->close_audio(driver.second);
            }

            drivers_.clear();
            host_->stop_audio();
            initialized_ = false;
            return Result{};
        });
    reg("_AIL_allocate_sample_handle@4", {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            if (!a[0]) {
                return Result{0, {}};
            }

            if (!drivers_.count(uint32_t(a[0]))) {
                throw GuestFault("invalid digital driver handle");
            }

            if (!mixer_) {
                throw GuestFault("digital driver has no mixer");
            }

            const uint32_t handle = mixer_->create_sample();
            return Result{handle, {}};
        });
    reg("_AIL_enumerate_3D_providers@12",
        {Convention::Stdcall,
         ReturnChannel::Eax,
         {{4, PointerDirection::InOut}, {4, PointerDirection::Out}, {4, PointerDirection::Out}},
         ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            if (!initialized_ || (a[0] && c.memory.load32(uint32_t(a[0])))) {
                throw GuestFault("unqualified 3D provider enumeration cursor");
            }

            // Only the real SDL stereo PCM driver exists.
            // Do not advertise a spatial provider or load Windows .m3d plugins.
            return Result{0, {}};
        });
    reg("_AIL_open_digital_driver@16", {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}, {4}, {4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            if (!initialized_ || a[0] != 44100 || a[1] != 16 || a[2] != 2 || a[3] != 1) {
                throw GuestFault("unqualified digital driver format or flags");
            }

            error_.clear();
            uint32_t device = host_->open_audio(uint32_t(a[0]), error_);
            if (!device) {
                return Result{};
            }

            uint32_t handle = next_driver_++;
            drivers_.emplace(handle, device);
            if (!mixer_) {
                mixer_device_ = device;
                mixer_ = std::make_unique<Mixer>(
                    [this, device](const int16_t* data, size_t frames) {
                        host_->queue_audio(device, data, uint32_t(frames * 4));
                    },
                    [this, device] { return size_t(host_->queued_audio(device) / 4); }, tee_);
                log_("QA_AUDIO_MIXER device=" + std::to_string(device) + " rate=44100 channels=2");
            }

            return Result{handle, {}};
        });
    reg("_AIL_set_preference@8", {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            // DIG_USE_WAVEOUT=0 selects the native mixer path. A request for
            // the foreign WinMM driver is not supported by this adapter.
            if (!initialized_ || a[0] != 15 || a[1] != 0) {
                throw GuestFault("unqualified Miles preference");
            }

            return Result{0, {}};
        });
    reg("_AIL_startup@0", {Convention::Stdcall, ReturnChannel::Eax, {}, ""},
        [this](Control&, const std::vector<uint64_t>&) {
            if (initialized_) {
                throw GuestFault("unqualified repeated AIL_startup");
            }

            if (!host_) {
                throw GuestFault("AIL_startup requires the SDL host");
            }

            error_ = host_->start_audio();
            initialized_ = error_.empty();
            return Result{initialized_ ? 1u : 0u, {}};
        });
}
}
