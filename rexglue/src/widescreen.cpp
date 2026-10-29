// Screens wider than 16:9 (21:9 and 18:9 phones): the scene widens to fill
// them, the interface keeps the console's 16:9 box. Narrower ones (a 16:10
// monitor): the scene grows taller instead, the interface keeps its 16:9 strip.
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
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

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
thread_local int g_interface_fill_depth = 0;
// Where the element being drawn goes (InterfaceEdge), set by its hook.
thread_local uint8_t g_element_edge = 0;
// CGt2dLayer::RenderObjects draws one object per call (it works out the
// object's space and tail-calls the object's own _Render): numbered here so
// the renderer can move each object to its edge as a whole.
thread_local int g_object_depth = 0;
thread_local uint32_t g_object_serial = 0;
// The edge of the announcement being drawn (PanelEdge), from its panel, for the
// elements after it in the same layer; 0 before any panel.
thread_local uint8_t g_message_edge = 0;

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

// The screen the scene is drawn for against 16:9: above 1 wider (21:9, 18:9),
// below it narrower (16:10, 4:3); 1 when it is 16:9, or the renderer has not said.
float WideningFactor() {
  const char* text = std::getenv("XERENGE_SCREEN_ASPECT");
  if (!text) {
    return 1.0f;
  }
  // Any shape a window can be dragged to: a fixed range (it was 1.2 to 4)
  // put the 16:9 scene back, stretched, on a window past either end. Only a
  // value no window has is refused.
  const float aspect = std::strtof(text, nullptr);
  const bool wider = aspect > kConsoleAspect * 1.01f && aspect < 64.0f;
  const bool narrower = aspect < kConsoleAspect * 0.99f && aspect > 1.0f / 64.0f;
  return wider || narrower ? aspect / kConsoleAspect : 1.0f;
}

}  // namespace

