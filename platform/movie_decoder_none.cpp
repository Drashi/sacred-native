#include "movie_decoder.h"

// The movie reader of a build without FFmpeg (-DSACRED_MOVIES=OFF): no file opens, so the game takes its documented
// skip path for every movie (runtime/ole/com.cpp: MOVIE_SKIPPED, the game continues as if the movie had ended).
namespace sacred {
struct MovieDecoder::Impl {
    MovieInfo info;
};
MovieDecoder::MovieDecoder() : impl_(new Impl) {
}

MovieDecoder::~MovieDecoder() = default;
unsigned MovieDecoder::decode_errors() const {
    return 0;
}

const MovieInfo& MovieDecoder::info() const {
    return impl_->info;
}

std::unique_ptr<MovieDecoder> MovieDecoder::open(const std::string&, std::string& error) {
    error = "built without movie support (SACRED_MOVIES=OFF)";
    return nullptr;
}

bool MovieDecoder::next_video(VideoFrame&, int, int) {
    return false;
}

bool MovieDecoder::decode_audio(std::vector<int16_t>&) {
    return false;
}
}
