// The cars' reflection cube, drawn a few faces a frame.
//
// The title draws all six faces of the cube every frame - the scene six more
// times, at 128x128: in a race about 700 of the frame's 1250 draws, more than
// the frame itself. XERENGE_REFLECTION_FACES=1, 2 or 3 makes it draw that many
// a frame, in turn (2: faces 0 and 3, 1 and 4, 2 and 5 together, each
// face every third frame; the settings' reflection_split, "Reflections in
// turns", sets 2).
// The others keep what they were last drawn with, as on the console when the
// frame rate drops - the reflection of something passing lags a frame or two.
//
// Done in the title's own loops over the faces - sub_8216A448 in a race (the
// loop at 0x8216A5F8) and sub_8216A7B8 in the garage (0x8216A93C), the face in
// r26: the hook is asked at the top of each turn and, for a face left out, the
// loop goes straight to its increment ([[midasm_hook]] in
// functions.toml). Nothing a turn sets is read by the next.
// XERENGE_REFLECTION_SPLIT, the renderer's own version of this, only spares
// the renderer the draws; this spares the title making them.
#include <cstdint>
#include <cstdlib>

#include <rex/ppc/context.h>

bool ReflectionFaceSkipped(PPCRegister& r26) {
  static const uint32_t groups = [] {
    const char* text = std::getenv("XERENGE_REFLECTION_FACES");
    const unsigned long faces = text && *text ? std::strtoul(text, nullptr, 10) : 6;
    return faces == 1 ? 6u : faces == 2 ? 3u : faces == 3 ? 2u : 1u;
  }();
  if (groups == 1) {
    return false;
  }
  // The title's thread alone runs the loop.
  static uint32_t rounds = 0;
  const uint32_t face = r26.u32;
  if (face == 0) {
    ++rounds;
  }
  // Every face for the first rounds, so none starts out empty.
  if (rounds <= 12 || face >= 6) {
    return false;
  }
  return face % groups != rounds % groups;
}