namespace xerenge {

bool DrawingInterface() {
  return g_interface_depth > 0 || g_interface_left_depth > 0 || g_interface_fill_depth > 0;
}

bool DrawingInterfaceLeft() {
  return g_interface_left_depth > 0;
}

bool DrawingInterfaceFill() {
  return g_interface_fill_depth > 0;
}

uint8_t InterfaceEdge() {
  return g_element_edge;
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

void EnterInterfaceFill() {
  ++g_interface_fill_depth;
}

void LeaveInterfaceFill() {
  --g_interface_fill_depth;
}

// The menus are Flash movies played by Apt; when one was last drawn
// (NoteMenuDrawn, from CB4AptManager::Render's hook in xerenge_menu.cpp).
std::atomic<int64_t> g_menu_drawn_ms{0};

int64_t SteadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void NoteMenuDrawn() {
  g_menu_drawn_ms.store(SteadyMs(), std::memory_order_relaxed);
}

// A menu's background video - SAVE/LOAD's, the main menu's, World Tour's -
// is drawn into the title's own target, copied out, and shown by the menu as
// a picture across its screen. Kept in the 16:9 box like any video, it filled
// only the middle of that picture, and the menu, drawn across the whole frame
// on top, ran into the black beside it. Over the whole frame (fill) it lines
// up with the menu as a scene does (car select). Only while a menu is shown:
// a video on its own (the intro) keeps its 16:9.
bool EnterMenuVideo() {
  static const bool off = [] {
    const char* mode = std::getenv("XERENGE_MENU_VIDEO");
    return mode != nullptr && std::strcmp(mode, "box") == 0;
  }();
  // The video plays from CB4FrontEnd::Update, before the menu is drawn, so
  // the menu is the previous frame's: within a quarter of a second.
  if (off || WideningFactor() == 1.0f ||
      SteadyMs() - g_menu_drawn_ms.load(std::memory_order_relaxed) > 250) {
    return false;
  }
  static std::atomic<bool> told{false};
  if (std::getenv("XERENGE_HUD_LOG") && !told.exchange(true)) {
    REXLOG_INFO("widescreen: a menu's background video, over the whole frame");
  }
  ++g_interface_fill_depth;
  return true;
}

void LeaveMenuVideo() {
  --g_interface_fill_depth;
}

void EnterInterfaceObject() {
  static std::atomic<bool> told{false};
  if (std::getenv("XERENGE_HUD_LOG") && !told.exchange(true)) {
    REXLOG_INFO("widescreen: first HUD object drawn");
  }
  ++g_interface_depth;
  if (g_object_depth++ == 0) {
    g_object_serial = g_object_serial == ~0u ? 1u : g_object_serial + 1;
  }
}

void LeaveInterfaceObject() {
  --g_object_depth;
  --g_interface_depth;
}

// The recompiled code keeps the console's stack frames: a function's prologue
// stores its return address at -8 from the stack pointer it was entered with
// and links its frame to its caller's (stwu r1), so [frame] is the caller's
// frame and [caller's frame - 8] what the function returns to. 0x82353340 is
// where CGt2dLayer::RenderObjects's loop returns to from an element's render.
void TraceInterfaceDraw(const char* what, uint32_t lr, uint32_t sp, const uint8_t* base) {
  static const bool on = std::getenv("XERENGE_HUD_TRACE") != nullptr;
  if (!on || !DrawingInterface()) {
    return;
  }
  // The next 150 interface draws, every 5 seconds.
  static std::atomic<uint64_t> window_ms{0};
  static std::atomic<uint32_t> left{0};
  const uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 std::chrono::steady_clock::now().time_since_epoch())
                                                 .count());
  uint64_t was = window_ms.load(std::memory_order_relaxed);
  if (now - was >= 5000 && window_ms.compare_exchange_strong(was, now)) {
    left.store(150, std::memory_order_relaxed);
    REXLOG_INFO("hud trace: ---- window ----");
  }
  if (left.load(std::memory_order_relaxed) == 0 ||
      left.fetch_sub(1, std::memory_order_relaxed) == 0) {
    return;
  }
  const auto is_code = [](uint32_t address) {
    return address >= 0x82000000u && address < 0x83000000u;
  };
  std::string chain = fmt::format("{:08X}", lr);
  bool in_element = false;
  // RenderObjects's own frame, once the walk reaches it: the space it works
  // out for the element is in its locals at +96 and copied to +128.
  std::string space;
  uint32_t frame = sp;
  for (int depth = 0; depth < 16; ++depth) {
    const uint32_t caller = LoadGuestU32(base, frame);
    // A frame chain runs up the stack, a frame at a time.
    if (caller <= frame || caller - frame > 0x10000u) {
      break;
    }
    const uint32_t ret = LoadGuestU32(base, caller - 8);
    if (!is_code(ret)) {
      break;
    }
    chain += fmt::format(" < {:08X}", ret);
    if (ret == 0x82353340u && !in_element) {
      in_element = true;
      // The element's render saves RenderObjects's r31 - the list node of the
      // element being drawn - at -16 from the stack pointer it was entered
      // with, which is RenderObjects's own.
      space = fmt::format(" node {:08X}", LoadGuestU32(base, caller - 12));
    }
    frame = caller;
  }
  REXLOG_INFO("hud trace: {} object {} {}:{} : {}", what, InterfaceObject(),
              in_element ? "in element" : "outside", space, chain);
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
      REXLOG_INFO("widescreen: cameras {} by {:.3f} for a {:.3f} screen",
                  factor > 1.0f ? "widened" : "made taller", factor, factor * kConsoleAspect);
    }
    // Wider: x grows with the scale, y (x over the aspect) stays. Narrower
    // (16:10): x stays and y grows - the same width of view, more above and
    // below, rather than the picture squeezed upright.
    if (factor > 1.0f) {
      StoreGuestFloat(base, viewport + 112, scale * factor);
    }
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

// The HUD is made of components (the position, the player list, the boost
// bar...), each owning one CGt2dLayer that the title places itself: in
// CB4HUDComponent::Prepare the layer (+8) is made at the point
// CB4HUD::GetAlignPos (sub_82188288) gives for the component's alignment
// (+24) - an index into the table at 825F50C8 of where in the HUD's frame it
// goes: 0 top-centre, 1 top-left, 2 middle-left, 3 bottom-left, 4
// bottom-centre, 5 bottom-right, 6 middle-right, 7 top-right, 8 none. The
// title's own answer to which edge each group belongs to, so the whole layer
// goes there.
namespace {

std::mutex g_layer_edges_mutex;
std::unordered_map<uint32_t, uint8_t> g_layer_edges;

uint8_t AlignEdge(uint32_t align) {
  constexpr uint8_t kEdges[] = {2, 1, 1, 1, 2, 3, 3, 3};
  return align < std::size(kEdges) ? kEdges[align] : 0;
}

uint8_t LayerEdge(uint32_t layer) {
  std::lock_guard lock(g_layer_edges_mutex);
  const auto it = g_layer_edges.find(layer);
  return it == g_layer_edges.end() ? 0 : it->second;
}

}  // namespace

