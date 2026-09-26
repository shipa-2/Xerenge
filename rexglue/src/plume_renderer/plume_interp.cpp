// Drawing between logic steps. WORK IN PROGRESS - enabled by XERENGE_INTERPOLATION=1.
//
// With the frame rate unlocked the title steps its logic sixty times a second
// and draws as often as it can (game_timing.cpp in the project). A frame drawn
// without a new step would show the same state again - no smoother than sixty.
// Every object the title draws takes its placement from its vertex shader's
// constants (the object-to-projection matrix and its relatives), so a draw
// seen in the last two steps can be drawn between them: each placement matrix
// goes from where it was to where it is by how far the wall clock has got from
// the one state to the other. The camera is in those matrices, so it is
// interpolated with everything else. Only the matrices, found by name in the
// shader's generated source: a colour or a count takes the new value whole.
//
// The drawn state trails the wall clock by up to a step (the steps owed are
// run at the start of the next frame), so what is drawn is the moment one step
// behind the clock, which always lies between the last two states - the same
// one-step delay re:Blue's interpolation has.
//
// A draw is recognised from step to step by its shaders, its buffers and its
// index range; several draws of one mesh (traffic of one model, props) are
// paired with the nearest placement of the step before. Anything drawn from a
// buffer that moves each frame (the interface, the particles) finds no partner
// and is drawn as it is. A camera cut - most placements jumping at once - is
// drawn as it is for the whole frame: deciding draw by draw left neighbours in
// different states, and they jittered against each other.
#include "plume_renderer/plume_interp.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>

#include <xxhash.h>

#include <rex/logging.h>

#include "plume_renderer/plume_draw.h"

extern "C" void (*rex_frame_clock_provider)(void*, size_t);

