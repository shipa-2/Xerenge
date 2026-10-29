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
// ... the kind drawn across the whole screen: overlays over all of the
// console's 16:9 screen (the crash border, the front end's full-screen flash).
bool DrawingInterfaceFill();
// Where the HUD element being drawn goes, from its rectangle in the title's
// layout: 0 undecided, 1 the left edge, 2 the middle, 3 the right edge.
uint8_t InterfaceEdge();
// The 2D object being drawn (CGt2dLayer::RenderObjects draws one at a time),
// numbered; 0 outside one.
uint32_t InterfaceObject();

// Around the title's calls that draw interface (game_timing.cpp hooks the
// menus' and the revenge meter's renders).
void EnterInterface();
void LeaveInterface();
// ... around one whose draws go over the whole frame (DrawingInterfaceFill):
// the revenge meter, drawn through the race's widened camera.
void EnterInterfaceFill();
void LeaveInterfaceFill();
// Around CCalVideoRenderer::Render (movie_frames.cpp): while a menu is shown
// over it, the video is a menu's background, and with XERENGE_MENU_VIDEO not
// "box" it is marked to cover the whole frame (DrawingInterfaceFill), zoomed
// or stretched by the renderer. Leave only when Enter answered true.
bool EnterMenuVideo();
// From CB4AptManager::Render (xerenge_menu.cpp): a menu is being drawn this
// frame - what EnterMenuVideo asks.
void NoteMenuDrawn();
void LeaveMenuVideo();
// ... and around one that draws HUD objects (CGt2dLayer::RenderObjects):
// numbered, so each goes to its edge.
void EnterInterfaceObject();
void LeaveInterfaceObject();

// XERENGE_HUD_TRACE: the guest call chain of an interface draw, from the
// Direct3D call's return address (lr) and the caller's stack pointer (r1).
void TraceInterfaceDraw(const char* what, uint32_t lr, uint32_t sp, const uint8_t* base);

}  // namespace xerenge
