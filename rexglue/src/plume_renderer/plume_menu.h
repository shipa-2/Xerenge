/**
 * @file        plume_renderer/plume_menu.h
 * @brief       The in-game settings menu (F1, or Back and Start on a pad).
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "menu_pad.h"

struct ImDrawData;

namespace rex::ui {
class Window;
}

namespace rex::plume_renderer {

// A menu of the settings, drawn over the frame by Dear ImGui. It is the model
// (the pages and what each item does), the input (the keyboard through the
// window, the pad through the title's own reads - see menu_input.cpp) and the
// layout; PlumeMenuDraw puts what it lays out on the screen.
//
// Adding an item: append to the page it belongs on in BuildPages
// (plume_menu.cpp). One that applies at once gives `read` and `apply`, which
// talk to the runtime state in plume_settings.h; one without them is saved to
// xerenge.conf and applies after a restart. `key` is its line in that file,
// the launcher's name for it.
class PlumeMenu {
 public:
  static PlumeMenu& Get();

  // The window the keyboard comes from. While the menu is open its events are
  // taken, so the title never sees them.
  void AttachWindow(rex::ui::Window* window);

  bool Visible() const { return visible_.load(std::memory_order_acquire); }
  void Toggle();
  // by_pad: closed with a button, which the title is not to see let go.
  void Close(bool by_pad);

  // The pad the title just read, for user `user`. Returns whether the menu is
  // holding the pad: the title is then to be given a neutral one. Otherwise
  // the title is given `pad`'s buttons as this leaves them - Back and Start
  // held back for a moment, in case they are the chord.
  bool OnPad(uint32_t user, xerenge::MenuPadState* pad);

  // Lays out the menu for a frame of this size and returns what to draw, or
  // null when it is closed. Called by the thread that renders.
  ImDrawData* Frame(uint32_t width, uint32_t height);

  // What the keyboard typed while the menu was open (the window's thread).
  void PushKey(uint16_t virtual_key);
  void PushCharacter(char32_t character);

  // A pointer: the mouse, or a finger on a phone. x and y are fractions of the
  // surface, 0 to 1 from the top left; so the same call serves a window in
  // physical pixels and a touch view of another size.
  enum class PointerEvent { kDown, kMove, kUp };
  void PushPointer(PointerEvent event, float x, float y);
  // The wheel, or a drag: rows to scroll the list by, down being positive.
  void PushScroll(float rows);

 private:
  PlumeMenu();
  ~PlumeMenu();

  std::atomic<bool> visible_{false};

  std::mutex input_mutex_;
  std::vector<uint16_t> pending_keys_;
  std::vector<char32_t> pending_characters_;
  struct Pointer {
    PointerEvent event;
    float x;
    float y;
  };
  std::vector<Pointer> pending_pointers_;
  float pending_scroll_ = 0.0f;
  xerenge::MenuPadState pad_;
  std::chrono::steady_clock::time_point pad_time_{};
  bool chord_was_down_ = false;
  // After the menu closes by the pad, the title is kept from seeing the
  // buttons that were used until they are let go.
  bool hold_pad_ = false;
  // Back and Start on their way to the title, each (see OnPad): up; held back,
  // since `since`; passed on, the other one not having followed in time; or
  // the menu's, kept from the title until let go. A press let go while held
  // back is shown to the title until `late_until`.
  enum class ChordButton { kUp, kHeldBack, kPassed, kUsed };
  struct ChordState {
    ChordButton state = ChordButton::kUp;
    std::chrono::steady_clock::time_point since{};
    std::chrono::steady_clock::time_point late_until{};
  };
  ChordState chord_[2];

  // The rendering thread's: the pages, the selection, the ImGui context.
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace rex::plume_renderer
