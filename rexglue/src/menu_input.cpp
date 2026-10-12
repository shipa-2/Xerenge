// The in-game menu's share of the title's pad reads.
//
// The title reads the pad in one place: sub_825AFE90, a wrapper that tail-calls
// XamInputGetState for a user and a state to fill in. Everything the host
// offers as a pad ends up there - the pads themselves, and the keyboard
// standing in for one - so this is where the menu both listens and keeps the
// title from listening: while it is open the title is handed a pad nobody
// touches.
//
// The menu is in the plume plugin (plume_renderer/plume_menu.cpp) and the
// plugin is a library of its own, so it is found by its exported function
// rather than linked. Under the xenos renderer there is none, and nothing here
// does anything.
#include <cstdint>
#include <cstring>
#include <filesystem>

#ifdef __ANDROID__
#include <dlfcn.h>
#endif

#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/platform/dynlib.h>
#include <rex/ppc.h>

#include "guest_memory.h"
#include "menu_pad.h"

extern "C" void __imp__sub_825AFE90(PPCContext& __restrict, uint8_t*);

void* xerenge::FindMenuSymbol(const char* name) {
#if defined(__ANDROID__)
  // The plugin is a library of the app, found by its name among those loaded.
  static void* const library = dlopen("librexgpu-plume.so", RTLD_LAZY | RTLD_NOLOAD);
  return library ? dlsym(library, name) : nullptr;
#else
#if defined(_WIN32)
  const char* file = "rexgpu-plume.dll";
#elif defined(__APPLE__)
  const char* file = "librexgpu-plume.dylib";
#else
  const char* file = "librexgpu-plume.so";
#endif
  // Already loaded by the SDK when the renderer is plume: this only takes
  // another reference to it. Kept for the life of the process, like the
  // SDK's own.
  static rex::platform::DynamicLibrary library;
  static const bool loaded = [&] {
    const auto path = rex::filesystem::GetExecutableFolder() / file;
    return std::filesystem::exists(path) && library.Load(path);
  }();
  return loaded ? library.GetRawSymbol(name) : nullptr;
#endif
}

namespace {

// The plugin's entry point, looked up once the first pad read has happened -
// by which time the renderer is up. Null for any renderer but plume.
xerenge::MenuPadFn MenuPad() {
  static const xerenge::MenuPadFn entry =
      reinterpret_cast<xerenge::MenuPadFn>(xerenge::FindMenuSymbol(xerenge::kMenuPadSymbol));
  return entry;
}

uint16_t LoadBe16(const uint8_t* bytes) {
  return uint16_t((bytes[0] << 8) | bytes[1]);
}

}  // namespace

REX_HOOK_RAW(sub_825AFE90) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t state = ctx.r4.u32;
  __imp__sub_825AFE90(ctx, base);
  const xerenge::MenuPadFn menu_pad = MenuPad();
  // A pad that is not there (the result is not ERROR_SUCCESS) is left alone.
  if (!menu_pad || state == 0 || ctx.r3.u32 != 0) {
    return;
  }
  // XINPUT_STATE: a packet number, then the buttons, the triggers and the four
  // stick axes, big-endian.
  uint8_t* gamepad = xerenge::GuestPointer(base, state) + 4;
  xerenge::MenuPadState pad;
  pad.buttons = LoadBe16(gamepad);
  pad.left_trigger = gamepad[2];
  pad.right_trigger = gamepad[3];
  pad.thumb_lx = int16_t(LoadBe16(gamepad + 4));
  pad.thumb_ly = int16_t(LoadBe16(gamepad + 6));
  pad.thumb_rx = int16_t(LoadBe16(gamepad + 8));
  pad.thumb_ry = int16_t(LoadBe16(gamepad + 10));
  const uint16_t buttons = pad.buttons;
  if (menu_pad(user, &pad)) {
    std::memset(gamepad, 0, 12);
  } else if (pad.buttons != buttons) {
    gamepad[0] = uint8_t(pad.buttons >> 8);
    gamepad[1] = uint8_t(pad.buttons);
  }
}
