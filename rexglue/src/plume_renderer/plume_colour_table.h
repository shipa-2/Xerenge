/**
 * @file        plume_renderer/plume_colour_table.h
 * @brief       The title's colour grading table, adjusted by the colour settings.
 */
#pragma once

#include <cstdint>

namespace rex::plume_renderer {

// The 3D frame's last pass looks every pixel up in a table of n x n x n
// colours, the title's grading - its yellow-brown cast. This changes the
// table, as the title loaded it, by the colour settings:
//
//  * `grade`, 0 to 1: how much of the table is kept; the rest is the identity
//    table (as plume's identity_lut_ holds it: red across, green down, blue by
//    layer). 1 is the console's.
//  * `brightness`, 0.7 to 1.6: a gamma on every colour the table gives. 1 is
//    as it is.
//  * `lift`, 0 to 0.3: the dark end raised, fading out towards white, so the
//    brightest colours cannot clip.
//
// At 1, 1 and 0 nothing is done and the table stays byte for byte what the
// title loaded. `texels` is n layers of n rows of `row_bytes`, four bytes a
// texel, in the order of `bgra`.
void AdjustColourTable(uint8_t* texels, uint32_t n, uint32_t row_bytes, bool bgra, float grade,
                       float brightness, float lift);

}  // namespace rex::plume_renderer