// Each event's HUD is made afresh, and its layers can land where another
// event's were: a Crash event's bottom-left component had the layer that the
// next race's announcements were drawn in, and kept them on the left. So the
// edges start over with each HUD: when a component comes more than two
// seconds after the last - each HUD's components are prepared within the
// same second, events minutes apart. (Not from CB4HUD::Prepare: it does not
// call the components' Prepare itself, so it may come after them and forget
// the HUD it belongs to.)
namespace {

std::chrono::steady_clock::time_point g_last_component{};

void ForgetLayerEdges() {
  size_t forgotten;
  {
    std::lock_guard lock(g_layer_edges_mutex);
    forgotten = g_layer_edges.size();
    g_layer_edges.clear();
  }
  if (forgotten != 0 && std::getenv("XERENGE_HUD_LOG")) {
    REXLOG_INFO("widescreen: a new HUD: {} layer edges forgotten", forgotten);
  }
}

}  // namespace

extern "C" void __imp__sub_821931F8(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_821931F8) {
  const uint32_t component = ctx.r3.u32;
  __imp__sub_821931F8(ctx, base);
  const uint32_t layer = xerenge::LoadGuestU32(base, component + 8);
  if (layer == 0) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (now - g_last_component > std::chrono::seconds(2)) {
    ForgetLayerEdges();
  }
  g_last_component = now;
  const uint32_t align = xerenge::LoadGuestU32(base, component + 24);
  const uint8_t edge = AlignEdge(align);
  {
    std::lock_guard lock(g_layer_edges_mutex);
    g_layer_edges[layer] = edge;
  }
  if (std::getenv("XERENGE_HUD_LOG")) {
    REXLOG_INFO("widescreen: component {:08X} layer {:08X} align {} -> {}", component, layer,
                align, edge == 1 ? "left" : edge == 3 ? "right" : edge == 2 ? "middle" : "unset");
  }
}

// CGt2dLayer::RenderObjects - a layer of the HUD: it walks the layer's list
// of elements and calls each one's render through the pointer the element
// holds, returning to 0x82353340. The layer is numbered here; each element
// gets its own number below (ELEMENT_HOOK).
extern "C" void __imp__sub_82353218(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_82353218) {
  const uint8_t outer_message_edge = g_message_edge;
  g_message_edge = 0;
  xerenge::EnterInterfaceObject();
  __imp__sub_82353218(ctx, base);
  xerenge::LeaveInterfaceObject();
  g_message_edge = outer_message_edge;
}

// Gt2dRenderer batches the vertices the elements give it in one buffer
// (Gt2dRenderer::CVertexBuffer, the global at 0x82D18190) and hands them to
// Direct3D (CVertexBuffer::Dispatch, sub_82355FD0) only when the texture,
// blend mode or colour next changes - which is usually in the next element's
// render. Each draw then came out under the element after the one it belongs
// to: an element's shadow pass went to one edge and its text to another. So
// the batch is flushed around each element, while that element's number is
// the current one. Dispatch returns at once when the batch is empty.
extern "C" void sub_82355FD0(PPCContext& __restrict, uint8_t*);

