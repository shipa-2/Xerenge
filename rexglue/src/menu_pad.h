/**
 * @file        menu_pad.h
 * @brief       What the executable and the in-game menu hand each other.
 *
 * The menu lives in the plume plugin; the game's pad is read in the executable
 * (menu_input.cpp), and on Android the touch view is too (android_touch.cpp).
 * They meet at a few exported functions of the plugin, found by name.
 */
#pragma once

#include <cstdint>

namespace xerenge {

// A pad as the game reads it (XINPUT_GAMEPAD).
struct MenuPadState {
  uint16_t buttons = 0;
  uint8_t left_trigger = 0;
  uint8_t right_trigger = 0;
  int16_t thumb_lx = 0;
  int16_t thumb_ly = 0;
  int16_t thumb_rx = 0;
  int16_t thumb_ry = 0;
};

// Called for every read of a pad by the game. The menu takes the pad of user 0
// as its own; with the menu open it returns true, and the game is to be given a
// pad nobody touches. Otherwise the game is given `pad->buttons` as the menu
// leaves them: Back and Start are held back for a moment, as they may be the
// start of the chord that opens the menu.
inline constexpr const char* kMenuPadSymbol = "xerenge_menu_pad";
using MenuPadFn = bool (*)(uint32_t user, MenuPadState* pad);

// A finger on the menu: event 0 down, 1 move, 2 up; x and y fractions of the
// screen, from the top left.
inline constexpr const char* kMenuPointerSymbol = "xerenge_menu_pointer";
using MenuPointerFn = void (*)(int event, float x, float y);

inline constexpr const char* kMenuToggleSymbol = "xerenge_menu_toggle";
using MenuToggleFn = void (*)();

inline constexpr const char* kMenuVisibleSymbol = "xerenge_menu_visible";
using MenuVisibleFn = bool (*)();

// A function the plume plugin exports, or null: under any other renderer, or
// before there is one.
void* FindMenuSymbol(const char* name);

}  // namespace xerenge
