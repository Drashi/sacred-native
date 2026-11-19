// Offscreen decode of every shipped movie: frame count, timestamps, duration, audio samples and decoder errors.
#include "../../platform/movie_decoder.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace {
int failures = 0;
void check(bool ok, const char* movie, const char* what) {
    if (!ok) {
        std::printf("FAIL %s: %s\n", movie, what);
        ++failures;
    }
}
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: movie_decode_tests ASSETS\n");
        return 2;
    }

    const char* names[] = {"Intro.wmv",   "act1.wmv",  "act2.wmv",    "act3.wmv",    "act4.wmv",
                           "ascaron.wmv", "Extro.wmv", "extrouw.wmv", "introuw.wmv", "oem.wmv"};
    for (const char* name : names) {
        std::string path = (std::filesystem::path(argv[1]) / "movie" / name).string(), error;
        auto movie = sacred::MovieDecoder::open(path, error);
        if (!movie) {
            std::printf("FAIL %s: open: %s\n", name, error.c_str());
            ++failures;
            continue;
        }

        const auto& info = movie->info();
        std::vector<int16_t> audio;
        bool has_audio = movie->decode_audio(audio);
        sacred::VideoFrame frame;
        unsigned frames = 0;
        double first = -1, last = -1, previous = -1, worst_gap = 0;
        bool monotonic = true;
        uint64_t checksum = 0;
        while (movie->next_video(frame)) {
            if (first < 0) {
                first = frame.pts;
            }

            if (previous >= 0) {
                monotonic &= frame.pts > previous;
                worst_gap = std::fmax(worst_gap, frame.pts - previous);
            }

            previous = last = frame.pts;
            if (frame.bgra.size() != size_t(info.width) * info.height * 4) {
                monotonic = false;
            }

            checksum += frame.bgra[frame.bgra.size() / 2] + frame.bgra[7];
            ++frames;
        }

        double audio_seconds = double(audio.size() / 2) / sacred::MovieDecoder::audio_rate;
        double energy = 0;
        for (int16_t sample : audio) {
            energy += double(sample) * sample;
        }

        double rms = audio.empty() ? 0 : std::sqrt(energy / double(audio.size()));
        std::printf("%-12s %s %dx%d fps=%.2f duration=%.3f video_frames=%u first=%.3f last=%.3f max_gap=%.3f "
                    "%s audio_frames=%zu audio_seconds=%.3f rms=%.1f decode_errors=%u\n",
                    name, info.video_codec.c_str(), info.width, info.height, info.fps, info.duration, frames, first,
                    last, worst_gap, info.audio_codec.c_str(), audio.size() / 2, audio_seconds, rms,
                    movie->decode_errors());
        check(info.width == 640 && info.height == 480, name, "unexpected frame size");
        check(frames > 100, name, "too few video frames");
        check(monotonic, name, "video timestamps not strictly increasing or frame size wrong");
        check(last <= info.duration + 0.5 && last >= info.duration - 1.0, name,
              "last frame far from container duration");
        check(has_audio && audio_seconds > info.duration - 1.0 && audio_seconds < info.duration + 1.0, name,
              "audio length differs from duration");
        check(rms > 50.0, name, "audio is silent");
        check(movie->decode_errors() == 0, name, "decoder reported errors");
        check(checksum != 0, name, "video frames are all black");
    }

    std::printf("%s\n", failures ? "MOVIE_DECODE_FAIL" : "MOVIE_DECODE_OK");
    return failures ? 1 : 0;
}
