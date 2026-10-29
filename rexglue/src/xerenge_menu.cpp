// Xerenge's settings in Driver Details > Settings: moving between its pages,
// and the natives its rows call (settings_movie.cpp builds the pages,
// xerenge_settings.cpp holds the settings).
//
// The menus run on a service ([0x82A522A0]) that the interface polls for the
// next movie to load (+20, sub_821E6BA8 hands it over and clears it), and
// that MenuScreenManager.SelectedOption(n) reaches as sub_821EC580(service, n)
// - the game's menu state machine. Our buttons hand it 0x58001-0x58003 (a
// page); back on the screen hands it -1. Both are taken here when they are
// ours: the Settings movie asked for again, built for the page.
//
// A row's values come from the natives XerengeGet(id) and XerengeSet(id * 16
// + option), registered beside the screen's own (DriverDetailsSettings's,
// sub_8242BDB0 from its adapter) as AptNativeFunction objects (sub_8242B338
// to allocate, sub_821AD188 to construct) around guest addresses the SDK
// routes to the functions below (FunctionDispatcher::AllocateThunk).
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <string>

#include <rex/hook.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>

#include "custom_music.h"
#include "mods.h"
#include "guest_memory.h"
#include "widescreen.h"
#include "xerenge_settings.h"

extern "C" void __imp__sub_821EC580(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_821E6BA8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8242BDB0(PPCContext& __restrict, uint8_t*);
extern "C" void sub_8242B338(PPCContext& __restrict, uint8_t*);
extern "C" void sub_821AD188(PPCContext& __restrict, uint8_t*);
extern "C" void sub_8242C678(PPCContext& __restrict, uint8_t*);
extern "C" void sub_821C84A8(PPCContext& __restrict, uint8_t*);
extern "C" void sub_821C7588(PPCContext& __restrict, uint8_t*);
extern "C" void sub_821E6928(PPCContext& __restrict, uint8_t*);
extern "C" void sub_822ADE70(PPCContext& __restrict, uint8_t*);
bool XerengeCanRestart();
void XerengeRestartGame();

namespace {

namespace settings = xerenge::settings;

constexpr uint32_t kMenuService = 0x82A522A0;   // holds the service's address
constexpr int32_t kOurOptions = 0x58000;         // + page (settings_movie.cpp)
constexpr uint32_t kAptAllocator = 0x82D39804;   // what sub_8242B338 allocates from
constexpr uint32_t kAptStack = 0x82D3A1B0;       // {count, ?, values}: the natives' arguments

uint32_t LoadU32(uint8_t* base, uint32_t address) {
  uint32_t raw = 0;
  std::memcpy(&raw, xerenge::GuestPointer(base, address), sizeof(raw));
  return __builtin_bswap32(raw);
}

void StoreU32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(xerenge::GuestPointer(base, address), &value, sizeof(value));
}

std::string GuestString(uint8_t* base, uint32_t address) {
  std::string text;
  if (address < 0x10000) {
    return text;
  }
  for (const char* c = reinterpret_cast<const char*>(xerenge::GuestPointer(base, address));
       *c && text.size() < 128; ++c) {
    text += *c;
  }
  return text;
}

uint32_t GuestCString(uint8_t* base, const char* text);

bool g_on_settings = false;  // the Settings screen is the one on show

// Whether a movie path names the Settings screen, whatever its letters' case.
bool IsSettings(const std::string& path) {
  for (const char* name : {"/driverdetailssettings", "/xerengesettingsa", "/xerengesettingsb"}) {
    const size_t length = std::strlen(name);
    if (path.size() >= length &&
        std::equal(name, name + length, path.end() - length,
                   [](char a, char b) { return a == std::tolower(static_cast<unsigned char>(b)); })) {
      return true;
    }
  }
  return false;
}

// The Settings screen asked for again, to show another page. Apt does not load
// again a movie it is showing - by its name, whatever its letters' case - so
// the screen is asked for under names of ours, two in turn, which the load
// below turns back into the Settings movie's.
void ShowSettings(uint8_t* base) {
  static uint32_t spellings[2] = {};
  static int next = 0;
  if (!spellings[0]) {
    spellings[0] = GuestCString(base, "../Burnout4/Menu/DriverDetails/XerengeSettingsA");
    spellings[1] = GuestCString(base, "../Burnout4/Menu/DriverDetails/XerengeSettingsB");
  }
  StoreU32(base, LoadU32(base, kMenuService) + 20, spellings[next]);
  next ^= 1;
}

// Another Driver Details screen's movie asked for: Stats's, which the load
// (settings_movie.cpp) fills with the achievements, and Player File's again,
// back from them.
void ShowMovie(uint8_t* base, const char* path) {
  StoreU32(base, LoadU32(base, kMenuService) + 20, GuestCString(base, path));
}

// The natives' one argument, as an integer: the top of the interpreter's
// stack, through the interpreter's own conversion.
int32_t Argument(PPCContext& ctx, uint8_t* base) {
  const uint32_t count = LoadU32(base, kAptStack);
  const uint32_t values = LoadU32(base, kAptStack + 8);
  if (!count || !values) {
    return 0;
  }
  ctx.r3.u64 = LoadU32(base, values + (count - 1) * 4);
  sub_8242C678(ctx, base);
  return int32_t(ctx.r3.u32);
}

// XerengeGet(id): the option the row is at. The integer value it returns is
// made by the screen's GetSpeakerConfig (an integer of the right kind, from
// the interpreter's pool), its number put in.
void XerengeGet(PPCContext& ctx, uint8_t* base) {
  const int32_t id = Argument(ctx, base);
  const int option = settings::Get(id);
  REXLOG_INFO("menu: XerengeGet({}) = {}", id, option);
  sub_821C84A8(ctx, base);
  if (ctx.r3.u32) {
    StoreU32(base, ctx.r3.u32 + 8, uint32_t(option));
  }
}

// XerengeSet(id * 16 + option). Returns what GetTakedownCamera does - the
// rows do not read it.
void XerengeSet(PPCContext& ctx, uint8_t* base) {
  const int32_t code = Argument(ctx, base);
  REXLOG_INFO("menu: XerengeSet({})", code);
  settings::Set(code / 16, code % 16);
  sub_821C7588(ctx, base);
}

uint32_t GuestCString(uint8_t* base, const char* text) {
  const uint32_t size = uint32_t(std::strlen(text) + 1);
  const uint32_t at = REX_KERNEL_MEMORY()->SystemHeapAlloc(size, 0x10);
  std::memcpy(xerenge::GuestPointer(base, at), text, size);
  return at;
}

void Register(PPCContext& ctx, uint8_t* base, uint32_t object, const char* name, uint32_t function) {
  static auto* names = new std::map<std::string, uint32_t>;
  uint32_t& guest_name = (*names)[name];
  if (!guest_name) {
    guest_name = GuestCString(base, name);
  }
  ctx.r3.u64 = LoadU32(base, kAptAllocator);
  ctx.r4.u64 = 36;
  sub_8242B338(ctx, base);
  const uint32_t native = ctx.r3.u32;
  if (!native) {
    return;
  }
  ctx.r3.u64 = native;
  ctx.r4.u64 = function;
  sub_821AD188(ctx, base);
  ctx.r3.u64 = object;
  ctx.r4.u64 = guest_name;
  ctx.r5.u64 = native;
  __imp__sub_8242BDB0(ctx, base);
}

}  // namespace

