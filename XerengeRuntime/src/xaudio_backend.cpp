#include "xaudio_backend.h"

#include <alsa/asoundlib.h>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace
{
constexpr unsigned kRate = 48000;
constexpr unsigned kFrames = 256;

float guestFloat(const uint8_t* base, uint32_t address)
{
    uint32_t bits = 0;
    std::memcpy(&bits, base + address, sizeof(bits));
    bits = __builtin_bswap32(bits);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
}

struct XAudioBackend::State
{
    snd_pcm_t* pcm = nullptr;
};

XAudioBackend::XAudioBackend() : state_(std::make_unique<State>()) {}

XAudioBackend::~XAudioBackend()
{
    if (state_->pcm != nullptr)
    {
        snd_pcm_drain(state_->pcm);
        snd_pcm_close(state_->pcm);
    }
}

void XAudioBackend::start()
{
    const char* enabled = std::getenv("XERENGE_AUDIO");
    if (enabled != nullptr && std::strcmp(enabled, "0") == 0)
        return;
    const char* device = std::getenv("XERENGE_AUDIO_DEVICE");
    if (device == nullptr)
        device = "default";
    if (snd_pcm_open(&state_->pcm, device, SND_PCM_STREAM_PLAYBACK,
                     SND_PCM_NONBLOCK) < 0)
        return;

    snd_pcm_hw_params_t* params = nullptr;
    snd_pcm_hw_params_malloc(&params);
    snd_pcm_hw_params_any(state_->pcm, params);
    snd_pcm_hw_params_set_access(state_->pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(state_->pcm, params, SND_PCM_FORMAT_FLOAT_LE);
    snd_pcm_hw_params_set_channels(state_->pcm, params, 2);
    unsigned rate = kRate;
    int direction = 0;
    snd_pcm_hw_params_set_rate_near(state_->pcm, params, &rate, &direction);
    snd_pcm_uframes_t period = kFrames;
    snd_pcm_hw_params_set_period_size_near(state_->pcm, params, &period, &direction);
    const int result = snd_pcm_hw_params(state_->pcm, params);
    snd_pcm_hw_params_free(params);
    if (result < 0 || rate != kRate)
    {
        snd_pcm_close(state_->pcm);
        state_->pcm = nullptr;
        return;
    }
    snd_pcm_prepare(state_->pcm);
    active_ = true;
    std::cerr << "XAudio ALSA sink ready device=" << device
              << " rate=48000 channels=2\n";
}

void XAudioBackend::submitGuestFrame(const uint8_t* guestBase, uint32_t guestAddress)
{
    ++submittedFrames_;
    if (!active_)
        return;
    std::array<float, kFrames * 2> stereo{};
    for (unsigned frame = 0; frame < kFrames; ++frame)
    {
        const float left = guestFloat(guestBase, guestAddress + frame * 4);
        const float right = guestFloat(guestBase, guestAddress + (kFrames + frame) * 4);
        const float center = guestFloat(guestBase, guestAddress + (2 * kFrames + frame) * 4);
        const float backLeft = guestFloat(guestBase, guestAddress + (4 * kFrames + frame) * 4);
        const float backRight = guestFloat(guestBase, guestAddress + (5 * kFrames + frame) * 4);
        stereo[2 * frame] = left + 0.70710678f * (center + backLeft);
        stereo[2 * frame + 1] = right + 0.70710678f * (center + backRight);
    }
    const snd_pcm_sframes_t written = snd_pcm_writei(state_->pcm, stereo.data(), kFrames);
    if (written == -EPIPE || written == -ESTRPIPE)
        snd_pcm_prepare(state_->pcm);
    else if (written < 0 && written != -EAGAIN)
        snd_pcm_recover(state_->pcm, static_cast<int>(written), 1);
}
