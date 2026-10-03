/**
 * @file        plume_renderer/plume_framegen.h
 * @brief       Frames drawn between two of the title's (XERENGE_FRAME_GENERATION).
 */
#pragma once

#include <cstdint>
#include <vector>

#include "plume_renderer/plume_draw.h"

namespace rex::plume_renderer {

// XERENGE_FRAME_GENERATION: on a screen faster than the title draws (a 120 Hz
// phone; the title at sixty, at thirty in Crash mode), frames between two of
// its frames - the title itself untouched, its clock, input and loading all as
// on the console. "1": as many as the screen's refresh rate has room for; a
// number above 1: that refresh rate is assumed.
bool FrameGenerationEnabled();
// The refresh rate asked for (0: the screen's own).
uint32_t FrameGenerationTarget();

// How two frames' draws correspond: each draw of the newer frame whose
// placement can be followed, with the draw it was in the older one.
struct FramePairing {
  struct Match {
    uint32_t current = 0;   // index in the newer frame
    uint32_t previous = 0;  // index in the older one
    std::vector<uint16_t> registers;  // its placement registers that moved
  };
  std::vector<Match> matches;
  // Most of what moved jumped - a camera cut, a restart: nothing to draw
  // between, the newer frame is drawn as it is.
  bool cut = false;
  uint32_t followed = 0;
  uint32_t jumped = 0;
};

FramePairing PairFrames(const std::vector<GuestDrawSnapshot>& previous,
                        const std::vector<GuestDrawSnapshot>& current);

// The frame t of the way from the older frame to the newer one (0 < t < 1):
// the newer frame's draws, each followed one placed between where it was and
// where it is. Into `out`, which keeps its storage from frame to frame.
void BuildInBetween(const std::vector<GuestDrawSnapshot>& previous,
                    const std::vector<GuestDrawSnapshot>& current, const FramePairing& pairing,
                    float t, std::vector<GuestDrawSnapshot>& out);

}  // namespace rex::plume_renderer
