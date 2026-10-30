#pragma once

#include <cstdint>
#include <string>

namespace xerenge::music {

// The music folder's songs chosen in Song Manager, in place of EA Trax.
bool Custom();

// The folder looked at again (Song Manager opening): songs added or taken away
// since are in the list.
void Rescan(uint8_t* base);

}  // namespace xerenge::music

namespace xerenge::music {

// Every frame of the menus, from the first: the folder looked at once and, when
// its songs were chosen at an earlier start, put in place before the game
// plays its first song - not only once Song Manager or a race asks for them.
void Prepare(uint8_t* base);

}  // namespace xerenge::music

namespace xerenge::music {

// Song Manager on show (xerenge_menu.cpp): LB/RB turn the music folder's pages.
void SetSongManagerShown(bool shown);

// Song Manager's subtitle with the folder's songs: [MY MUSIC], and the page,
// [MY MUSIC 3/35], when there is more than one.
std::string Title();

}  // namespace xerenge::music
