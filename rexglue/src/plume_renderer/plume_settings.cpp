/**
 * @file        plume_renderer/plume_settings.cpp
 * @brief       The settings that change while the game runs (the in-game menu).
 */
#include "plume_renderer/plume_settings.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace rex::plume_renderer::settings {

namespace {

float EnvFloat(const char* name, float fallback, float low, float high) {
  const char* text = std::getenv(name);
  if (!text || !*text) {
    return fallback;
  }
  return std::clamp(std::strtof(text, nullptr), low, high);
}

struct State {
  std::atomic<float> colour_grade{EnvFloat("XERENGE_COLOUR_GRADE", 1.0f, 0.0f, 1.0f)};
  std::atomic<float> brightness{EnvFloat("XERENGE_BRIGHTNESS", 1.0f, 0.7f, 1.6f)};
  std::atomic<float> shadow_lift{EnvFloat("XERENGE_SHADOW_LIFT", 0.0f, 0.0f, 0.3f)};
  std::atomic<uint32_t> colour_table_version{1};
  std::atomic<uint32_t> max_anisotropy{16};
  std::atomic<uint32_t> sampler_version{1};
  std::atomic<bool> fps_shown{false};
  std::atomic<bool> crash_interpolation{true};

  State() {
    // The title's own request is capped at 4 on a phone by default (its GPU
    // pays for every pixel of it), and at 16 - not at all - elsewhere.
#ifdef __ANDROID__
    constexpr unsigned long kDefaultAnisotropy = 4;
#else
    constexpr unsigned long kDefaultAnisotropy = 16;
#endif
    const char* aniso = std::getenv("XERENGE_MAX_ANISOTROPY");
    const unsigned long level =
        aniso && *aniso ? std::strtoul(aniso, nullptr, 10) : kDefaultAnisotropy;
    max_anisotropy = uint32_t(std::clamp<unsigned long>(level, 1, 16));
    const char* fps = std::getenv("XERENGE_FPS_SHOW");
    fps_shown = fps != nullptr && *fps != '\0' && *fps != '0';
    const char* crash = std::getenv("XERENGE_CRASH_INTERPOLATION");
    crash_interpolation =
        crash == nullptr || (std::strcmp(crash, "0") != 0 && std::strcmp(crash, "false") != 0);
  }
};

State& Get() {
  static State state;
  return state;
}

}  // namespace

float ColourGrade() {
  return Get().colour_grade.load(std::memory_order_relaxed);
}
float Brightness() {
  return Get().brightness.load(std::memory_order_relaxed);
}
float ShadowLift() {
  return Get().shadow_lift.load(std::memory_order_relaxed);
}

void SetColourGrade(float strength) {
  Get().colour_grade = std::clamp(strength, 0.0f, 1.0f);
  ++Get().colour_table_version;
}

void SetBrightness(float gamma) {
  Get().brightness = std::clamp(gamma, 0.7f, 1.6f);
  ++Get().colour_table_version;
}

void SetShadowLift(float lift) {
  Get().shadow_lift = std::clamp(lift, 0.0f, 0.3f);
  ++Get().colour_table_version;
}

uint32_t ColourTableVersion() {
  return Get().colour_table_version.load(std::memory_order_relaxed);
}

uint32_t MaxAnisotropy() {
  return Get().max_anisotropy.load(std::memory_order_relaxed);
}

void SetMaxAnisotropy(uint32_t level) {
  Get().max_anisotropy = std::clamp<uint32_t>(level, 1, 16);
  ++Get().sampler_version;
}

uint32_t SamplerVersion() {
  return Get().sampler_version.load(std::memory_order_relaxed);
}

bool FpsShown() {
  return Get().fps_shown.load(std::memory_order_relaxed);
}
void SetFpsShown(bool shown) {
  Get().fps_shown = shown;
}

bool CrashInterpolation() {
  return Get().crash_interpolation.load(std::memory_order_relaxed);
}
void SetCrashInterpolation(bool on) {
  Get().crash_interpolation = on;
}

}  // namespace rex::plume_renderer::settings
