// Screens wider than 16:9 (21:9 and 18:9 phones): the scene widens to fill
// them, the interface keeps the console's 16:9 box.
//
// The cameras are RenderWare's: CGtViewport::Update hands the field of view to
// sub_82353BA8, which makes the view window - x from the viewport's scale
// (+112) and tan(fov), y = x / the viewport's aspect (+116) - and sets it with
// RwCameraSetViewWindow (sub_8234ABC0). In a race that is viewport 82923220,
// aspect 16/9 and scale 1.2, the field of view moving with speed. Scaling both
// fields by the screen's width over 16:9 for the call widens x by that much
// and leaves y alone: the same vertical view, scale and shapes, more to either
// side. The culling frustum is taken from the view-projection matrix
// (CalculateViewFrustrumFromViewProjMatrix), so it widens with it and nothing
// at the edges goes missing.
//
// The width comes from the renderer (XERENGE_SCREEN_ASPECT, set when its
// scene target is made at the screen's shape); without it nothing changes.
//
// The interface is drawn by the title's 2D layer - CGt2dLayer::RenderObjects
// for the HUD and its 2D objects, CB4FlashManager::Render for the menus and
// CB4CustomRevengeMeter::Render (both hooked in game_timing.cpp) - and the
// Direct3D draws made inside those are marked (DrawingInterface) for the
// renderer to keep in the 16:9 box.
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include "guest_memory.h"
#include "widescreen.h"

extern "C" void __imp__sub_82353BA8(PPCContext& __restrict, uint8_t*);

namespace {

constexpr float kConsoleAspect = 16.0f / 9.0f;

thread_local int g_interface_depth = 0;
thread_local int g_interface_left_depth = 0;
// CGt2dLayer::RenderObjects draws one object per call (it works out the
// object's space and tail-calls the object's own _Render): numbered here so
// the renderer can move each object to its edge as a whole.
thread_local int g_object_depth = 0;
thread_local uint32_t g_object_serial = 0;

float GuestFloat(const uint8_t* base, uint32_t address) {
  const uint32_t bits = xerenge::LoadGuestU32(base, address);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

void StoreGuestFloat(uint8_t* base, uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  xerenge::StoreGuestU32(base, address, bits);
}

// How much wider than 16:9 the screen the scene is drawn for is; 1 when it
// is not wider, or the renderer has not said.
float WideningFactor() {
  const char* text = std::getenv("XERENGE_SCREEN_ASPECT");
  if (!text) {
    return 1.0f;
  }
  const float aspect = std::strtof(text, nullptr);
  return aspect > kConsoleAspect * 1.01f && aspect < 4.0f ? aspect / kConsoleAspect : 1.0f;
}

}  // namespace

namespace xerenge {

bool DrawingInterface() {
  return g_interface_depth > 0 || g_interface_left_depth > 0;
}

bool DrawingInterfaceLeft() {
  return g_interface_left_depth > 0;
}

uint32_t InterfaceObject() {
  return g_object_depth > 0 ? g_object_serial : 0;
}

void EnterInterface() {
  ++g_interface_depth;
}

void LeaveInterface() {
  --g_interface_depth;
}

}  // namespace xerenge

// The view window from the field of view: r3 the viewport, f1 the angle.
REX_HOOK_RAW(sub_82353BA8) {
  const float factor = WideningFactor();
  const uint32_t viewport = ctx.r3.u32;
  const float scale = GuestFloat(base, viewport + 112);
  const float aspect = GuestFloat(base, viewport + 116);
  // The screen's cameras run from 4:3 (the menus') to 16:9 (the race's);
  // anything else - a reflection's, a shadow's - is left as it is.
  const bool widen = factor != 1.0f && aspect > 1.3f && aspect < 1.8f;
  if (widen) {
    static std::atomic<bool> told{false};
    if (!told.exchange(true)) {
      REXLOG_INFO("widescreen: cameras widened by {:.3f} for a {:.3f} screen", factor,
                  factor * kConsoleAspect);
    }
    StoreGuestFloat(base, viewport + 112, scale * factor);
    StoreGuestFloat(base, viewport + 116, aspect * factor);
  }
  __imp__sub_82353BA8(ctx, base);
  if (widen) {
    StoreGuestFloat(base, viewport + 112, scale);
    StoreGuestFloat(base, viewport + 116, aspect);
  }
}

#define INTERFACE_HOOK(name)                                       \
  extern "C" void __imp__##name(PPCContext& __restrict, uint8_t*); \
  REX_HOOK_RAW(name) {                                             \
    ++g_interface_depth;                                           \
    __imp__##name(ctx, base);                                      \
    --g_interface_depth;                                           \
  }

// CGt2dLayer::RenderObjects - the HUD, one 2D object per call.
extern "C" void __imp__sub_82353218(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_82353218) {
  ++g_interface_depth;
  if (g_object_depth++ == 0) {
    g_object_serial = g_object_serial == ~0u ? 1u : g_object_serial + 1;
  }
  __imp__sub_82353218(ctx, base);
  --g_object_depth;
  --g_interface_depth;
}

// CB4EATraxDisplay::Render - the music player's panel, in a 16:9 box against
// the screen's left edge: in the corner of a wide screen, where it sat when it
// was first left out of the box, rather than in the corner of the middle.
extern "C" void __imp__sub_8215C0E8(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_8215C0E8) {
  ++g_interface_left_depth;
  __imp__sub_8215C0E8(ctx, base);
  --g_interface_left_depth;
}
