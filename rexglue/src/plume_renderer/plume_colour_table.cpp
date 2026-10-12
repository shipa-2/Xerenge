/**
 * @file        plume_renderer/plume_colour_table.cpp
 * @brief       The title's colour grading table, adjusted by the colour settings.
 */
#include "plume_renderer/plume_colour_table.h"

#include <algorithm>
#include <cmath>

namespace rex::plume_renderer {

namespace {

// What every output colour of the table is run through after the grade is
// blended in: the gamma, then the lift. Dark cars are close to black on the
// console too; this is the way to lift them.
struct ColourCurve {
  uint8_t lut[256];
  bool identity = true;
};

ColourCurve BuildColourCurve(float brightness, float lift) {
  ColourCurve curve;
  curve.identity = brightness == 1.0f && lift == 0.0f;
  for (int i = 0; i < 256; ++i) {
    float v = std::pow(float(i) / 255.0f, 1.0f / brightness);
    // The lift is the most at black and nothing at white; cubed, so it is the
    // shadows that rise and not the midtones.
    const float rest = 1.0f - v;
    v += lift * rest * rest * rest;
    curve.lut[i] = uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
  }
  return curve;
}

}  // namespace

void AdjustColourTable(uint8_t* texels, uint32_t n, uint32_t row_bytes, bool bgra, float grade,
                       float brightness, float lift) {
  const ColourCurve curve = BuildColourCurve(brightness, lift);
  if ((grade >= 1.0f && curve.identity) || n == 0) {
    return;
  }
  const uint32_t r = bgra ? 2 : 0;
  const uint32_t b = bgra ? 0 : 2;
  const uint32_t channel[3] = {r, 1, b};
  for (uint32_t layer = 0; layer < n; ++layer) {
    for (uint32_t y = 0; y < n; ++y) {
      uint8_t* row = texels + (size_t(layer) * n + y) * row_bytes;
      for (uint32_t x = 0; x < n; ++x) {
        uint8_t* t = row + size_t(x) * 4;
        const float identity[3] = {float((x + 0.5) / n * 255.0), float((y + 0.5) / n * 255.0),
                                   float(double(layer) / n * 255.0)};
        for (uint32_t c = 0; c < 3; ++c) {
          if (grade < 1.0f) {
            const float graded = t[channel[c]];
            t[channel[c]] = uint8_t(std::lround(identity[c] + (graded - identity[c]) * grade));
          }
          if (!curve.identity) {
            t[channel[c]] = curve.lut[t[channel[c]]];
          }
        }
      }
    }
  }
}

}  // namespace rex::plume_renderer
