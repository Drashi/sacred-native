#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sacred {
// FFmpeg-backed WMV/WMA reader (libavformat/libavcodec/libswscale/libswresample, dynamically linked). Video and
// audio are demuxed through independent contexts so neither stream needs packet queues. No SDL or OS-specific API.
struct MovieInfo {
    int width = 0, height = 0;
    double fps = 0;      // 0 when the container does not declare one
    double duration = 0; // seconds
    std::string video_codec, audio_codec;
    bool has_audio = false;
};
struct VideoFrame {
    double pts = 0; // seconds from the start of the movie
    int width = 0, height = 0;
    std::vector<uint8_t> bgra; // width * height * 4, B,G,R,A byte order (D3D X8R8G8B8 on little endian)
};
class MovieDecoder {
public:
    static constexpr unsigned audio_rate = 44100; // decode_audio output: interleaved stereo signed 16-bit
    ~MovieDecoder();
    // Returns null and sets `error` when the file cannot be opened or a codec is missing.
    static std::unique_ptr<MovieDecoder> open(const std::string& path, std::string& error);
    const MovieInfo& info() const;
    // Next video frame scaled to dst_width x dst_height (0 = native); false at the end of the stream.
    bool next_video(VideoFrame& frame, int dst_width = 0, int dst_height = 0);
    // Decodes the whole audio track to stereo audio_rate; false when there is no audio track.
    bool decode_audio(std::vector<int16_t>& stereo);
    // Packets or frames the decoders rejected so far (other than normal end/again conditions).
    unsigned decode_errors() const;

private:
    MovieDecoder();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