// Called by menu_trace.cpp's hook on sub_8242BDB0 after the screen's adapter
// hands it IsInControl, its last: ours beside its own, on the same object.
void XerengeMenuRegisterNatives(PPCContext& ctx, uint8_t* base, uint32_t object) {
  static uint32_t get = 0, set = 0;
  if (!get) {
    auto* dispatcher = rex::Runtime::instance()->function_dispatcher();
    get = dispatcher->AllocateThunk(&XerengeGet, 0);
    set = dispatcher->AllocateThunk(&XerengeSet, 0);
    REXLOG_INFO("menu: XerengeGet at {:08X}, XerengeSet at {:08X}", get, set);
  }
  if (!get || !set) {
    return;
  }
  const PPCContext saved = ctx;
  Register(ctx, base, object, "XerengeGet", get);
  Register(ctx, base, object, "XerengeSet", set);
  ctx = saved;
}

// The restart popup, when Settings is left after our pages changed something
// that takes effect at a start: the game's own popup (the one a failed
// connection to EA's servers shows), asked for through sub_821E6928 on the
// popup service ([0x82A52288]) with a callback of ours, which the service
// calls with the button pressed (sub_821EE6F8). A starts the game again (its
// launcher reads xerenge.conf anew); B leaves Settings as back would have.
namespace {

constexpr uint32_t kPopupService = 0x82A52288;  // holds the service's address
constexpr uint32_t kEmptyTitle = 0x82010E58;    // "", the title the game's popups have
constexpr uint32_t kPopupButtons = 4;           // ePopupType_YesNo (Cafe.bin's popup manager): A and B
bool g_visited_pages = false;                   // our pages were opened since Settings was
bool g_popup_pending = false;                   // the popup to ask for at the next frame
int g_achievement_popup = -1;                   // the achievement whose popup to show next
constexpr uint32_t kPopupOk = 1;                // ePopupType_OK: A alone, "OK"
constexpr int32_t kAchievementOptions = 0x58100;  // + its row: A on it in the list (settings_movie.cpp)

// The popup's title and text, the keys below give (LocalisedText).
std::string g_popup_title, g_popup_text;

void RestartAnswer(PPCContext& ctx, uint8_t* base) {
  const uint32_t button = ctx.r3.u32;
  REXLOG_INFO("menu: restart popup answered with button {}", button);
  if (button == 2) {  // Yes, the A button, in a YesNo popup
    XerengeRestartGame();
  }
  // Cancelled (or the restart could not start): leave Settings as back does.
  const PPCContext saved = ctx;
  ctx.r3.u64 = LoadU32(base, kMenuService);
  ctx.r4.u64 = uint32_t(-1);
  __imp__sub_821EC580(ctx, base);
  ctx = saved;
}

bool ShowRestartPopup(PPCContext& ctx, uint8_t* base) {
  static uint32_t answer = 0, message = 0;
  if (!answer) {
    answer = rex::Runtime::instance()->function_dispatcher()->AllocateThunk(&RestartAnswer, 0);
    message = GuestCString(base, "$XerengeRestart");  // the text: LocalisedText below
  }
  if (!answer) {
    return false;
  }
  const PPCContext saved = ctx;
  // The two arguments past r10, in the caller's parameter area.
  StoreU32(base, ctx.r1.u32 + 84, 0);
  StoreU32(base, ctx.r1.u32 + 92, 0);
  ctx.r3.u64 = LoadU32(base, kPopupService);
  ctx.r4.u64 = kEmptyTitle;
  ctx.r5.u64 = message;
  ctx.r6.u64 = kPopupButtons;
  ctx.r7.u64 = answer;
  ctx.r8.u64 = 0;
  ctx.r9.u64 = 12;
  ctx.r10.u64 = 0;
  REXLOG_INFO("menu: restart popup asked for");
  sub_821E6928(ctx, base);
  ctx = saved;
  return true;
}

// An achievement's popup, from the Stats-like list: its name over what it is
// for (or how to earn it), its score and when it was earned; OK closes it.
void AchievementAnswer(PPCContext& ctx, uint8_t*) {
  REXLOG_INFO("menu: achievement popup closed (button {})", ctx.r3.u32);
}

void ShowAchievementPopup(PPCContext& ctx, uint8_t* base, int index) {
  const auto rows = settings::AchievementRows();
  if (index < 0 || size_t(index) >= rows.size()) {
    return;
  }
  const auto& row = rows[size_t(index)];
  g_popup_title = row.name;
  g_popup_text = row.description + "\n\n" +
                 (row.state == "LOCKED" ? row.score + " - LOCKED" : row.score + " - EARNED " + row.state);
  static uint32_t answer = 0, title = 0, text = 0;
  if (!answer) {
    answer = rex::Runtime::instance()->function_dispatcher()->AllocateThunk(&AchievementAnswer, 0);
    title = GuestCString(base, "$XerengeAchievementTitle");
    text = GuestCString(base, "$XerengeAchievementText");
  }
  if (!answer) {
    return;
  }
  const PPCContext saved = ctx;
  StoreU32(base, ctx.r1.u32 + 84, 0);
  StoreU32(base, ctx.r1.u32 + 92, 0);
  ctx.r3.u64 = LoadU32(base, kPopupService);
  ctx.r4.u64 = title;
  ctx.r5.u64 = text;
  ctx.r6.u64 = kPopupOk;
  ctx.r7.u64 = answer;
  ctx.r8.u64 = 0;
  ctx.r9.u64 = 12;
  ctx.r10.u64 = 0;
  REXLOG_INFO("menu: achievement popup for row {} ({})", index, row.name);
  sub_821E6928(ctx, base);
  ctx = saved;
}

// B on the main menu, where the game takes no B: a popup asking whether to quit
// the game; Yes (A) ends it, as a restart does, No (B) stays. The pad as the
// game will read it, at each frame of the interface, for B as it goes down.
bool g_on_main_menu = false;
bool g_quit_popup_open = false;

void QuitAnswer(PPCContext& ctx, uint8_t*) {
  g_quit_popup_open = false;
  REXLOG_INFO("menu: quit popup answered with button {}", ctx.r3.u32);
  if (ctx.r3.u32 == 2) {  // Yes, A, in a YesNo popup
    std::_Exit(0);
  }
}

void CheckQuitButton(PPCContext& ctx, uint8_t* base) {
  static bool was_down = false;
  auto* kernel_state = REX_KERNEL_STATE();
  auto* input = kernel_state ? static_cast<rex::input::InputSystem*>(kernel_state->emulator()->input_system()) : nullptr;
  rex::input::X_INPUT_STATE state{};
  const bool down = input && input->GetState(0, &state) == 0 && (uint16_t(state.gamepad.buttons) & 0x2000);
  const bool pressed = down && !was_down;
  was_down = down;
  if (!pressed || !g_on_main_menu || g_quit_popup_open) {
    return;
  }
  static uint32_t answer = 0, message = 0;
  if (!answer) {
    answer = rex::Runtime::instance()->function_dispatcher()->AllocateThunk(&QuitAnswer, 0);
    message = GuestCString(base, "$XerengeQuit");
  }
  if (!answer) {
    return;
  }
  g_quit_popup_open = true;
  const PPCContext saved = ctx;
  StoreU32(base, ctx.r1.u32 + 84, 0);
  StoreU32(base, ctx.r1.u32 + 92, 0);
  ctx.r3.u64 = LoadU32(base, kPopupService);
  ctx.r4.u64 = kEmptyTitle;
  ctx.r5.u64 = message;
  ctx.r6.u64 = kPopupButtons;
  ctx.r7.u64 = answer;
  ctx.r8.u64 = 0;
  ctx.r9.u64 = 12;
  ctx.r10.u64 = 0;
  REXLOG_INFO("menu: quit popup asked for");
  sub_821E6928(ctx, base);
  ctx = saved;
}

}  // namespace

