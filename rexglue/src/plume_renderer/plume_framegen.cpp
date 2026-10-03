// Frames drawn between two of the title's - frame generation.
//
// The title runs as it does on the console, sixty frames a second (thirty in
// Crash mode), and the renderer keeps the frame before the one it has just
// been given. Between the two it draws frames of its own: the newer frame's
// draws, each object placed between where it was and where it is. What is
// shown is a frame behind what the title has made - the newer frame goes up
// after the frames leading to it - which is the price of drawing between two
// frames already in hand rather than guessing where things go next.
//
// Every object the title draws takes its placement from its vertex shader's
// constants: the object-to-projection matrix and its relatives, found by name
// in the shader's generated source (shader_source_info.h). The camera is in
// those matrices, so it moves between frames with everything else. A colour,
// a count or a texture takes the newer frame's value whole.
//
// A draw is recognised from frame to frame by its shaders, its buffers and
// its index range; several draws of one mesh (traffic of one model, props)
// are paired with the nearest placement of the frame before. Anything drawn
// from a buffer filled anew each frame (the interface, the particles) finds no
// partner and is drawn as in the newer frame. A draw whose placement jumped
// (a part torn off) is drawn as it is; when most of what moved jumped - a
// camera cut, a restart - the whole frame is.
#include "plume_renderer/plume_framegen.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <utility>

#include <xxhash.h>

#include "plume_renderer/shader_source_info.h"

