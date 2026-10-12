// The on-screen controls' gamepad (Android): TouchControls, the translucent
// buttons BurnoutActivity draws over the game, press the buttons and move the
// axes of an SDL virtual gamepad here. The SDK's SDL input driver opens it
// like any pad that is plugged in (SDL_EVENT_GAMEPAD_ADDED). While the game's
// settings menu is open the view hands it its touches instead.

#include <jni.h>

#include <atomic>
#include <mutex>

#include <SDL3/SDL.h>

#include "menu_pad.h"

namespace {

std::mutex g_mutex;
SDL_Joystick* g_pad = nullptr;

// Made once SDL's joystick subsystem is up (the input driver brings it up
// when the game starts); until then there is nothing to attach to.
SDL_Joystick* Pad() {
  if (g_pad) {
    return g_pad;
  }
  if (!SDL_WasInit(SDL_INIT_JOYSTICK)) {
    return nullptr;
  }
  SDL_VirtualJoystickDesc desc;
  SDL_INIT_INTERFACE(&desc);
  desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
  desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
  desc.nbuttons = SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1;
  desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
  desc.button_mask = (1u << (SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1)) - 1;
  desc.name = "Xerenge touch controls";
  const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
  if (!id) {
    return nullptr;
  }
  g_pad = SDL_OpenJoystick(id);
  return g_pad;
}

// A function of the settings menu (plume_renderer/plume_menu.cpp) by its
// exported name, or null while the renderer, and the menu with it, is not
// loaded yet. The view asks from its first moment, so only a function found is
// remembered, in `found`.
template <typename Fn>
Fn MenuFunction(std::atomic<void*>& found, const char* name) {
  void* entry = found.load(std::memory_order_acquire);
  if (!entry) {
    entry = xerenge::FindMenuSymbol(name);
    found.store(entry, std::memory_order_release);
  }
  return reinterpret_cast<Fn>(entry);
}

}  // namespace

extern "C" JNIEXPORT jboolean JNICALL Java_com_xerenge_burnout_TouchControls_nativeAttach(JNIEnv*, jclass) {
  std::lock_guard lock(g_mutex);
  return Pad() != nullptr;
}

// button: an SDL_GamepadButton.
extern "C" JNIEXPORT void JNICALL Java_com_xerenge_burnout_TouchControls_nativeButton(JNIEnv*, jclass,
                                                                                    jint button,
                                                                                    jboolean down) {
  std::lock_guard lock(g_mutex);
  if (SDL_Joystick* pad = Pad()) {
    SDL_SetJoystickVirtualButton(pad, button, down);
  }
}

// axis: an SDL_GamepadAxis; value -1..1 for the sticks, 0..1 for the triggers.
extern "C" JNIEXPORT void JNICALL Java_com_xerenge_burnout_TouchControls_nativeAxis(JNIEnv*, jclass,
                                                                                  jint axis,
                                                                                  jfloat value) {
  std::lock_guard lock(g_mutex);
  if (SDL_Joystick* pad = Pad()) {
    const bool trigger = axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
    float raw = trigger ? SDL_JOYSTICK_AXIS_MIN + value * (SDL_JOYSTICK_AXIS_MAX - SDL_JOYSTICK_AXIS_MIN)
                        : value * SDL_JOYSTICK_AXIS_MAX;
    raw = SDL_clamp(raw, float(SDL_JOYSTICK_AXIS_MIN), float(SDL_JOYSTICK_AXIS_MAX));
    SDL_SetJoystickVirtualAxis(pad, axis, Sint16(raw));
  }
}

// The settings menu has the screen while it is open: the view then hands it its
// touches instead of working the pad, and draws nothing over it.
extern "C" JNIEXPORT jboolean JNICALL Java_com_xerenge_burnout_TouchControls_nativeMenuVisible(JNIEnv*,
                                                                                              jclass) {
  static std::atomic<void*> found{nullptr};
  const auto visible = MenuFunction<xerenge::MenuVisibleFn>(found, xerenge::kMenuVisibleSymbol);
  return visible && visible();
}

extern "C" JNIEXPORT void JNICALL Java_com_xerenge_burnout_TouchControls_nativeMenuToggle(JNIEnv*, jclass) {
  static std::atomic<void*> found{nullptr};
  const auto toggle = MenuFunction<xerenge::MenuToggleFn>(found, xerenge::kMenuToggleSymbol);
  if (toggle) {
    toggle();
  }
}

// event: 0 down, 1 move, 2 up; x and y, 0 to 1 across the view from its top left.
extern "C" JNIEXPORT void JNICALL Java_com_xerenge_burnout_TouchControls_nativeMenuPointer(JNIEnv*, jclass,
                                                                                         jint event,
                                                                                         jfloat x,
                                                                                         jfloat y) {
  static std::atomic<void*> found{nullptr};
  const auto pointer = MenuFunction<xerenge::MenuPointerFn>(found, xerenge::kMenuPointerSymbol);
  if (pointer) {
    pointer(event, x, y);
  }
}
