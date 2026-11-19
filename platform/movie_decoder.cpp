#include "movie_decoder.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#include <cmath>

namespace sacred {
namespace {
std::string av_text(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, text, sizeof text);
    return text;
}

struct Track {
    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    AVStream* stream = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    bool flushing = false, finished = false;
    ~Track() {
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&codec);
        avformat_close_input(&format);
    }
};
bool open_track(Track& track, const std::string& path, AVMediaType type, std::string& error, bool required) {
    int code = avformat_open_input(&track.format, path.c_str(), nullptr, nullptr);
    if (code < 0) {
        error = "cannot open " + path + ": " + av_text(code);
        return false;
    }

    code = avformat_find_stream_info(track.format, nullptr);
    if (code < 0) {
        error = "no stream info: " + av_text(code);
        return false;
    }

    int index = av_find_best_stream(track.format, type, -1, -1, nullptr, 0);
    if (index < 0) {
        if (required) {
            error = std::string("no ") + av_get_media_type_string(type) + " stream";
        }

        return false;
    }

    track.stream = track.format->streams[index];
    const AVCodec* codec = avcodec_find_decoder(track.stream->codecpar->codec_id);
    if (!codec) {
        error = std::string("no decoder for ") + avcodec_get_name(track.stream->codecpar->codec_id);
        return false;
    }

    track.codec = avcodec_alloc_context3(codec);
    if (!track.codec || avcodec_parameters_to_context(track.codec, track.stream->codecpar) < 0) {
        error = "codec context setup failed";
        return false;
    }

    code = avcodec_open2(track.codec, codec, nullptr);
    if (code < 0) {
        error = "cannot open decoder: " + av_text(code);
        return false;
    }

    track.packet = av_packet_alloc();
    track.frame = av_frame_alloc();
    return track.packet && track.frame;
}

double origin(const Track& track) {
    return track.format->start_time == AV_NOPTS_VALUE ? 0.0 : double(track.format->start_time) / AV_TIME_BASE;
}
}

struct MovieDecoder::Impl {
    MovieInfo info;
    Track video, audio;
    SwsContext* scaler = nullptr;
    int scaler_format = -1, scaler_width = 0, scaler_height = 0, scaler_out_w = 0, scaler_out_h = 0;
    unsigned errors = 0;
    double last_pts = -1, frame_duration = 0.04;
    ~Impl() { sws_freeContext(scaler); }
    // Pulls the next decoded frame of `track`; false once the stream is exhausted.
    bool receive(Track& track) {
        for (;;) {
            if (track.finished) {
                return false;
            }

            int code = avcodec_receive_frame(track.codec, track.frame);
            if (code >= 0) {
                return true;
            }

            if (code == AVERROR_EOF) {
                track.finished = true;
                return false;
            }

            if (code != AVERROR(EAGAIN)) {
                ++errors;
                continue;
            }

            if (track.flushing) {
                track.finished = true;
                return false;
            }

            int read = av_read_frame(track.format, track.packet);
            if (read < 0) {
                track.flushing = true;
                avcodec_send_packet(track.codec, nullptr);
                continue;
            }

            if (track.packet->stream_index == track.stream->index &&
                avcodec_send_packet(track.codec, track.packet) < 0) {
                ++errors;
            }

            av_packet_unref(track.packet);
        }
    }
};
MovieDecoder::MovieDecoder() : impl_(new Impl) {
}

MovieDecoder::~MovieDecoder() = default;
unsigned MovieDecoder::decode_errors() const {
    return impl_->errors;
}

const MovieInfo& MovieDecoder::info() const {
    return impl_->info;
}

std::unique_ptr<MovieDecoder> MovieDecoder::open(const std::string& path, std::string& error) {
    av_log_set_level(AV_LOG_ERROR);
    std::unique_ptr<MovieDecoder> movie(new MovieDecoder);
    auto& impl = *movie->impl_;
    if (!open_track(impl.video, path, AVMEDIA_TYPE_VIDEO, error, true)) {
        return nullptr;
    }

    std::string audio_error;
    impl.info.has_audio = open_track(impl.audio, path, AVMEDIA_TYPE_AUDIO, audio_error, false);
    if (!impl.info.has_audio && !audio_error.empty()) {
        error = audio_error;
        return nullptr;
    }

    auto* parameters = impl.video.stream->codecpar;
    impl.info.width = parameters->width;
    impl.info.height = parameters->height;
    AVRational rate = impl.video.stream->avg_frame_rate;
    if (rate.num > 0 && rate.den > 0) {
        impl.info.fps = av_q2d(rate);
        impl.frame_duration = 1.0 / impl.info.fps;
    }

    if (impl.video.format->duration > 0) {
        impl.info.duration = double(impl.video.format->duration) / AV_TIME_BASE;
    }

    impl.info.video_codec = avcodec_get_name(parameters->codec_id);
    if (impl.info.has_audio) {
        impl.info.audio_codec = avcodec_get_name(impl.audio.stream->codecpar->codec_id);
    }

    return movie;
}