REX_HOOK_RAW(sub_821EC580) {
  const int32_t option = int32_t(ctx.r4.u32);
  REXLOG_INFO("menu: SelectedOption({:X})", uint32_t(option));
  // A on an achievement in the list: its popup, at the next frame (below).
  if (option >= kAchievementOptions && option < kAchievementOptions + 0x100) {
    g_achievement_popup = option - kAchievementOptions;
    return;
  }
  // The achievements, Player File's button: in Stats's movie; back from them, to
  // Player File.
  if (option == kOurOptions + settings::kAchievements) {
    settings::SetPage(settings::kAchievements);
    ShowMovie(base, "../Burnout4/Menu/DriverDetails/DriverDetailsStats");
    return;
  }
  if (option == -1 && settings::CurrentPage() == settings::kAchievements) {
    settings::SetPage(settings::kMain);
    ShowMovie(base, "../Burnout4/Menu/DriverDetails/DriverDetailsProfile");
    return;
  }
  if (option > kOurOptions && option <= kOurOptions + 3) {
    settings::SetPage(settings::Page(option - kOurOptions));
    g_visited_pages = true;
    REXLOG_INFO("menu: Xerenge settings page {}", option - kOurOptions);
    ShowSettings(base);
    return;
  }
  if (option == -1 && g_on_settings) {
    if (settings::CurrentPage() != settings::kMain) {
      settings::SetPage(settings::kMain);
      ShowSettings(base);
      return;
    }
    // Leaving Settings: the restart popup, if our pages were opened and a
    // change wants a start; its answer goes on from there.
    const bool restart = settings::TakeRestartNeeded();
    const bool visited = g_visited_pages;
    g_visited_pages = false;
    if (restart && visited && XerengeCanRestart()) {
      // Not from here, inside the interface's code: asked for before it runs next (below).
      g_popup_pending = true;
      return;
    }
  }
  __imp__sub_821EC580(ctx, base);
}

