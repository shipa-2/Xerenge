// The race AI's rubber banding, and a switch to take the catch-up out of it.
//
// What the game does to keep the AI close to the player, as found in this
// build (Burnout Revenge, retail):
//
// 1. The speed limit. sub_820D7AD8 runs each step for each AI car (r3 = the
//    car). It compares how far the car has come in the race with how far the
//    racer it is after - the player - has (each racer's +0x34B0,
//    sub_820D0018), and sets the car's +0xA6E: 1 while the car is behind the
//    player at all, 0 once it is more than 10 m ahead. While 1, the car's
//    vehicle gets the player's top speed plus 30 mph as its own (vehicle
//    +0x1954) and a softer wall penalty (+0x1928); while 0, its own car's
//    "Max Speed In Boost" and "Wall Collsion Penalty" - what that car has with
//    a person driving it. The car's handling is the vehicle's +0x1984: "Speed
//    Limit" +0x198, "Wall Collsion Penalty" +0x194, "Max Speed In Boost" +0x1C0
//    (registered by sub_82269238), in mph.
//
// 2. Coming back from a wreck. A wrecked car (vehicle +0x3941 clear) is
//    revived by sub_8229AE58 -> sub_822989E8 -> sub_82296958 at a respawn
//    point made from its racer's node along its route (+0x3B60, copied from
//    the route tracker at +0x3B14 by sub_820EDEC8; respawn point by
//    sub_820EF148 -> sub_820D3CF8). For the AI, its per-step update
//    sub_820F6EA8 (r3 = the racer) keeps advancing that route while the car
//    lies wrecked, so it came back about 100 m from the player wherever it
//    crashed. The player's route stands still while they are wrecked, and they
//    come back where they crashed.
//
// 3. The timetable. sub_820F6320 (r3 = the car) first sets the car's speed cap
//    (+0xA44, sub_820A8528) to segment length / (scheduled time + car +0xA40),
//    then moves +0xA40 by how the race clock (racer +0x34BC) stands against
//    two running totals of scheduled time, a fast (+0xA38) and a slow (+0xA3C):
//    past the fast one it goes below 0 (a higher cap), short of the slow one
//    above 0 (a lower cap). Mostly the second: the AI held back.
//
// 4. Left alone: an AI car that gets about 130 m ahead of the player, out of
//    sight, is taken off its physics (vehicle +0x3941 clear, as for a wreck)
//    and carried along some 125-140 m ahead of the player whatever the player
//    does - it cannot get away.
//
// "No AI rubber banding" (no_rubber_band in xerenge.conf, the HACKS page;
// XERENGE_NO_RUBBER_BAND=1) takes out the catch-up and leaves the AI's waiting
// for the player as it is: (1) each AI car keeps its own car's top speed and
// wall penalty, (2) a wrecked AI car's route stands still as the player's
// does, and (3) the timetable never raises the cap. (4) is untouched.
//
// XERENGE_AI_PACING_TRACE=1 logs, once a second of race time, each AI car's
// pacing and limits and where every car is, with any jump further than a car
// can drive in a step (tools/race_report.py turns it into a summary);
// XERENGE_AI_WATCH as well puts hardware watchpoints on the player's car to
// see what moves it (ai_watch.cpp).
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "guest_memory.h"

void XerengeAiWatch(uint8_t* base, uint32_t player, uint32_t ai);  // ai_watch.cpp