namespace {

constexpr uint32_t kVertexBatch = 0x82D18190;
constexpr uint32_t kRenderObjectsLoop = 0x82353340;

void FlushVertexBatch(const PPCContext& ctx, uint8_t* base) {
  PPCContext flush = ctx;
  flush.r3.u64 = kVertexBatch;
  sub_82355FD0(flush, base);
}

// Where an element goes, from the rectangle RenderObjects hands its render
// (r3: x, y, width, height in the console's 1280x720, worked out from the
// element's own position and size and its layer's). Against an edge of the
// console's screen - within 20% of it - it goes to that edge of a wider one;
// otherwise, or spanning both, it stays in the middle with the rest of the
// layout. Decided once for the element, not from whichever of its draws
// comes first: the boost bar's first draw is sometimes a flame in the middle.
constexpr float kConsoleWidth = 1280.0f;

uint8_t EdgeOf(const uint8_t* base, uint32_t rect) {
  float lo = GuestFloat(base, rect);
  float hi = lo + GuestFloat(base, rect + 8);
  if (hi < lo) {
    std::swap(lo, hi);
  }
  // 20%, not 15%: the rating's star ends at x 1080, left of the word it goes
  // with, and at 15% it stayed in the middle while the word went right.
  const bool at_left = lo < kConsoleWidth * 0.2f;
  const bool at_right = hi > kConsoleWidth * 0.8f;
  if (at_left != at_right) {
    return at_left ? 1 : 3;
  }
  if (!at_left) {
    return 2;
  }
  // Across both: a full-width piece, or one zooming in - TAKEDOWN! and its
  // icon start several times their size and shrink into the corner, and taken
  // as spanning the screen they began in the middle and jumped to the edge a
  // moment later. A zoom keeps the middle of the element where it is.
  const float middle = (lo + hi) * 0.5f;
  return middle < kConsoleWidth * 0.4f ? 1 : middle > kConsoleWidth * 0.6f ? 3 : 2;
}

// The rating's star keeps the edge it was first given while it stays on
// screen: it sits left of the word, and AWESOME, longer than GREAT, pushed it
// out of the right edge's 20% and back to the middle, away from the word. Only
// the star: the title reuses the nodes of short-lived elements all the time
// (a message's icon, then its text), and every element keeping its first edge
// sent a timer's digits and a takedown's icon to the edge of what had been
// there before. The element is its list node (RenderObjects's r31); one not
// drawn for a second is decided afresh.
uint8_t StickyEdge(uint32_t node, uint8_t edge) {
  struct Seen {
    uint8_t edge;
    std::chrono::steady_clock::time_point at;
  };
  thread_local std::unordered_map<uint32_t, Seen> seen;
  const auto now = std::chrono::steady_clock::now();
  auto [it, added] = seen.try_emplace(node, Seen{edge, now});
  if (!added && now - it->second.at > std::chrono::seconds(1)) {
    it->second.edge = edge;
  }
  it->second.at = now;
  return it->second.edge;
}

// The announcements (TAKEDOWN!, 6th PLACE ELIMINATED!, a medal) share one
// layer that no HUD component owns, each laid out around its panel
// (CB4HUDMessagePanel2dObject, drawn before the rest of it): a point, 2 by
// 1.5, at x 60 for a takedown's in the top left corner and at 467 to 603 for
// an elimination's in the middle. The icon, text and stars that follow go
// where the panel is, together - classified one by one, an elimination's
// skull went left and its text right.
uint8_t PanelEdge(const uint8_t* base, uint32_t rect) {
  const float x = GuestFloat(base, rect) + GuestFloat(base, rect + 8) * 0.5f;
  // 15%: GREAT boosting!'s panel is at 317, a quarter in, and its message is
  // centred all the same.
  return x < kConsoleWidth * 0.15f ? 1 : x > kConsoleWidth * 0.85f ? 3 : 2;
}

// XERENGE_HUD_LOG: an element's rectangle and edge, when the edge it gets
// changes (the elements of a race are drawn every frame). The element is its
// list node (RenderObjects's r31): one render function draws many elements.
void LogElement(const char* name, uint32_t node, const uint8_t* base, uint32_t rect,
                uint8_t edge, uint8_t layer_edge) {
  static const bool on = std::getenv("XERENGE_HUD_LOG") != nullptr;
  if (!on) {
    return;
  }
  static std::mutex mutex;
  static std::unordered_map<uint32_t, uint8_t> last;
  {
    std::lock_guard lock(mutex);
    auto [it, added] = last.try_emplace(node, edge);
    if (!added && it->second == edge) {
      return;
    }
    it->second = edge;
  }
  REXLOG_INFO("widescreen: element {:08X} {} layer {:08X} x {:.1f} y {:.1f} w {:.1f} h {:.1f} -> {}{}",
              node, name, xerenge::LoadGuestU32(base, node + 64), GuestFloat(base, rect),
              GuestFloat(base, rect + 4), GuestFloat(base, rect + 8), GuestFloat(base, rect + 12),
              edge == 1 ? "left" : edge == 3 ? "right" : "middle",
              layer_edge != 0 ? " (its component's)" : "");
}

}  // namespace

