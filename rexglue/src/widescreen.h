/**
 * @file        widescreen.h
 * @brief       Screens wider than 16:9: what the title is drawing (widescreen.cpp).
 */
#pragma once

#include <cstdint>

namespace xerenge {

// Whether the title's thread is inside its 2D layer (the interface, the menus)
// right now - the draws it makes there keep the console's 16:9 box when the
// scene is widened to the screen.
bool DrawingInterface();
// ... the kind kept against the screen's left edge (the music player's panel).
bool DrawingInterfaceLeft();
// The 2D object being drawn (CGt2dLayer::RenderObjects draws one at a time),
// numbered; 0 outside one.
uint32_t InterfaceObject();

// Around the title's calls that draw interface (game_timing.cpp hooks the
// menus' and the revenge meter's renders).
void EnterInterface();
void LeaveInterface();

}  // namespace xerenge