namespace {

// The AI car (sub_820F6320's and sub_820D7AD8's r3).
constexpr uint32_t kCarTarget = 0x7D4;       // the racer it is after: the player
constexpr uint32_t kCarRacer = 0x7E0;        // its own racer
constexpr uint32_t kCarSegment = 0xA4C;      // int8, the segment last scheduled
constexpr uint32_t kCarCorrection = 0xA40;   // float, seconds
constexpr uint32_t kCarSpeedCap = 0xA44;     // float, metres a second
constexpr uint32_t kCarMatching = 0xA6D;     // uint8, matching the player's speed
constexpr uint32_t kCarBehind = 0xA6E;       // uint8, behind the player: raised limit

// The racer (sub_820F6EA8's r3; the car's +0x7E0; the vehicle's +0x1B00).
constexpr uint32_t kRacerClock = 0x34BC;     // float, seconds of race
constexpr uint32_t kRacerColumn = 0x34C4;    // int32, the timetable's column (lap)
constexpr uint32_t kRacerNode = 0x3B60;      // uint16, node along its route
constexpr uint32_t kRacerVehicle = 0x4740;

// The vehicle.
constexpr uint32_t kVehicleSpeed = 0xBC;     // float, metres a second
constexpr uint32_t kVehicleVelocity = 0xB0;
constexpr uint32_t kVehicleMatrix = 0x204;   // its rigid body's matrix
constexpr uint32_t kVehiclePosition = 0x240; // copied from that matrix each step
constexpr uint32_t kVehicleWallPenalty = 0x1928;
constexpr uint32_t kVehicleTopSpeed = 0x1954;   // mph, the limit in force
constexpr uint32_t kVehicleHandling = 0x1984;
constexpr uint32_t kVehicleAlive = 0x3941;      // clear while wrecked or carried along

// The car's handling (its own, as with a person driving it).
constexpr uint32_t kHandlingSpeedLimit = 0x198;  // "Speed Limit"
constexpr uint32_t kHandlingBoostLimit = 0x1C0;  // "Max Speed In Boost", mph
constexpr uint32_t kHandlingWallPenalty = 0x194; // "Wall Collsion Penalty"

// The rolling start leaves every vehicle's +0x3941 clear too, with the AI
// still to drive: the wreck rule starts after it.
constexpr float kRollingStart = 5.0f;

constexpr float kMph = 2.23694f;  // per metre a second

std::atomic<bool> g_no_rubber_band{[] {
  const char* value = std::getenv("XERENGE_NO_RUBBER_BAND");
  return value != nullptr && value[0] != '\0' && value[0] != '0';
}()};

bool NoRubberBand() {
  return g_no_rubber_band.load(std::memory_order_relaxed);
}

bool Tracing() {
  static const bool on = std::getenv("XERENGE_AI_PACING_TRACE") != nullptr;
  return on;
}

bool Plausible(uint32_t address) {
  return address >= 0x40000000u && address < 0xA0000000u;
}

float LoadF32(const uint8_t* base, uint32_t address) {
  const uint32_t bits = xerenge::LoadGuestU32(base, address);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

void StoreF32(uint8_t* base, uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  xerenge::StoreGuestU32(base, address, bits);
}

uint8_t LoadU8(const uint8_t* base, uint32_t address) {
  return *xerenge::GuestPointer(base, address);
}

uint16_t LoadU16(const uint8_t* base, uint32_t address) {
  const uint8_t* p = xerenge::GuestPointer(base, address);
  return uint16_t((p[0] << 8) | p[1]);
}

uint32_t VehicleOf(const uint8_t* base, uint32_t racer) {
  return Plausible(racer) ? xerenge::LoadGuestU32(base, racer + kRacerVehicle) : 0;
}

// The player's vehicle, as the AI's target - kept by sub_820F6320's hook.
uint32_t g_player_vehicle = 0;

// Once a second of race time per key: whether to log now.
bool Due(std::unordered_map<uint32_t, float>& logged_at, uint32_t key, float clock) {
  float& at = logged_at[key];
  if (clock >= at && clock - at < 1.0f) {
    return false;
  }
  at = clock;
  return true;
}

// --- the trace -------------------------------------------------------------

// The vehicles seen, the player's among them (the rolling start drives it
// through the AI for a few steps; tools/race_report.py tells it by that).
constexpr uint32_t kMaxVehicles = 12;
uint32_t g_vehicles[kMaxVehicles] = {};
uint32_t g_vehicle_count = 0;

void NoteVehicle(uint32_t vehicle) {
  for (uint32_t i = 0; i < g_vehicle_count; ++i) {
    if (g_vehicles[i] == vehicle) {
      return;
    }
  }
  if (g_vehicle_count < kMaxVehicles) {
    g_vehicles[g_vehicle_count++] = vehicle;
  }
}

struct Position {
  float x = 0.0f, y = 0.0f, z = 0.0f;
  float clock = -1.0f;
};

Position PositionOf(const uint8_t* base, uint32_t vehicle) {
  Position p;
  p.x = LoadF32(base, vehicle + kVehiclePosition);
  p.y = LoadF32(base, vehicle + kVehiclePosition + 4);
  p.z = LoadF32(base, vehicle + kVehiclePosition + 8);
  return p;
}

bool Usable(const Position& p) {
  return std::isfinite(p.x) && std::isfinite(p.z) && std::fabs(p.x) < 1e5f &&
         std::fabs(p.z) < 1e5f && (p.x != 0.0f || p.z != 0.0f);
}

// Each AI car's pacing once a second, and any move between two of its steps
// further than its speed allows (twice that, with room for a bump or a slam).
void TraceCar(const uint8_t* base, uint32_t car, uint32_t racer, uint32_t vehicle) {
  static std::unordered_map<uint32_t, Position> last;
  static std::unordered_map<uint32_t, float> logged_at;
  static uint32_t jumps = 0;
  const float clock = LoadF32(base, racer + kRacerClock);
  const float speed = LoadF32(base, vehicle + kVehicleSpeed);
  Position now = PositionOf(base, vehicle);
  now.clock = clock;
  Position& before = last[vehicle];
  const float dt = clock - before.clock;
  if (before.clock >= 0.0f && dt > 0.0f && Usable(before) && Usable(now) && jumps < 300) {
    const float moved = std::hypot(std::hypot(now.x - before.x, now.y - before.y), now.z - before.z);
    const float allowed = std::fmax(speed, 10.0f) * std::fmax(dt, 1.0f / 60.0f) * 2.0f + 8.0f;
    if (moved > allowed) {
      ++jumps;
      REXLOG_INFO("ai pacing: JUMP between updates racer 0 vehicle {:08X} moved {:.1f} m in {:.3f} s"
                  " at {:.0f} mph ({:.1f}, {:.1f}, {:.1f}) -> ({:.1f}, {:.1f}, {:.1f}) clock {:.2f}",
                  vehicle, moved, dt, speed * kMph, before.x, before.y, before.z, now.x, now.y,
                  now.z, clock);
    }
  }
  before = now;
  if (!Due(logged_at, vehicle, clock)) {
    return;
  }
  REXLOG_INFO("ai pacing: racer 0 vehicle {:08X} clock {:.1f} lap {} seg {} speed {:.0f} mph cap {:.0f} mph"
              " corr {:+.1f} matching {}",
              vehicle, clock, int32_t(xerenge::LoadGuestU32(base, racer + kRacerColumn)),
              int8_t(LoadU8(base, car + kCarSegment)), speed * kMph,
              LoadF32(base, car + kCarSpeedCap) * kMph, LoadF32(base, car + kCarCorrection),
              LoadU8(base, car + kCarMatching));
}

// Every vehicle seen, once a second of race time: speed, where it is and which
// way it is going.
void TraceField(const uint8_t* base, float clock) {
  static float logged_at = -1.0f;
  if (clock >= logged_at && clock - logged_at < 1.0f) {
    return;
  }
  logged_at = clock;
  std::string line;
  for (uint32_t i = 0; i < g_vehicle_count; ++i) {
    const uint32_t vehicle = g_vehicles[i];
    const Position p = PositionOf(base, vehicle);
    if (!Usable(p)) {
      continue;
    }
    line += fmt::format(" | {:08X} AI {:.0f} mph at ({:.0f}, {:.0f}) heading ({:.1f}, {:.1f})",
                        vehicle, LoadF32(base, vehicle + kVehicleSpeed) * kMph, p.x, p.z,
                        LoadF32(base, vehicle + kVehicleVelocity),
                        LoadF32(base, vehicle + kVehicleVelocity + 8));
  }
  REXLOG_INFO("ai pacing: field at {:.1f}{}", clock, line);
}

// --- the timetable (3) -------------------------------------------------------

// A negative correction - a raised cap - is taken back to 0.
void ClampCatchUp(uint8_t* base, uint32_t car) {
  if (LoadF32(base, car + kCarCorrection) < 0.0f) {
    StoreF32(base, car + kCarCorrection, 0.0f);
  }
}

}  // namespace

// The HACKS page's row, at once (xerenge_settings.cpp).
void XerengeSetNoRubberBand(bool on) {
  g_no_rubber_band.store(on, std::memory_order_relaxed);
  REXLOG_INFO("ai pacing: no AI rubber banding {}", on ? "on" : "off");
}

bool XerengeNoRubberBand() {
  return NoRubberBand();
}

extern "C" void __imp__sub_820F6320(PPCContext& __restrict, uint8_t*);

REX_HOOK_RAW(sub_820F6320) {
  const uint32_t car = ctx.r3.u32;
  if (!Plausible(car)) {
    __imp__sub_820F6320(ctx, base);
    return;
  }
  const uint32_t target = VehicleOf(base, xerenge::LoadGuestU32(base, car + kCarTarget));
  if (Plausible(target)) {
    g_player_vehicle = target;
  }
  // Before the call as well as after: the cap is worked out from the
  // correction first thing, before the correction is updated.
  if (NoRubberBand()) {
    ClampCatchUp(base, car);
  }
  __imp__sub_820F6320(ctx, base);
  if (NoRubberBand()) {
    ClampCatchUp(base, car);
  }
  if (Tracing()) {
    const uint32_t racer = xerenge::LoadGuestU32(base, car + kCarRacer);
    const uint32_t vehicle = VehicleOf(base, racer);
    if (Plausible(vehicle)) {
      NoteVehicle(vehicle);
      TraceCar(base, car, racer, vehicle);
      TraceField(base, LoadF32(base, racer + kRacerClock));
      XerengeAiWatch(base, target, vehicle);
    }
  }
}

// The speed limit (1): each AI car's own car's, after the game has set it.
extern "C" void __imp__sub_820D7AD8(PPCContext& __restrict, uint8_t*);

REX_HOOK_RAW(sub_820D7AD8) {
  const uint32_t car = ctx.r3.u32;
  __imp__sub_820D7AD8(ctx, base);
  if (!Plausible(car)) {
    return;
  }
  const uint32_t racer = xerenge::LoadGuestU32(base, car + kCarRacer);
  const uint32_t vehicle = VehicleOf(base, racer);
  if (!Plausible(vehicle)) {
    return;
  }
  const uint32_t handling = xerenge::LoadGuestU32(base, vehicle + kVehicleHandling);
  if (!Plausible(handling)) {
    return;
  }
  const float own = LoadF32(base, handling + kHandlingBoostLimit);
  if (Tracing()) {
    static std::unordered_map<uint32_t, float> logged_at;
    if (Due(logged_at, vehicle, LoadF32(base, racer + kRacerClock))) {
      REXLOG_INFO("ai pacing: limits vehicle {:08X}: its car's speed limit {:.0f}, boosting {:.0f}"
                  " mph; in force {:.0f} mph{}",
                  vehicle, LoadF32(base, handling + kHandlingSpeedLimit), own,
                  LoadF32(base, vehicle + kVehicleTopSpeed),
                  LoadU8(base, car + kCarBehind) ? " (raised: behind the player)" : "");
    }
  }
  if (NoRubberBand() && std::isfinite(own) && own > 0.0f) {
    StoreF32(base, vehicle + kVehicleTopSpeed, own);
    StoreF32(base, vehicle + kVehicleWallPenalty, LoadF32(base, handling + kHandlingWallPenalty));
  }
}

// Coming back from a wreck (2): an AI car's update is not run while its car is
// down, so its route stands still where it crashed, as the player's does.
extern "C" void __imp__sub_820F6EA8(PPCContext& __restrict, uint8_t*);

REX_HOOK_RAW(sub_820F6EA8) {
  const uint32_t racer = ctx.r3.u32;
  if (NoRubberBand() && Plausible(racer)) {
    const uint32_t vehicle = VehicleOf(base, racer);
    const float clock = LoadF32(base, racer + kRacerClock);
    if (Plausible(vehicle) && vehicle != g_player_vehicle &&
        LoadU8(base, vehicle + kVehicleAlive) == 0 && clock > kRollingStart) {
      if (Tracing()) {
        static std::unordered_map<uint32_t, float> logged_at;
        if (Due(logged_at, vehicle, clock)) {
          REXLOG_INFO("ai pacing: vehicle {:08X} wrecked at {:.1f} s - its route held at node {}",
                      vehicle, clock, LoadU16(base, racer + kRacerNode));
        }
      }
      return;
    }
  }
  __imp__sub_820F6EA8(ctx, base);
}