bool MovieDecoder::next_video(VideoFrame& out, int dst_width, int dst_height) {
    auto& impl = *impl_;
    if (!impl.receive(impl.video)) {
        return false;
    }

    AVFrame* frame = impl.video.frame;
    int64_t stamp = frame->best_effort_timestamp;
    double pts = impl.last_pts < 0 ? 0 : impl.last_pts + impl.frame_duration;
    if (stamp != AV_NOPTS_VALUE) {
        pts = double(stamp) * av_q2d(impl.video.stream->time_base) - origin(impl.video);
    }

    if (pts < 0) {
        pts = 0;
    }

    if (impl.last_pts >= 0 && pts > impl.last_pts && pts - impl.last_pts < 1.0) {
        impl.frame_duration = pts - impl.last_pts;
    }

    impl.last_pts = pts;
    int width = dst_width ? dst_width : frame->width, height = dst_height ? dst_height : frame->height;
    if (!impl.scaler || impl.scaler_format != frame->format || impl.scaler_width != frame->width ||
        impl.scaler_height != frame->height || impl.scaler_out_w != width || impl.scaler_out_h != height) {
        sws_freeContext(impl.scaler);
        impl.scaler = sws_getContext(frame->width, frame->height, AVPixelFormat(frame->format), width, height,
                                     AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!impl.scaler) {
            ++impl.errors;
            av_frame_unref(frame);
            return false;
        }

        impl.scaler_format = frame->format;
        impl.scaler_width = frame->width;
        impl.scaler_height = frame->height;
        impl.scaler_out_w = width;
        impl.scaler_out_h = height;
    }

    out.pts = pts;
    out.width = width;
    out.height = height;
    out.bgra.resize(size_t(width) * height * 4);
    uint8_t* planes[4] = {out.bgra.data(), nullptr, nullptr, nullptr};
    int strides[4] = {width * 4, 0, 0, 0};
    sws_scale(impl.scaler, frame->data, frame->linesize, 0, frame->height, planes, strides);
    av_frame_unref(frame);
    return true;
}

bool MovieDecoder::decode_audio(std::vector<int16_t>& stereo) {
    auto& impl = *impl_;
    if (!impl.info.has_audio) {
        return false;
    }

    SwrContext* resampler = nullptr;
    AVChannelLayout layout;
    av_channel_layout_default(&layout, 2);
    std::vector<uint8_t> scratch;
    auto drain = [&](const uint8_t** input, int count) {
        int capacity = swr_get_out_samples(resampler, count);
        if (capacity <= 0) {
            return;
        }

        size_t before = stereo.size();
        stereo.resize(before + size_t(capacity) * 2);
        uint8_t* output = reinterpret_cast<uint8_t*>(stereo.data() + before);
        int produced = swr_convert(resampler, &output, capacity, input, count);
        if (produced < 0) {
            ++impl.errors;
            produced = 0;
        }

        stereo.resize(before + size_t(produced) * 2);
    };
    while (impl.receive(impl.audio)) {
        AVFrame* frame = impl.audio.frame;
        if (!resampler) {
            if (swr_alloc_set_opts2(&resampler, &layout, AV_SAMPLE_FMT_S16, audio_rate, &frame->ch_layout,
                                    AVSampleFormat(frame->format), frame->sample_rate, 0, nullptr) < 0 ||
                swr_init(resampler) < 0) {
                ++impl.errors;
                swr_free(&resampler);
                av_channel_layout_uninit(&layout);
                return false;
            }
        }

        drain(const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
        av_frame_unref(frame);
    }

    if (resampler) {
        drain(nullptr, 0);
        swr_free(&resampler);
    }

    av_channel_layout_uninit(&layout);
    return true;
}
}
