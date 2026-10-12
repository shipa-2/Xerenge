// The game's rumble on the device's own vibrator (Android).
//
// A handheld's built-in pad often has no motors while the device itself has a
// vibrator, and a phone played on the on-screen controls has no pad at all:
// the title's rumble then went nowhere. It can go to the device instead.
//
// The title sets its pad's motors in one place: sub_825AFEA0, a wrapper that
// tail-calls XamInputSetState for a user and an XINPUT_VIBRATION - the left,
// low-frequency big motor and the right, high-frequency small one, 0 to 65535
// each. It comes there in pairs: from the function that sets the left motor
// (sub_82360950) and then from the one that sets the right (sub_823609C8),
// each with both values.
//
// Where the rumble goes is XERENGE_VIBRATION, which the launcher sets from
// `vibration` in xerenge.conf:
//   controller  the pad, as before
//   device      the device's vibrator; the pad is told to stop
//   auto        the pad if one of the game's pads can rumble, else the device
//   off         neither
// The vibrator itself is worked from Java (DeviceVibration), which is handed
// one strength for the two motors.

#include <jni.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <SDL3/SDL.h>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "guest_memory.h"

extern "C" void __imp__sub_825AFEA0(PPCContext& __restrict, uint8_t*);

namespace {

enum Mode { kAuto = 0, kController = 1, kDevice = 2, kOff = 3 };

using Clock = std::chrono::steady_clock;

// DeviceVibration and its set(int), handed over by nativeStart once the device
// is known to have a vibrator. Without one there is nothing to call.
std::atomic<jclass> g_class{nullptr};
std::atomic<jmethodID> g_set{nullptr};

// What the title's thread keeps between two calls.
std::mutex g_mutex;
int g_sent = 0;
Clock::time_point g_sent_at;
bool g_pad_rumbles = false;
Clock::time_point g_pad_asked_at;
bool g_pad_asked = false;
int g_route = -1;

// XERENGE_VIBRATION: auto (the default, and anything it does not know),
// controller, device or off.
int ConfiguredMode() {
  static const int mode = [] {
    const char* v = std::getenv("XERENGE_VIBRATION");
    if (!v) {
      return kAuto;
    }
    return std::strcmp(v, "controller") == 0 ? kController
           : std::strcmp(v, "device") == 0   ? kDevice
           : std::strcmp(v, "off") == 0      ? kOff
                                             : kAuto;
  }();
  return mode;
}

const char* ModeName(int mode) {
  return mode == kController ? "controller" : mode == kDevice ? "device" : mode == kOff ? "off" : "auto";
}

// Whether any pad the game has open can rumble, for auto. SDL knows: the
// SDK's input driver opens every pad through it, and the on-screen controls'
// virtual pad, which has no motors, is one of them. Pads come and go, so it is
// asked again once a second.
bool PadRumbles(Clock::time_point now) {
  if (g_pad_asked && now - g_pad_asked_at < std::chrono::seconds(1)) {
    return g_pad_rumbles;
  }
  g_pad_asked = true;
  g_pad_asked_at = now;
  g_pad_rumbles = false;
  if (!SDL_WasInit(SDL_INIT_GAMEPAD)) {
    return false;
  }
  SDL_LockJoysticks();
  int count = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&count);
  for (int i = 0; ids && i < count; ++i) {
    SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
    if (pad && SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad),
                                      SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false)) {
      g_pad_rumbles = true;
      break;
    }
  }
  SDL_free(ids);
  SDL_UnlockJoysticks();
  return g_pad_rumbles;
}

// One strength for the device's one motor, 0 to 255 in steps of 16: the big
// motor counts in full and the small one, the lighter of the two in the hand,
// for half. The steps keep an engine's hum, which wanders a little every
// frame, from starting the vibrator afresh sixty times a second.
int Strength(uint32_t left, uint32_t right) {
  const uint32_t both = std::min<uint32_t>(left + right / 2, 65535);
  if (both == 0) {
    return 0;
  }
  const int step = int((both * 16 + 32768) / 65536);
  return std::clamp(step * 16, 16, 255);
}

// Hands the device's vibrator a strength: when it changes, and every half
// second while it stays on - a vibration there ends by itself after a few
// seconds unless it is asked for again, so a game that stops asking cannot
// leave the device buzzing.
void Device(int strength, Clock::time_point now) {
  if (strength == g_sent && (strength == 0 || now - g_sent_at < std::chrono::milliseconds(500))) {
    return;
  }
  const jclass device = g_class.load(std::memory_order_acquire);
  const jmethodID set = g_set.load(std::memory_order_acquire);
  if (!device || !set) {
    return;
  }
  // The title's thread is not one of Java's: SDL attaches it, once, and lets
  // go of it when the thread ends.
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  if (!env) {
    return;
  }
  env->CallStaticVoidMethod(device, set, jint(strength));
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
  }
  g_sent = strength;
  g_sent_at = now;
}

uint16_t LoadBe16(const uint8_t* bytes) {
  return uint16_t((bytes[0] << 8) | bytes[1]);
}

}  // namespace

// The vibrator's Java side is ready: its class and the method to call.
extern "C" JNIEXPORT void JNICALL Java_com_xerenge_burnout_DeviceVibration_nativeStart(JNIEnv* env,
                                                                                    jclass device) {
  if (g_class.load(std::memory_order_acquire)) {
    return;
  }
  const jmethodID set = env->GetStaticMethodID(device, "set", "(I)V");
  if (!set) {
    env->ExceptionClear();
    return;
  }
  g_set.store(set, std::memory_order_release);
  g_class.store(static_cast<jclass>(env->NewGlobalRef(device)), std::memory_order_release);
}

REX_HOOK_RAW(sub_825AFEA0) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t vibration = ctx.r4.u32;
  // The device is the first player's; anybody else's pad is left alone.
  if (user != 0 || vibration == 0) {
    __imp__sub_825AFEA0(ctx, base);
    return;
  }
  std::lock_guard lock(g_mutex);
  const auto now = Clock::now();
  const int mode = ConfiguredMode();
  const bool to_pad = mode == kController || (mode == kAuto && PadRumbles(now));
  const int route = to_pad ? kController : mode == kOff ? kOff : kDevice;
  if (route != g_route) {
    g_route = route;
    REXLOG_INFO("vibration: the game's rumble goes to {} (vibration = {})",
                route == kController ? "the controller"
                : route == kDevice   ? "the device's vibrator"
                                     : "nothing",
                ModeName(mode));
  }
  if (to_pad) {
    __imp__sub_825AFEA0(ctx, base);
    Device(0, now);
    return;
  }
  // XINPUT_VIBRATION: the two motor speeds, big-endian.
  uint8_t* motors = xerenge::GuestPointer(base, vibration);
  const uint16_t left = LoadBe16(motors);
  const uint16_t right = LoadBe16(motors + 2);
  // The pad is told to stop, whatever it was doing when the setting changed:
  // the call goes on with both motors at 0, and the title gets its own values
  // back.
  uint8_t asked[4];
  std::memcpy(asked, motors, sizeof(asked));
  std::memset(motors, 0, sizeof(asked));
  __imp__sub_825AFEA0(ctx, base);
  std::memcpy(motors, asked, sizeof(asked));
  // The title takes a failure to mean the motor was not set and puts its old
  // value back, which with a pad that cannot rumble is 0 for ever: each call
  // would then carry one motor's value and never both. It is told it worked.
  ctx.r3.u64 = 0;
  Device(route == kOff ? 0 : Strength(left, right), now);
}
