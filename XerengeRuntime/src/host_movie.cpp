#include "host_movie.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include "xbox_media.h"

extern XboxMedia gXboxMedia;

HostMovie gHostMovie;

struct HostMovie::State
{
    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    SwsContext* scaler = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    int stream = -1;
    uint32_t width = 0;
    uint32_t height = 0;
    double timeBase = 0.0;
    // Wall clock the clip started on, so frames are presented at the rate the
    // clip was authored at rather than as fast as the renderer happens to run.
    std::chrono::steady_clock::time_point began{};
    bool begun = false;
    bool ended = false;
    uint64_t generation = 0;
    std::vector<uint8_t> rgba;
};

HostMovie::~HostMovie()
{
    close();
}

void HostMovie::closeLocked()
{
    if (state_ == nullptr)
        return;
    if (state_->scaler != nullptr)
        sws_freeContext(state_->scaler);
    if (state_->frame != nullptr)
        av_frame_free(&state_->frame);
    if (state_->packet != nullptr)
        av_packet_free(&state_->packet);
    if (state_->codec != nullptr)
        avcodec_free_context(&state_->codec);
    if (state_->format != nullptr)
        avformat_close_input(&state_->format);
    delete state_;
    state_ = nullptr;
}

void HostMovie::close()
{
    std::lock_guard lock(mutex_);
    closeLocked();
}

bool HostMovie::active() const
{
    std::lock_guard lock(mutex_);
    return state_ != nullptr && !state_->ended;
}

bool HostMovie::finished() const
{
    std::lock_guard lock(mutex_);
    return state_ != nullptr && state_->ended;
}

bool HostMovie::open(const std::string& path)
{
    std::lock_guard lock(mutex_);
    closeLocked();

    auto state = new State();
    if (avformat_open_input(&state->format, path.c_str(), nullptr, nullptr) != 0 ||
        avformat_find_stream_info(state->format, nullptr) < 0)
    {
        std::cerr << "host movie: could not open " << path << '\n';
        delete state;
        return false;
    }

    const AVCodec* decoder = nullptr;
    state->stream = av_find_best_stream(state->format, AVMEDIA_TYPE_VIDEO, -1, -1,
        &decoder, 0);
    if (state->stream < 0 || decoder == nullptr)
    {
        std::cerr << "host movie: no video stream in " << path << '\n';
        avformat_close_input(&state->format);
        delete state;
        return false;
    }

    state->codec = avcodec_alloc_context3(decoder);
    const AVStream* stream = state->format->streams[state->stream];
    if (state->codec == nullptr ||
        avcodec_parameters_to_context(state->codec, stream->codecpar) < 0)
    {
        std::cerr << "host movie: could not prepare the decoder for " << path << '\n';
        closeLocked();
        delete state;
        return false;
    }
    // Let the decoder use the cores that are going spare; the recompiled guest
    // code is what the single-thread budget is spent on.
    state->codec->thread_count = 0;
    if (avcodec_open2(state->codec, decoder, nullptr) < 0)
    {
        std::cerr << "host movie: could not start the decoder for " << path << '\n';
        avcodec_free_context(&state->codec);
        avformat_close_input(&state->format);
        delete state;
        return false;
    }

    state->width = uint32_t(state->codec->width);
    state->height = uint32_t(state->codec->height);
    state->timeBase = av_q2d(stream->time_base);
    state->frame = av_frame_alloc();
    state->packet = av_packet_alloc();
    if (state->frame == nullptr || state->packet == nullptr || state->width == 0 ||
        state->height == 0)
    {
        closeLocked();
        delete state;
        return false;
    }

    state_ = state;
    std::cerr << "host movie: playing " << path << ' ' << state->width << 'x'
              << state->height << '\n';
    return true;
}

bool HostMovie::nextFrame(std::vector<uint8_t>& rgba, uint32_t& width,
    uint32_t& height, uint64_t& generation)
{
    std::lock_guard lock(mutex_);
    if (state_ == nullptr || state_->ended)
        return false;

    if (!state_->begun)
    {
        state_->begun = true;
        state_->began = std::chrono::steady_clock::now();
    }
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - state_->began).count();

    // Decode forward until the frame that belongs on screen now. Frames that
    // are already late are decoded and discarded rather than shown, so a slow
    // renderer costs dropped frames instead of a clip that plays in slow
    // motion and drifts away from its own sound.
    bool decoded = false;
    for (;;)
    {
        int received = avcodec_receive_frame(state_->codec, state_->frame);
        if (received == 0)
        {
            const int64_t stamp = state_->frame->best_effort_timestamp;
            const double when = stamp == AV_NOPTS_VALUE
                ? elapsed : double(stamp) * state_->timeBase;
            decoded = true;
            if (when >= elapsed)
                break;
            // Late: keep it only as the newest thing we have, and look for a
            // more current one.
            continue;
        }
        if (received != AVERROR(EAGAIN) && received != AVERROR_EOF)
        {
            state_->ended = true;
            return false;
        }
        if (received == AVERROR_EOF)
        {
            state_->ended = true;
            break;
        }
        const int read = av_read_frame(state_->format, state_->packet);
        if (read < 0)
        {
            avcodec_send_packet(state_->codec, nullptr);  // flush
            continue;
        }
        if (state_->packet->stream_index == state_->stream)
            avcodec_send_packet(state_->codec, state_->packet);
        av_packet_unref(state_->packet);
    }

    if (!decoded)
        return false;

    const uint32_t w = state_->width;
    const uint32_t h = state_->height;
    state_->scaler = sws_getCachedContext(state_->scaler,
        state_->frame->width, state_->frame->height,
        AVPixelFormat(state_->frame->format), int(w), int(h), AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (state_->scaler == nullptr)
        return false;

    state_->rgba.resize(size_t(w) * h * 4u);
    uint8_t* planes[4] = {state_->rgba.data(), nullptr, nullptr, nullptr};
    int strides[4] = {int(w) * 4, 0, 0, 0};
    sws_scale(state_->scaler, state_->frame->data, state_->frame->linesize, 0,
        state_->frame->height, planes, strides);

    rgba = state_->rgba;
    width = w;
    height = h;
    generation = ++state_->generation;
    return true;
}

std::string hostMoviePathForClip(const std::string& clipName)
{
    if (clipName.empty())
        return {};
    std::string lowered;
    lowered.reserve(clipName.size());
    for (const char c : clipName)
        lowered.push_back(char(std::tolower(static_cast<unsigned char>(c))));
    // The descriptor names the clip; the title keeps every movie in one
    // directory under that name.
    return gXboxMedia.hostPath("ovid/" + lowered + ".xmv");
}
