#pragma once

#include <cstdint>
#include <memory>

class XAudioBackend
{
public:
    XAudioBackend();
    ~XAudioBackend();
    XAudioBackend(const XAudioBackend&) = delete;
    XAudioBackend& operator=(const XAudioBackend&) = delete;

    void start();
    void submitGuestFrame(const uint8_t* guestBase, uint32_t guestAddress);

private:
    struct State;
    std::unique_ptr<State> state_;
    bool active_ = false;
    uint64_t submittedFrames_ = 0;
};
