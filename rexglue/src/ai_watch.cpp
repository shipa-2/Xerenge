// How a crashed car is put back on the road: hardware watchpoints on the
// player's car, and on one AI car to compare it with.
//
// A wrecked car is revived by sub_8229AE58 -> sub_822989E8 -> sub_82296958,
// which copies its racer's respawn matrix (racer = vehicle +0x1B00; matrix at
// +0x3BA0, position +0x3BD0) into the car. The respawn point is made from the
// racer's node along its route (+0x3B60, copied from the route tracker by
// sub_820EDEC8). For the AI that node runs up the road while the car is
// down, and the car comes back some 100 m from the player wherever it
// crashed; the player comes back where they crashed. Watching the player's
// crash says how the fair one is done.
//
// Four write breakpoints (perf_event_open, PERF_TYPE_BREAKPOINT; SIGTRAP in
// the game's thread on every write), with the host call stack kept for the
// writes that matter - recompiled guest functions call each other as C
// functions, so the frames are the guest's call chain, logged as offsets into
// the executable for `nm -n build/burnout` to name:
//
//   the player's car position (vehicle +0x204 -> +0x30)  a move of over 30 m
//   the player's respawn point (racer +0x3BD0)           a move of over 30 m
//   the player's route node (racer +0x3B60)              any change while wrecked
//   an AI car's route node                                any change while wrecked
//
// "Wrecked" is the vehicle's +0x3941 clear (sub_822989E8 sets it on revival).
// Linux x86-64 only, and only with XERENGE_AI_WATCH set and
// XERENGE_AI_PACING_TRACE=1.
#include <cstdint>

#if defined(__linux__) && defined(__x86_64__)

#include <dlfcn.h>
#include <execinfo.h>
#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#include <signal.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include <rex/logging.h>

#include "guest_memory.h"

namespace {

constexpr int kSlots = 4;  // the CPU's debug registers
constexpr int kFrames = 12;
constexpr int kRecords = 128;
constexpr int kPerWreck = 6;  // node changes kept per crash

constexpr uint32_t kVehicleMatrix = 0x204;
constexpr uint32_t kMatrixPosition = 0x30;
constexpr uint32_t kVehicleRacer = 0x1B00;
constexpr uint32_t kVehicleAlive = 0x3941;
constexpr uint32_t kRacerRespawnPosition = 0x3BD0;
constexpr uint32_t kRacerNode = 0x3B60;

enum class Kind { kPosition, kNode };

struct Slot {
  int fd = -1;
  const char* what = "";
  Kind kind = Kind::kPosition;
  uint32_t vehicle = 0;
  uint32_t watched = 0;  // guest address
  volatile uint8_t* host = nullptr;
  volatile uint8_t* alive = nullptr;
  float last[3] = {};
  uint16_t last_node = 0;
  int kept = 0;
};

struct Record {
  const char* what;
  uint32_t vehicle;
  bool node;
  float from[3];
  float to[3];
  int from_node;
  int to_node;
  bool wrecked;
  void* rip;
  int depth;
  void* frames[kFrames];
};

Slot g_slots[kSlots];
Record g_records[kRecords];
std::atomic<int> g_written{0};
int g_read = 0;
bool g_failed = false;

bool Enabled() {
  static const bool on = std::getenv("XERENGE_AI_WATCH") != nullptr;
  return on;
}

float Distance(const float* a, const float* b) {
  const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
  return std::sqrt(x * x + y * y + z * z);
}

bool Usable(const float* p) {
  return std::isfinite(p[0]) && std::isfinite(p[2]) && std::fabs(p[0]) < 1e5f &&
         std::fabs(p[2]) < 1e5f && (p[0] != 0.0f || p[2] != 0.0f);
}

void ReadPosition(volatile uint8_t* host, float* out) {
  for (int k = 0; k < 3; ++k) {  // big-endian floats
    uint32_t bits;
    std::memcpy(&bits, const_cast<const uint8_t*>(host) + k * 4, sizeof(bits));
    bits = __builtin_bswap32(bits);
    std::memcpy(&out[k], &bits, sizeof(bits));
  }
}

uint16_t ReadNode(volatile uint8_t* host) {
  return uint16_t((host[0] << 8) | host[1]);
}

Record* Take(const Slot& slot, void* context) {
  const int n = g_written.load(std::memory_order_relaxed);
  if (n - g_read >= kRecords) {
    return nullptr;
  }
  Record& r = g_records[n % kRecords];
  r.what = slot.what;
  r.vehicle = slot.vehicle;
  r.wrecked = slot.alive && !*slot.alive;
  r.rip = reinterpret_cast<void*>(static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP]);
  r.depth = backtrace(r.frames, kFrames);
  return &r;
}