namespace rex::plume_renderer {
namespace {

float AsFloat(uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

uint32_t AsBits(float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  return bits;
}

bool Followable(const GuestDrawSnapshot& d) {
  return d.valid && !d.is_clear && !d.is_resolve && d.d3d_vertex_buffer != 0 &&
         !d.has_video_frame();
}

uint64_t DrawKey(const GuestDrawSnapshot& d) {
  struct {
    uint64_t vs, ps;
    uint32_t vb, stride, ib, start, count, base_vertex, prim;
    uint32_t streams[4];
  } key{};
  key.vs = d.vs_hash;
  key.ps = d.ps_hash;
  key.vb = d.d3d_vertex_buffer;
  key.stride = d.d3d_vertex_stride;
  key.ib = d.d3d_index_buffer;
  key.start = d.d3d_start_index;
  key.count = d.num_indices;
  key.base_vertex = d.d3d_base_vertex;
  key.prim = d.prim_type;
  for (int i = 0; i < 4; ++i) {
    key.streams[i] = d.d3d_stream_address[i];
  }
  return XXH3_64bits(&key, sizeof(key));
}

const std::vector<uint16_t>& MatrixRegisters(uint64_t vs_hash) {
  static const std::vector<uint16_t> kNone;
  const ShaderSourceInfo* info = FindShaderSourceInfo(vs_hash);
  return info ? info->matrix_registers : kNone;
}

// How far apart two draws' placements are.
double Distance(const GuestDrawSnapshot& a, const GuestDrawSnapshot& b,
                const std::vector<uint16_t>& regs) {
  double d = 0.0;
  for (const uint16_t r : regs) {
    for (int c = 0; c < 4; ++c) {
      const float x = AsFloat(a.vs_constants[r * 4 + c]);
      const float y = AsFloat(b.vs_constants[r * 4 + c]);
      d += (std::isfinite(x) && std::isfinite(y)) ? std::fabs(double(x) - double(y)) : 1e9;
    }
  }
  return d;
}

}  // namespace

bool FrameGenerationEnabled() {
  static const bool on = [] {
    const char* v = std::getenv("XERENGE_FRAME_GENERATION");
    return v != nullptr && *v != '\0' && *v != '0';
  }();
  return on;
}

uint32_t FrameGenerationTarget() {
  static const uint32_t target = [] {
    const char* v = std::getenv("XERENGE_FRAME_GENERATION");
    const unsigned long hz = v ? std::strtoul(v, nullptr, 10) : 0;
    return hz > 1 && hz <= 480 ? uint32_t(hz) : 0u;
  }();
  return target;
}

FramePairing PairFrames(const std::vector<GuestDrawSnapshot>& previous,
                        const std::vector<GuestDrawSnapshot>& current) {
  FramePairing pairing;
  // The older frame's draws by key, in the order drawn.
  std::unordered_map<uint64_t, std::vector<uint32_t>> before;
  before.reserve(previous.size());
  for (uint32_t i = 0; i < previous.size(); ++i) {
    if (Followable(previous[i])) {
      before[DrawKey(previous[i])].push_back(i);
    }
  }
  std::unordered_map<uint64_t, std::vector<uint32_t>> now;
  now.reserve(current.size());
  for (uint32_t i = 0; i < current.size(); ++i) {
    if (Followable(current[i])) {
      now[DrawKey(current[i])].push_back(i);
    }
  }
  pairing.matches.reserve(current.size());
  for (const auto& [key, members] : now) {
    const auto found = before.find(key);
    if (found == before.end()) {
      continue;
    }
    const std::vector<uint32_t>& candidates = found->second;
    const std::vector<uint16_t>& regs = MatrixRegisters(current[members.front()].vs_hash);
    if (regs.empty()) {
      continue;
    }
    // One of each: the same object. Several (traffic of one model, sorted
    // by distance and so not drawn in the same order from frame to frame):
    // each with the nearest placement not yet taken.
    std::vector<bool> taken(candidates.size(), false);
    const bool crowd = members.size() > 1 || candidates.size() > 1;
    if (crowd && (members.size() > 256 || candidates.size() > 256)) {
      continue;
    }
    for (const uint32_t m : members) {
      size_t best_j = candidates.size();
      if (!crowd) {
        best_j = 0;
      } else {
        double best = 0.0;
        for (size_t j = 0; j < candidates.size(); ++j) {
          if (taken[j]) {
            continue;
          }
          const double d = Distance(previous[candidates[j]], current[m], regs);
          if (best_j == candidates.size() || d < best) {
            best = d;
            best_j = j;
          }
        }
      }
      if (best_j == candidates.size()) {
        continue;
      }
      taken[best_j] = true;
      const GuestDrawSnapshot& a = previous[candidates[best_j]];
      const GuestDrawSnapshot& b = current[m];
      FramePairing::Match match;
      match.current = m;
      match.previous = candidates[best_j];
      double moved = 0.0, size = 0.0;
      bool bad = false;
      for (const uint16_t r : regs) {
        if (std::memcmp(&a.vs_constants[r * 4], &b.vs_constants[r * 4], 16) == 0) {
          continue;
        }
        match.registers.push_back(r);
        for (int c = 0; c < 4; ++c) {
          const float x = AsFloat(a.vs_constants[r * 4 + c]);
          const float y = AsFloat(b.vs_constants[r * 4 + c]);
          if (!std::isfinite(x) || !std::isfinite(y)) {
            bad = true;
            continue;
          }
          moved += std::fabs(double(y) - double(x));
          size += 0.5 * (std::fabs(double(x)) + std::fabs(double(y)));
        }
      }
      if (match.registers.empty()) {
        continue;  // did not move: nothing to place between
      }
      ++pairing.followed;
      // A jump rather than a movement: drawn as it is now. A frame of a car
      // at speed moves its nearest objects a fair part of their own size in
      // the camera's matrices, so only well past that is a jump.
      if (bad || (size > 0.0 && moved > 0.8 * size)) {
        ++pairing.jumped;
        continue;
      }
      pairing.matches.push_back(std::move(match));
    }
  }
  pairing.cut = pairing.followed >= 8 && pairing.jumped * 2 > pairing.followed;
  return pairing;
}

void BuildInBetween(const std::vector<GuestDrawSnapshot>& previous,
                    const std::vector<GuestDrawSnapshot>& current, const FramePairing& pairing,
                    float t, std::vector<GuestDrawSnapshot>& out) {
  out.assign(current.begin(), current.end());
  if (pairing.cut) {
    return;
  }
  for (const FramePairing::Match& match : pairing.matches) {
    const GuestDrawSnapshot& a = previous[match.previous];
    GuestDrawSnapshot& d = out[match.current];
    for (const uint16_t r : match.registers) {
      for (int c = 0; c < 4; ++c) {
        const float x = AsFloat(a.vs_constants[r * 4 + c]);
        const float y = AsFloat(std::as_const(d.vs_constants)[r * 4 + c]);
        d.vs_constants[r * 4 + c] = AsBits(x + (y - x) * t);
      }
    }
  }
}

}  // namespace rex::plume_renderer