// An element's render, numbered as an element only when RenderObjects's loop
// calls it: the same functions are also called from inside other elements
// (the boost bar draws its label with CB4HUDText2dObject::_RenderSolidFill),
// and there they are part of the element calling them. In a layer a HUD
// component anchored, the element goes with its layer - the CGt2dLayer its
// list node (RenderObjects's r31) holds at +64, whose position and scale
// RenderObjects places it by; the rules below are for the layers no
// component owns. (RenderObjects's own r3 is the scene's slot for the list,
// not a CGt2dLayer.)
constexpr uint32_t kNodeLayer = 64;
#define ELEMENT_HOOK_FULL(name, depth, centred, sticky, panel)           \
  extern "C" void __imp__##name(PPCContext& __restrict, uint8_t*);       \
  REX_HOOK_RAW(name) {                                                   \
    if (uint32_t(ctx.lr) != kRenderObjectsLoop) {                        \
      __imp__##name(ctx, base);                                          \
      return;                                                            \
    }                                                                    \
    FlushVertexBatch(ctx, base);                                         \
    g_object_serial = g_object_serial == ~0u ? 1u : g_object_serial + 1; \
    const uint8_t outer_edge = g_element_edge;                           \
    const uint8_t layer_edge =                                           \
        LayerEdge(xerenge::LoadGuestU32(base, ctx.r31.u32 + kNodeLayer)); \
    if (panel) {                                                         \
      g_message_edge = PanelEdge(base, ctx.r3.u32);                      \
    }                                                                    \
    g_element_edge = &depth == &g_interface_fill_depth ? 2               \
                     : layer_edge != 0                 ? layer_edge      \
                     : g_message_edge != 0             ? g_message_edge  \
                     : (centred)                       ? 2               \
                     : (sticky) ? StickyEdge(ctx.r31.u32, EdgeOf(base, ctx.r3.u32)) \
                                : EdgeOf(base, ctx.r3.u32);              \
    LogElement(#name, ctx.r31.u32, base, ctx.r3.u32, g_element_edge, layer_edge); \
    ++depth;                                                             \
    __imp__##name(ctx, base);                                            \
    FlushVertexBatch(ctx, base);                                         \
    --depth;                                                             \
    g_element_edge = outer_edge;                                         \
  }
#define ELEMENT_HOOK(name) ELEMENT_HOOK_FULL(name, g_interface_depth, false, false, false)
// ... an element that keeps its first edge (StickyEdge).
#define ELEMENT_STICKY_HOOK(name) ELEMENT_HOOK_FULL(name, g_interface_depth, false, true, false)
// ... an element that stays in the middle wherever it is: the announcements
// (a medal, AWESOME CHECKING!) are laid out as several elements around the
// middle, and one of them moved to an edge left its text behind.
#define ELEMENT_CENTRED_HOOK(name) ELEMENT_HOOK_FULL(name, g_interface_depth, true, false, false)
// ... an announcement's panel: the elements after it in its layer go where it
// is (PanelEdge).
#define ELEMENT_PANEL_HOOK(name) ELEMENT_HOOK_FULL(name, g_interface_depth, true, false, true)
// ... an element drawn across the whole screen.
#define ELEMENT_FILL_HOOK(name) ELEMENT_HOOK_FULL(name, g_interface_fill_depth, true, false, false)

// The element renders a race's HUD was seen to use (names from Beta 5's
// symbols, carried over by opcode match).
ELEMENT_HOOK(sub_8215B940)  // CB4EATrax2dObject::_Render
ELEMENT_HOOK(sub_82186F30)  // C2dObjectBoostBarMessage::_Render
ELEMENT_PANEL_HOOK(sub_82188FF0)  // CB4HUDMessagePanel2dObject::_Render
ELEMENT_CENTRED_HOOK(sub_82189238)  // CB4HUDMessageStars2dObject::_Render
ELEMENT_HOOK(sub_8218AA90)  // CB4HUDRatingBar::_Render
ELEMENT_HOOK(sub_8218AFC8)  // CB4HUDRotateImage2dObject::_Render
ELEMENT_HOOK(sub_8218B458)  // CB4HUDSmallPanel2dObject::_Render
ELEMENT_HOOK(sub_8218C168)  // CB4HUDSmallPanel2dObject::_RenderCenter
ELEMENT_HOOK(sub_8218C3C8)  // CB4HUDLargePanel2dObject::_Render
ELEMENT_HOOK(sub_8218E0C8)  // CB4HUDText2dObject::_RenderSolidFill
ELEMENT_HOOK(sub_8218E128)  // CB4HUDText2dObject::_RenderVGradientEmboss
ELEMENT_CENTRED_HOOK(sub_8218E248)  // CB4HUDText2dObject::_RenderMessage1In
ELEMENT_CENTRED_HOOK(sub_8218E730)  // CB4HUDText2dObject::_RenderMessage1Out
ELEMENT_CENTRED_HOOK(sub_8218F0A8)  // CB4HUDText2dObject::_RenderVGradientEmbossZoomFlash
ELEMENT_HOOK(sub_8218F978)  // CB4HUDText2dObject::_RenderRatingText
ELEMENT_HOOK(sub_8218FC60)  // CB4HUDText2dObject::_RenderDigitalTimer
ELEMENT_HOOK(sub_82197A88)  // C2dObjectBoostBar::_Render
// A button prompt (a crash's LS AFTERTOUCH). Placed by the renderer's guess
// from its draws' extent, it went between its edge and the middle as the
// stick's icon animated.
ELEMENT_HOOK(sub_8219ACB0)  // CB4HUDDPad2dObject::_Render
ELEMENT_STICKY_HOOK(sub_8235DB20)  // CGt2dObjectImageRotatable::_Render (the rating's star)
// The rest of the element renders the 2D layer has. One left unhooked only
// batches its vertices, and they went to Direct3D when the next hooked element
// flushed the batch, before its edge was set: a crash's (A) stayed in the
// middle while IMPACT TIME, its text, went to the right edge.
ELEMENT_HOOK(sub_8235DA90)  // CGt2dObjectImage::_Render
ELEMENT_HOOK(sub_8235DA18)  // CGt2dObjectBox::_Render
ELEMENT_HOOK(sub_8235DF90)  // CGt2dObjectParagraph::_RenderPlain
ELEMENT_HOOK(sub_8235E100)  // CGt2dObjectParagraph::_RenderDropShadow
ELEMENT_HOOK(sub_8235E2A8)  // CGt2dObjectParagraph::_RenderOutline
ELEMENT_HOOK(sub_8235E678)  // CGt2dObjectVignette::_RenderHorizontal
ELEMENT_HOOK(sub_8235E8B0)  // CGt2dObjectVignette::_RenderVertical
ELEMENT_HOOK(sub_821907B8)  // CB4HUDTrafficAttackBar2dObject::_Render
ELEMENT_HOOK(sub_8218DDA8)  // CB4HUDText2dObject::_RenderOutline
ELEMENT_HOOK(sub_8218DF38)  // CB4HUDText2dObject::_RenderColourVerticesOutline
ELEMENT_HOOK(sub_8218FD58)  // CB4HUDText2dObject::_Render2dGeometryGradientText
// Not CB4HUDText2dObject::_RenderMessage2 and _RenderMessage3 (0x8218EAB0,
// 0x8218F3C8): a crash's TOTALLED! comes with a tint over the whole screen,
// and with the batch flushed around it the tint came out solid brown for a
// frame every few tenths of a second.
// The border a crash or a takedown is framed in: black bars over the top and
// bottom of the whole screen, not of its middle.
ELEMENT_FILL_HOOK(sub_82187BE0)  // CB4HUDBorder2dObject::_Render

// CB4GraphicsManagerBase::RenderFullScreenPass - a pass over the whole screen,
// such as the flash the front end draws when a car is won (from
// CB4FlashManager::Render, whose draws are otherwise kept in the box).
extern "C" void __imp__sub_82142CD8(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_82142CD8) {
  ++g_interface_fill_depth;
  __imp__sub_82142CD8(ctx, base);
  --g_interface_fill_depth;
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
