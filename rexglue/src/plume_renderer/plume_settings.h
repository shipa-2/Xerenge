/**
 * @file        plume_renderer/plume_settings.h
 * @brief       The settings that change while the game runs (the in-game menu).
 */
#pragma once

#include <cstdint>

namespace rex::plume_renderer::settings {

// Each of these started as an environment variable read once into a static;
// they are now the runtime state, initialised from the same variables so a run
// without the menu behaves as before. The menu writes them from the UI thread,
// the renderer reads them every frame.
//
// A setting that must be noticed by something with its own cache (the colour
// tables, the sampler cache) has a version that goes up when it changes.

// XERENGE_COLOUR_GRADE: how much of the title's colour grading is kept, 0 to 1.
float ColourGrade();
void SetColourGrade(float strength);
// XERENGE_BRIGHTNESS: a gamma on the 3D picture, 0.7 to 1.6; 1 leaves it as drawn.
float Brightness();
void SetBrightness(float gamma);
// XERENGE_SHADOW_LIFT: how far the darkest colours are raised, 0 to 0.3.
float ShadowLift();
void SetShadowLift(float lift);
// Goes up whenever one of the three above changes: the colour tables are
// blended again.
uint32_t ColourTableVersion();

// XERENGE_MAX_ANISOTROPY: the most anisotropy a texture is filtered with, a cap
// on what the title asks; 1 (none) to 16.
uint32_t MaxAnisotropy();
void SetMaxAnisotropy(uint32_t level);
// Goes up whenever the above changes: the samplers are made again.
uint32_t SamplerVersion();

// XERENGE_FPS_SHOW: presents per second in the top left corner.
bool FpsShown();
void SetFpsShown(bool shown);

// XERENGE_PERF_OVERLAY: the frame-time graph in the corner, drawn even with the menu closed.
bool PerfOverlay();
void SetPerfOverlay(bool shown);

// XERENGE_CRASH_INTERPOLATION: a frame between each two of Crash mode's thirty.
bool CrashInterpolation();
void SetCrashInterpolation(bool on);

}  // namespace rex::plume_renderer::settings