namespace rex::plume_renderer {
namespace {

// The layout game_timing.cpp publishes.
struct FrameClock {
  uint64_t frame = 0;
  uint32_t steps_this_frame = 0;
  uint32_t epoch = 0;
  double state_steps = 0.0;
  double real_steps = 0.0;
  uint32_t valid = 0;
  uint32_t vblanks_per_frame = 1;
};

constexpr size_t kRegisters = 256;
using Constants = std::array<uint32_t, kRegisters * 4>;

// One draw of the current state, and where it was at the state before.
struct Instance {
  Constants prev{};
  Constants curr{};
  bool has_prev = false;
  bool snap = false;  // too large a change to draw between
  std::vector<uint16_t> changed;
};

// Every draw of one mesh with one pair of shaders, in the order drawn.
struct Group {
  std::vector<Instance> instances;
  uint64_t step = 0;  // the state they belong to
};

struct Interpolator {
  std::unordered_map<uint64_t, Group> groups;
  uint32_t epoch = ~0u;
  double last_state = -1.0;
  double prev_state = -1.0;
  uint64_t steps_recorded = 0;
  bool cut = false;  // this state is drawn as it is
};

Interpolator& State() {
  static Interpolator state;
  return state;
}

// Work in progress, off unless asked for: pairing and cut detection still
// leave objects jittering and dropping out, and the default is the title's own
// sixty frames a second, where there is nothing to draw between anyway.
bool Enabled() {
  static const bool on = std::getenv("XERENGE_INTERPOLATION") != nullptr;
  return on;
}

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

uint64_t BaseKey(const GuestDrawSnapshot& d) {
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

// The registers holding a shader's placement matrices, from its generated
// source: each named constant runs from its start to the next one's.
const std::vector<uint16_t>& MatrixRegisters(uint64_t vs_hash) {
  static std::unordered_map<uint64_t, std::vector<uint16_t>> cache;
  if (auto it = cache.find(vs_hash); it != cache.end()) {
    return it->second;
  }
  std::vector<uint16_t>& out = cache[vs_hash];
  char name[64];
  std::snprintf(name, sizeof(name), "generated/xenos-hlsl/%016llx.hlsl",
                static_cast<unsigned long long>(vs_hash));
  std::ifstream file(name, std::ios::binary);
  if (!file) {
    return out;
  }
  const std::string hlsl((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  static constexpr const char* kMatrices[] = {
      "gObjToProjMatrix",       "gCurrentWorldViewMatrix", "gPrevWorldViewMatrix",
      "gObjToWorldMatrix",      "gWorldToProjMatrix",      "gObjToWorldRotationMatrix",
      "gVertexShader_BlendMatrices", "gTransformArray"};
  // Every named vertex constant and where it starts.
  std::vector<std::pair<uint32_t, bool>> starts;  // register, is a matrix
  static constexpr std::string_view kDefine = "#define g";
  static constexpr std::string_view kBase = "g_PushConstants.VertexShaderConstants + ";
  for (size_t at = hlsl.find(kDefine); at != std::string::npos; at = hlsl.find(kDefine, at + 1)) {
    const size_t eol = hlsl.find('\n', at);
    const std::string line = hlsl.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
    const size_t base = line.find(kBase);
    if (base == std::string::npos) {
      continue;
    }
    // "+ (START + min(INDEX, ...)) * 16" for arrays, "+ OFFSET," for single ones.
    size_t num = base + kBase.size();
    uint32_t reg = 0;
    if (line[num] == '(') {
      reg = uint32_t(std::strtoul(line.c_str() + num + 1, nullptr, 10));
    } else {
      reg = uint32_t(std::strtoul(line.c_str() + num, nullptr, 10)) / 16;
    }
    const std::string id = line.substr(8, line.find_first_of("( ", 8) - 8);
    bool matrix = false;
    for (const char* m : kMatrices) {
      matrix |= id == m;
    }
    starts.emplace_back(reg, matrix);
  }
  std::sort(starts.begin(), starts.end());
  for (size_t i = 0; i < starts.size(); ++i) {
    if (!starts[i].second) {
      continue;
    }
    const uint32_t end = i + 1 < starts.size() ? starts[i + 1].first : kRegisters;
    for (uint32_t r = starts[i].first; r < end && r < kRegisters; ++r) {
      out.push_back(uint16_t(r));
    }
  }
  return out;
}

// How far apart two draws' placements are.
double Distance(const Constants& a, const Constants& b, const std::vector<uint16_t>& regs) {
  double d = 0.0;
  for (const uint16_t r : regs) {
    for (int c = 0; c < 4; ++c) {
      const float x = AsFloat(a[r * 4 + c]);
      const float y = AsFloat(b[r * 4 + c]);
      d += (std::isfinite(x) && std::isfinite(y)) ? std::fabs(double(x) - double(y)) : 1e9;
    }
  }
  return d;
}

// Pairs a new state's instance with where it was.
void Pair(Instance& inst, const Constants* before, const std::vector<uint16_t>& regs) {
  inst.has_prev = before != nullptr;
  inst.changed.clear();
  inst.snap = false;
  if (!before) {
    return;
  }
  inst.prev = *before;
  double moved = 0.0, size = 0.0;
  for (const uint16_t r : regs) {
    if (std::memcmp(&inst.prev[r * 4], &inst.curr[r * 4], 16) == 0) {
      continue;
    }
    inst.changed.push_back(r);
    for (int c = 0; c < 4; ++c) {
      const float a = AsFloat(inst.prev[r * 4 + c]);
      const float b = AsFloat(inst.curr[r * 4 + c]);
      if (!std::isfinite(a) || !std::isfinite(b)) {
        inst.snap = true;
        continue;
      }
      moved += std::fabs(double(b) - double(a));
      size += 0.5 * (std::fabs(double(a)) + std::fabs(double(b)));
    }
  }
  // A jump rather than a movement.
  if (size > 0.0 && moved > 0.35 * size) {
    inst.snap = true;
  }
}

}  // namespace

void InterpolateFrame(std::vector<GuestDrawSnapshot>& draws) {
  if (!Enabled() || !rex_frame_clock_provider) {
    return;
  }
  FrameClock clock;
  rex_frame_clock_provider(&clock, sizeof(clock));
  if (!clock.valid) {
    return;
  }
  Interpolator& state = State();
  if (clock.epoch != state.epoch) {
    state.groups.clear();
    state.epoch = clock.epoch;
    state.last_state = -1.0;
    state.prev_state = -1.0;
  }
  // A new state: the draws in this frame are it.
  const bool new_state = clock.state_steps != state.last_state;
  if (new_state) {
    state.prev_state = state.last_state;
    state.last_state = clock.state_steps;
    ++state.steps_recorded;
  }
  const double span = state.last_state - state.prev_state;
  const bool drawable = state.prev_state >= 0.0 && span > 0.0 && span <= 4.0;
  // What is drawn: one step behind the clock. The drawn state trails the
  // clock by up to a step, so that moment lies between the last two states.
  const float t = drawable ? float(std::clamp((clock.real_steps - 1.0 - state.prev_state) / span,
                                              0.0, 1.0))
                           : 1.0f;

  // This frame's draws, grouped by mesh, in order.
  std::unordered_map<uint64_t, std::vector<GuestDrawSnapshot*>> frame_groups;
  frame_groups.reserve(draws.size());
  for (GuestDrawSnapshot& d : draws) {
    if (d.is_clear || d.is_resolve || d.d3d_vertex_buffer == 0 || !d.video_rgba.empty()) {
      continue;
    }
    frame_groups[BaseKey(d)].push_back(&d);
  }

  uint32_t interpolated = 0, jumped = 0, paired = 0, unmatched = 0;
  if (new_state) {
    for (auto& [key, members] : frame_groups) {
      Group& group = state.groups[key];
      const std::vector<uint16_t>& regs = MatrixRegisters(members.front()->vs_hash);
      // Where each of these was at the state before. A mesh drawn several
      // times is not drawn in the same order from step to step - traffic of
      // one model is sorted by distance - so each is paired with the nearest
      // placement of the ones before.
      const bool had_before = group.step + 1 == state.steps_recorded;
      std::vector<Instance> before;
      if (had_before) {
        before = std::move(group.instances);
      }
      group.instances.assign(members.size(), Instance{});
      std::vector<bool> taken(before.size(), false);
      for (size_t i = 0; i < members.size(); ++i) {
        Instance& inst = group.instances[i];
        std::memcpy(inst.curr.data(), members[i]->vs_constants.data(), sizeof(inst.curr));
        const Constants* from = nullptr;
        if (before.size() == 1 && members.size() == 1) {
          from = &before[0].curr;
        } else if (!before.empty() && members.size() <= 256 && before.size() <= 256) {
          double best = 0.0;
          size_t best_j = before.size();
          for (size_t j = 0; j < before.size(); ++j) {
            if (taken[j]) {
              continue;
            }
            const double d = Distance(before[j].curr, inst.curr, regs);
            if (best_j == before.size() || d < best) {
              best = d;
              best_j = j;
            }
          }
          if (best_j < before.size()) {
            taken[best_j] = true;
            from = &before[best_j].curr;
          }
        }
        Pair(inst, from, regs);
        if (inst.has_prev && !inst.changed.empty()) {
          ++paired;
          jumped += inst.snap ? 1 : 0;
        }
      }
      group.step = state.steps_recorded;
    }
    // A cut: most of what moved jumped. The whole frame is drawn as it is.
    state.cut = paired > 0 && jumped * 3 > paired;
  }

  for (auto& [key, members] : frame_groups) {
    auto found = state.groups.find(key);
    if (found == state.groups.end() || found->second.step != state.steps_recorded) {
      unmatched += uint32_t(members.size());
      continue;
    }
    if (state.cut || t >= 1.0f) {
      continue;
    }
    const Group& group = found->second;
    for (size_t i = 0; i < members.size() && i < group.instances.size(); ++i) {
      const Instance& inst = group.instances[i];
      // A draw that jumped on its own (a part torn off, a wheel spinning past
      // what a straight line can follow) is drawn as it is; everything around
      // it still moves smoothly.
      if (!inst.has_prev || inst.changed.empty() || inst.snap) {
        continue;
      }
      GuestDrawSnapshot& d = *members[i];
      for (const uint16_t r : inst.changed) {
        for (int c = 0; c < 4; ++c) {
          const float a = AsFloat(inst.prev[r * 4 + c]);
          const float b = AsFloat(inst.curr[r * 4 + c]);
          d.vs_constants[r * 4 + c] = AsBits(a + (b - a) * t);
        }
      }
      ++interpolated;
    }
  }

  // Forget meshes that have not been drawn for a while.
  if (new_state && (state.steps_recorded % 120) == 0) {
    for (auto it = state.groups.begin(); it != state.groups.end();) {
      if (it->second.step + 4 < state.steps_recorded) {
        it = state.groups.erase(it);
      } else {
        ++it;
      }
    }
    REXLOG_INFO("plume: interpolation - {} meshes tracked, last frame {} between steps (t {:.2f}{}), "
                "{} unmatched (state {:.2f}, clock {:.2f})",
                state.groups.size(), interpolated, t, state.cut ? ", cut" : "", unmatched,
                clock.state_steps, clock.real_steps);
  }
}

}  // namespace rex::plume_renderer