void Publish() {
  g_written.store(g_written.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}

// Which breakpoint fired is in the signal's perf data, which glibc's
// siginfo_t does not name; every slot is looked at instead.
void OnTrap(int, siginfo_t* info, void* context) {
  if (info->si_code != TRAP_PERF) {
    return;
  }
  for (Slot& slot : g_slots) {
    if (!slot.host) {
      continue;
    }
    if (slot.kind == Kind::kNode) {
      const uint16_t now = ReadNode(slot.host);
      if (now == slot.last_node) {
        continue;
      }
      if (*slot.alive) {
        slot.kept = 0;
      } else if (slot.kept < kPerWreck) {
        if (Record* r = Take(slot, context)) {
          ++slot.kept;
          r->node = true;
          r->from_node = slot.last_node;
          r->to_node = now;
          Publish();
        }
      }
      slot.last_node = now;
      continue;
    }
    float now[3];
    ReadPosition(slot.host, now);
    if (Usable(slot.last) && Usable(now) && Distance(slot.last, now) > 30.0f) {
      if (Record* r = Take(slot, context)) {
        r->node = false;
        std::memcpy(r->from, slot.last, sizeof(r->from));
        std::memcpy(r->to, now, sizeof(r->to));
        Publish();
      }
    }
    std::memcpy(slot.last, now, sizeof(now));
  }
}

bool Install() {
  static int installed = 0;  // 0 not yet, 1 done, -1 failed
  if (installed == 0) {
    void* prime[2];
    backtrace(prime, 2);  // loads the unwinder now, not inside the handler
    struct sigaction action = {};
    action.sa_sigaction = OnTrap;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    installed = sigaction(SIGTRAP, &action, nullptr) == 0 ? 1 : -1;
  }
  return installed == 1;
}

void Disarm(Slot& slot) {
  if (slot.fd >= 0) {
    close(slot.fd);
  }
  slot = Slot{};
}

void Arm(uint8_t* base, int index, const char* what, Kind kind, uint32_t vehicle,
         uint32_t watched) {
  Slot& slot = g_slots[index];
  if (slot.vehicle == vehicle && slot.watched == watched && slot.fd >= 0) {
    return;
  }
  Disarm(slot);
  slot.what = what;
  slot.kind = kind;
  slot.vehicle = vehicle;
  slot.watched = watched;
  slot.alive = xerenge::GuestPointer(base, vehicle + kVehicleAlive);
  volatile uint8_t* host = xerenge::GuestPointer(base, watched);
  if (kind == Kind::kNode) {
    slot.last_node = ReadNode(host);
  } else {
    ReadPosition(host, slot.last);
  }
  perf_event_attr attr = {};
  attr.type = PERF_TYPE_BREAKPOINT;
  attr.size = sizeof(attr);
  attr.bp_type = HW_BREAKPOINT_W;
  attr.bp_addr = reinterpret_cast<uint64_t>(host);
  attr.bp_len = kind == Kind::kNode ? HW_BREAKPOINT_LEN_2 : HW_BREAKPOINT_LEN_4;
  attr.sample_period = 1;
  attr.sigtrap = 1;
  attr.remove_on_exec = 1;
  attr.exclude_kernel = 1;
  attr.exclude_hv = 1;
  attr.sig_data = uint64_t(index);
  slot.fd = int(syscall(SYS_perf_event_open, &attr, 0, -1, -1, PERF_FLAG_FD_CLOEXEC));
  if (slot.fd < 0) {
    const int error = errno;
    if (!g_failed) {
      g_failed = true;
      REXLOG_INFO("ai watch: no hardware watchpoint ({}) - kernel.perf_event_paranoid may be"
                  " too strict.",
                  std::strerror(error));
    }
    return;
  }
  slot.host = host;
  REXLOG_INFO("ai watch: watching {} - vehicle {:08X}, {:08X}", what, vehicle, watched);
}

bool PlausibleAddress(uint32_t address) {
  return address >= 0x40000000u && address < 0xA0000000u;
}

uintptr_t ExecutableBase() {
  Dl_info info = {};
  dladdr(reinterpret_cast<void*>(&Install), &info);
  return reinterpret_cast<uintptr_t>(info.dli_fbase);
}

}  // namespace

// From the AI update, on the game's thread (the breakpoints belong to the
// thread that makes them): the player's vehicle and an AI car's.
void XerengeAiWatch(uint8_t* base, uint32_t player, uint32_t ai) {
  if (!Enabled() || g_failed || !Install()) {
    return;
  }
  if (PlausibleAddress(player)) {
    const uint32_t matrix = xerenge::LoadGuestU32(base, player + kVehicleMatrix);
    const uint32_t racer = xerenge::LoadGuestU32(base, player + kVehicleRacer);
    if (PlausibleAddress(matrix)) {
      Arm(base, 0, "the player's car", Kind::kPosition, player, matrix + kMatrixPosition);
    }
    if (PlausibleAddress(racer)) {
      Arm(base, 1, "the player's respawn point", Kind::kPosition, player,
          racer + kRacerRespawnPosition);
      Arm(base, 2, "the player's route node", Kind::kNode, player, racer + kRacerNode);
    }
  }
  // One AI car, kept once chosen: the first one seen.
  static uint32_t chosen = 0;
  if (!chosen && PlausibleAddress(ai) && ai != player) {
    chosen = ai;
  }
  if (chosen) {
    const uint32_t racer = xerenge::LoadGuestU32(base, chosen + kVehicleRacer);
    if (PlausibleAddress(racer)) {
      Arm(base, 3, "an AI car's route node", Kind::kNode, chosen, racer + kRacerNode);
    }
  }
  static const uintptr_t executable = ExecutableBase();
  const int written = g_written.load(std::memory_order_acquire);
  while (g_read < written) {
    const Record& r = g_records[g_read % kRecords];
    std::string frames;
    for (int f = 0; f < r.depth; ++f) {
      const uintptr_t address = reinterpret_cast<uintptr_t>(r.frames[f]);
      frames += address >= executable ? fmt::format(" {:x}", address - executable) : " ?";
    }
    const std::string change =
        r.node ? fmt::format("node {} -> {}", r.from_node, r.to_node)
               : fmt::format("({:.0f}, {:.0f}) -> ({:.0f}, {:.0f})", r.from[0], r.from[2],
                             r.to[0], r.to[2]);
    REXLOG_INFO("ai watch: WRITE {} {:08X}{}: {} at exe+{:x}; stack (exe offsets):{}", r.what,
                r.vehicle, r.wrecked ? " (wrecked)" : "", change,
                reinterpret_cast<uintptr_t>(r.rip) - executable, frames);
    ++g_read;
  }
}

#else

void XerengeAiWatch(uint8_t*, uint32_t, uint32_t) {}

#endif