// The movie the interface loads next: the screen on show.
//
// GitHub #29: leaving a reward video's screen (a challenge complete, a rank
// up...) the soundtrack, paused for the video (sub_821F50F8), is resumed. The
// game resumes it only when the interface's video has played to its end
// (sub_822ADEE8 waits for that), and skipping one with START leaves it playing
// unseen - the music came back a minute or two later, the video's length.
REX_HOOK_RAW(sub_821E6BA8) {
  const uint32_t requested = LoadU32(base, ctx.r3.u32 + 20);
  __imp__sub_821E6BA8(ctx, base);
  if (requested) {
    static std::string on_show;
    const std::string next = GuestString(base, requested);
    g_on_settings = IsSettings(next);
    g_on_main_menu = next.size() >= 14 && next.compare(next.size() - 14, 14, "/Main/MainMenu") == 0;
    constexpr uint32_t kMusic = 0x8268748C;  // the soundtrack's state; +780: paused for a video
    const bool left_video = on_show.size() > 5 && on_show.compare(on_show.size() - 5, 5, "Video") == 0 &&
                            next != on_show;
    if (left_video && LoadU32(base, kMusic + 780) != 0) {
      REXLOG_INFO("menu: music resumed, leaving {}", on_show);
      const PPCContext saved = ctx;
      ctx.r3.u64 = kMusic;
      sub_822ADE70(ctx, base);
      ctx = saved;
    }
    on_show = next;
  }
}

