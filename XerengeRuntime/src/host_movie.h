#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Decodes the title's movies on the host instead of letting the guest's own
// WMC decoder do it.
//
// The .xmv files are ordinary ASF containers - WMV3 video at 1280x720/59.94
// alongside WMA v2 audio - so a host decoder reads them directly. Measured on
// this machine, ffmpeg decodes one of them at about 450 frames per second
// where 60 are needed, which is the headroom the recompiled decoder does not
// have: it runs on the same core budget as everything else the title does.
//
// Only the picture is taken over here. The guest still opens the file, still
// runs its own decoder, and still issues the draw that places the video on
// screen, so the movie appears exactly where and when the title puts it - this
// supplies the pixels for that draw and nothing else.
class HostMovie
{
public:
    ~HostMovie();

    // Starts decoding `path`. Replaces whatever was playing.
    bool open(const std::string& path);
    void close();
    bool active() const;

    // Hands back the frame due at the current presentation time, decoding
    // forward (and dropping late frames) to get there. Returns false when the
    // clip is not playing, has ended, or has no new frame since the last call,
    // in which case the caller keeps showing what it already has.
    bool nextFrame(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height,
        uint64_t& generation);

    // Whether the clip has run to its end. The title's own status machine
    // waits on the decoder's answer to that question.
    bool finished() const;

private:
    struct State;

    void closeLocked();

    mutable std::mutex mutex_;
    State* state_ = nullptr;
};

extern HostMovie gHostMovie;

// Maps a clip name from the title's video descriptor - "BG1_P", "EAHD_E_P" -
// onto the movie file on the host, or an empty string when there is none.
std::string hostMoviePathForClip(const std::string& clipName);
