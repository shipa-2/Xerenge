#pragma once

#include <vector>

namespace rex::plume_renderer {

struct GuestDrawSnapshot;

// Frames drawn between two of the title's logic steps, drawn between the two
// states: see plume_interp.cpp.
void InterpolateFrame(std::vector<GuestDrawSnapshot>& draws);

}  // namespace rex::plume_renderer