// CB4AptManager::AptCBLoadAnimation(name, file): Apt asking for a movie's data.
extern "C" void __imp__sub_821FF408(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_821FF408) {
  const std::string name = GuestString(base, ctx.r3.u32);
  REXLOG_INFO("menu: Apt loads {}", name);
  // Our names for the Settings screen (ShowSettings): its data.
  if (name.find("/XerengeSettings") != std::string::npos) {
    ctx.r3.u64 = 0x82036178;  // "../Burnout4/Menu/DriverDetails/DriverDetailsSettings"
  }
  __imp__sub_821FF408(ctx, base);
}

// The string table's lookup by key (sub_8210FCF8(table, key) -> UTF-16), which
// a text "$Key" in the interface comes to without its $: the keys of ours,
// with their text, big-endian UTF-16 in guest memory.
extern "C" void __imp__sub_8210FCF8(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_8210FCF8) {
  static const struct {
    const char* key;
    const char* text;
  } kTexts[] = {
      {"XerengeRestart", "Some settings take effect after a restart. Restart the game now?"},
      {"XerengeInfo", "Info"},  // the achievements' list: A opens one's popup
      {"XerengeQuit", "Quit the game?"},  // B on the main menu
      {"XerengeQuitHint", "Quit"},  // its hint, beside Continue
  };
  static uint32_t guest[std::size(kTexts)] = {};
  const char* key = reinterpret_cast<const char*>(xerenge::GuestPointer(base, ctx.r4.u32));
  // A localisation mod's text first (mods.cpp).
  if (ctx.r4.u32 >= 0x10000) {
    if (const uint32_t text = xerenge::mods::Text(base, key)) {
      ctx.r3.u64 = text;
      return;
    }
  }
  // An achievement's popup: its title and text, as they are now, each in a
  // buffer of its own, written again every time.
  if (ctx.r4.u32 >= 0x10000) {
    const bool title = std::strcmp(key, "XerengeAchievementTitle") == 0;
    if (title || std::strcmp(key, "XerengeAchievementText") == 0) {
      constexpr size_t kChars = 1024;
      static uint32_t buffers[2] = {};
      uint32_t& buffer = buffers[title ? 0 : 1];
      if (!buffer) {
        buffer = REX_KERNEL_MEMORY()->SystemHeapAlloc(uint32_t(kChars * 2), 0x10);
      }
      const std::string& text = title ? g_popup_title : g_popup_text;
      uint8_t* out = xerenge::GuestPointer(base, buffer);
      const size_t length = std::min(text.size(), kChars - 1);
      for (size_t c = 0; c <= length; ++c) {
        out[c * 2] = 0;
        out[c * 2 + 1] = c < length ? uint8_t(text[c]) : 0;
      }
      ctx.r3.u64 = buffer;
      return;
    }
  }
  // Song Manager's switch, its word (PLAYLIST) made this key (settings_movie.cpp): the
  // icon alone, the word's width kept (the movie places the icon by it).
  if (ctx.r4.u32 >= 0x10000 && std::strcmp(key, "XerengeBlankWord") == 0) {
    static uint32_t blank = 0;
    if (!blank) {
      blank = REX_KERNEL_MEMORY()->SystemHeapAlloc(18, 0x10);
      uint8_t* out = xerenge::GuestPointer(base, blank);
      for (int c = 0; c < 9; ++c) {
        out[c * 2] = 0;
        out[c * 2 + 1] = c < 8 ? ' ' : 0;
      }
    }
    ctx.r3.u64 = blank;
    return;
  }
  // Song Manager's subtitle while the music folder's songs are chosen.
  if (ctx.r4.u32 >= 0x10000 && std::strcmp(key, "FEDRIVERDETEATRAX") == 0 && xerenge::music::Custom()) {
    static uint32_t mine = 0;
    if (!mine) {
      const char* text = "[MY MUSIC]";
      const size_t length = std::strlen(text);
      mine = REX_KERNEL_MEMORY()->SystemHeapAlloc(uint32_t(length + 1) * 2, 0x10);
      uint8_t* out = xerenge::GuestPointer(base, mine);
      for (size_t c = 0; c <= length; ++c) {
        out[c * 2] = 0;
        out[c * 2 + 1] = uint8_t(text[c]);
      }
    }
    ctx.r3.u64 = mine;
    return;
  }
  for (size_t i = 0; ctx.r4.u32 >= 0x10000 && i < std::size(kTexts); ++i) {
    if (std::strcmp(key, kTexts[i].key) == 0) {
      if (!guest[i]) {
        const size_t length = std::strlen(kTexts[i].text);
        guest[i] = REX_KERNEL_MEMORY()->SystemHeapAlloc(uint32_t(length + 1) * 2, 0x10);
        uint8_t* out = xerenge::GuestPointer(base, guest[i]);
        for (size_t c = 0; c <= length; ++c) {
          out[c * 2] = 0;
          out[c * 2 + 1] = uint8_t(kTexts[i].text[c]);
        }
      }
      ctx.r3.u64 = guest[i];
      return;
    }
  }
  __imp__sub_8210FCF8(ctx, base);
}

// The interface taking a popup's text and buttons.
extern "C" void __imp__sub_821DF3A8(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_821DF3A8) {
  REXLOG_INFO("menu: popup parameters taken");
  __imp__sub_821DF3A8(ctx, base);
  REXLOG_INFO("menu: popup parameters made");
}

// CB4AptManager::Render: each frame, before the interface's code runs. A popup
// asked for from inside that code (a native, as SelectedOption is) runs the
// interpreter again within itself and stops the game; from here it does not.
extern "C" void __imp__sub_821FF308(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_821FF308) {
  CheckQuitButton(ctx, base);
  if (g_popup_pending) {
    g_popup_pending = false;
    ShowRestartPopup(ctx, base);
  }
  if (g_achievement_popup >= 0) {
    const int index = g_achievement_popup;
    g_achievement_popup = -1;
    ShowAchievementPopup(ctx, base, index);
  }
  // A menu is shown: a video behind it is its background (widescreen.cpp).
  xerenge::NoteMenuDrawn();
  __imp__sub_821FF308(ctx, base);
}
