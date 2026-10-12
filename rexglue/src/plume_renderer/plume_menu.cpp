/**
 * @file        plume_renderer/plume_menu.cpp
 * @brief       The in-game settings menu (F1, or Back and Start on a pad).
 */
#include "plume_renderer/plume_menu.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>

#include <imgui.h>

#include <rex/logging.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>
#include <rex/ui/windowed_app_context.h>
#include <rex/system/gpu_plugin.h>

#include "plume_renderer/plume_menu_conf.h"
#include "plume_renderer/plume_perf.h"
#include "plume_renderer/plume_postfx.h"
#include "plume_renderer/plume_settings.h"

namespace rex::plume_renderer {

namespace {

using rex::ui::VirtualKey;

// XINPUT_GAMEPAD_*.
constexpr uint16_t kPadUp = 0x0001;
constexpr uint16_t kPadDown = 0x0002;
constexpr uint16_t kPadLeft = 0x0004;
constexpr uint16_t kPadRight = 0x0008;
constexpr uint16_t kPadStart = 0x0010;
constexpr uint16_t kPadBack = 0x0020;
constexpr uint16_t kPadLeftShoulder = 0x0100;
constexpr uint16_t kPadRightShoulder = 0x0200;
constexpr uint16_t kPadA = 0x1000;
constexpr uint16_t kPadB = 0x2000;
constexpr uint16_t kPadX = 0x4000;

// A stick pushed this far counts as the d-pad.
constexpr int kStickThreshold = 16000;
// Back or Start, pressed, is held back from the title this long: the other one
// pressed within it makes the chord that opens the menu, and the title sees
// neither; otherwise the title is given the button late. Pressed later, while
// the first is still held, the second still opens the menu and is kept from the
// title, which has only seen the first.
constexpr auto kChordWindow = std::chrono::milliseconds(100);
// A press let go within that time is shown to the title for this long after.
constexpr auto kLatePress = std::chrono::milliseconds(50);
// A pad the title has not read for this long is taken to have been let go: the
// title reads it every frame, so it is only stale when the title is not
// running (a load, a video).
constexpr auto kPadStale = std::chrono::milliseconds(300);
// Held directions repeat after a pause, then quickly.
constexpr float kRepeatDelay = 0.4f;
constexpr float kRepeatInterval = 0.07f;
constexpr size_t kMaxTextLength = 48;

// The launcher's names for true and false.
const char* BoolText(bool on) {
  return on ? "true" : "false";
}

std::string FormatNumber(float value) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.3f", value);
  std::string result = text;
  while (result.size() > 1 && result.back() == '0') {
    result.pop_back();
  }
  if (result.back() == '.') {
    result.pop_back();
  }
  return result;
}

void AppendUtf8(std::string* text, char32_t c) {
  if (c < 0x80) {
    *text += char(c);
  } else if (c < 0x800) {
    *text += char(0xC0 | (c >> 6));
    *text += char(0x80 | (c & 0x3F));
  } else if (c < 0x10000) {
    *text += char(0xE0 | (c >> 12));
    *text += char(0x80 | ((c >> 6) & 0x3F));
    *text += char(0x80 | (c & 0x3F));
  } else {
    *text += char(0xF0 | (c >> 18));
    *text += char(0x80 | ((c >> 12) & 0x3F));
    *text += char(0x80 | ((c >> 6) & 0x3F));
    *text += char(0x80 | (c & 0x3F));
  }
}

void PopUtf8(std::string* text) {
  while (!text->empty() && (static_cast<unsigned char>(text->back()) & 0xC0) == 0x80) {
    text->pop_back();
  }
  if (!text->empty()) {
    text->pop_back();
  }
}

// The keyboard, as the window delivers it. Takes what is typed while the menu
// is open - the title's own keyboard handling (the SDK's, which turns keys
// into pad buttons) is below this in the order and never hears of it - and
// F1, always.
class KeyboardListener : public rex::ui::WindowInputListener {
 public:
  void OnKeyDown(rex::ui::KeyEvent& e) override {
    PlumeMenu& menu = PlumeMenu::Get();
    const VirtualKey key = e.virtual_key();
    if (key == VirtualKey::kF1) {
      if (!e.prev_state()) {
        menu.Toggle();
      }
      e.set_handled(true);
      return;
    }
    if (!menu.Visible()) {
      return;
    }
    // Remembered, so that the key's release is taken as well - and only that
    // one: a key let go that was pressed before the menu opened is the
    // title's to hear about.
    taken_.insert(key);
    menu.PushKey(uint16_t(key));
    e.set_handled(true);
  }

  void OnKeyUp(rex::ui::KeyEvent& e) override {
    if (e.virtual_key() == VirtualKey::kF1 || taken_.erase(e.virtual_key()) != 0) {
      e.set_handled(true);
    }
  }

  void OnKeyChar(rex::ui::KeyEvent& e) override {
    if (PlumeMenu::Get().Visible()) {
      PlumeMenu::Get().PushCharacter(char32_t(uint32_t(e.virtual_key())));
      e.set_handled(true);
    }
  }

  // The mouse points at the menu; while it is open it is not the title's, which
  // would steer by it.
  void OnMouseDown(rex::ui::MouseEvent& e) override {
    if (PlumeMenu::Get().Visible()) {
      buttons_taken_.insert(e.button());
      if (e.button() == rex::ui::MouseEvent::Button::kLeft) {
        left_down_ = true;
        Point(PlumeMenu::PointerEvent::kDown, e);
      }
      e.set_handled(true);
    }
  }
  void OnMouseUp(rex::ui::MouseEvent& e) override {
    if (e.button() == rex::ui::MouseEvent::Button::kLeft && left_down_) {
      left_down_ = false;
      Point(PlumeMenu::PointerEvent::kUp, e);
    }
    if (buttons_taken_.erase(e.button()) != 0) {
      e.set_handled(true);
    }
  }
  void OnMouseMove(rex::ui::MouseEvent& e) override {
    if (PlumeMenu::Get().Visible()) {
      if (left_down_) {
        Point(PlumeMenu::PointerEvent::kMove, e);
      }
      e.set_handled(true);
    }
  }
  void OnMouseWheel(rex::ui::MouseEvent& e) override {
    if (PlumeMenu::Get().Visible()) {
      PlumeMenu::Get().PushScroll(-float(e.scroll_y()) / float(rex::ui::MouseEvent::kScrollPerDetent) * 2.0f);
      e.set_handled(true);
    }
  }

 private:
  // The window's own size, in the physical pixels the mouse is given in.
  static void Point(PlumeMenu::PointerEvent kind, const rex::ui::MouseEvent& e) {
    const rex::ui::Window* window = e.target();
    if (window && window->GetActualPhysicalWidth() != 0 && window->GetActualPhysicalHeight() != 0) {
      PlumeMenu::Get().PushPointer(kind, float(e.x()) / float(window->GetActualPhysicalWidth()),
                                   float(e.y()) / float(window->GetActualPhysicalHeight()));
    }
  }

  bool left_down_ = false;
  std::set<VirtualKey> taken_;
  std::set<rex::ui::MouseEvent::Button> buttons_taken_;
};

KeyboardListener& Listener() {
  static KeyboardListener listener;
  return listener;
}

rex::ui::Window* g_window = nullptr;

}  // namespace

// What the menu is made of, and the rendering thread's view of it.
struct PlumeMenu::State {
  enum class Kind { Toggle, Choice, Slider, Text, Key };
  struct Choice {
    std::string value;
    std::string label;
  };
  struct Item {
    std::string label;
    std::string key;  // its line in xerenge.conf
    Kind kind = Kind::Toggle;
    std::string help;
    std::string default_value;
    std::vector<Choice> choices;
    float min = 0.0f;
    float max = 1.0f;
    float step = 0.05f;
    // Set for a setting that applies at once: where it is now, and how to make
    // it so. Without them it is saved and applies after a restart.
    std::function<std::string()> read;
    std::function<void(const std::string&)> apply;
    std::string value;

    bool live() const { return bool(apply); }
  };
  struct Page {
    std::string name;
    std::vector<Item> items;
    int selected = 0;
    int scroll = 0;
  };
  enum class Mode { Browse, EditText, CaptureKey };
  enum class Action { Up, Down, Left, Right, Accept, Back, PreviousPage, NextPage, Reset };

  explicit State(PlumeMenu& owner) : menu(owner) {
    conf_path = menu_conf::Path();
    if (!conf_path.empty()) {
      conf = menu_conf::Read(conf_path);
    }
    BuildPages();
    if (const char* page = std::getenv("XERENGE_MENU_PAGE")) {
      current = std::clamp(std::atoi(page), 0, int(pages.size()) - 1);
    }
  }

  // ---------------------------------------------------------------- the pages

  static Item Toggle(const char* label, const char* key, bool fallback, const char* help) {
    Item item;
    item.label = label;
    item.key = key;
    item.kind = Kind::Toggle;
    item.default_value = BoolText(fallback);
    item.help = help;
    return item;
  }
  static Item Choose(const char* label, const char* key, const char* fallback,
                     std::vector<Choice> choices, const char* help) {
    Item item;
    item.label = label;
    item.key = key;
    item.kind = Kind::Choice;
    item.default_value = fallback;
    item.choices = std::move(choices);
    item.help = help;
    return item;
  }
  static Item Slide(const char* label, const char* key, float fallback, float low, float high,
                    float step, const char* help) {
    Item item;
    item.label = label;
    item.key = key;
    item.kind = Kind::Slider;
    item.default_value = FormatNumber(fallback);
    item.min = low;
    item.max = high;
    item.step = step;
    item.help = help;
    return item;
  }
  static Item Type(const char* label, const char* key, const char* help) {
    Item item;
    item.label = label;
    item.key = key;
    item.kind = Kind::Text;
    item.help = help;
    return item;
  }
  static Item Bind(const char* label, const char* key, const char* fallback) {
    Item item;
    item.label = label;
    item.key = key;
    item.kind = Kind::Key;
    item.default_value = fallback;
    item.help = "The key that stands in for this control. Several keys, comma separated, can "
                "be written into xerenge.conf by hand.";
    return item;
  }
  static Item Live(Item item, std::function<std::string()> read,
                   std::function<void(const std::string&)> apply) {
    item.read = std::move(read);
    item.apply = std::move(apply);
    return item;
  }

  void BuildPages() {
    using namespace settings;
    Page display{"Display"};
#ifndef __ANDROID__
    display.items.push_back(Live(
        Toggle("Windowed", "windowed", false, "Run in a window instead of full screen."),
        [] { return std::string(BoolText(g_window && !g_window->IsFullscreen())); },
        [](const std::string& v) {
          // The window system is the window thread's.
          rex::ui::Window* window = g_window;
          const bool windowed = v == "true";
          if (window) {
            window->app_context().CallInUIThread(
                [window, windowed] { window->SetFullscreen(!windowed); });
          }
        }));
#endif
    display.items.push_back(Toggle("Keep 16:9", "aspect_16_9", true,
                                   "The console's 16:9 with black bars on a wider screen. Off, "
                                   "the game widens to fill the screen."));
    display.items.push_back(Choose("Render resolution", "render_resolution", "",
                                   {{"", "The window's"},
                                    {"1600x900", "1600x900"},
                                    {"1280x720", "1280x720"},
                                    {"960x540", "960x540"},
                                    {"854x480", "854x480"},
                                    {"640x360", "640x360"}},
                                   "The size the frame is drawn at, scaled onto the window. "
                                   "Lower is faster."));
    display.items.push_back(Live(
        Toggle("FPS counter", "fps_counter", false, "Frames per second in the top left corner."),
        [] { return std::string(BoolText(FpsShown())); },
        [](const std::string& v) { SetFpsShown(v == "true"); }));
    display.items.push_back(Toggle("Frame generation", "frame_generation", false,
                                   "Frames drawn between the game's own, for a screen faster "
                                   "than 60 Hz."));
    display.items.push_back(Live(
        Toggle("Crash mode smoothing", "crash_interpolation", true,
               "A frame drawn between each two of Crash mode's thirty, for sixty."),
        [] { return std::string(BoolText(CrashInterpolation())); },
        [](const std::string& v) { SetCrashInterpolation(v == "true"); }));
    display.items.push_back(Toggle("30 fps lock", "fps_30", false,
                                   "Thirty frames a second with the world still at sixty "
                                   "steps, for a machine that cannot hold sixty."));
    display.items.push_back(Choose("Language", "language", "0",
                                   {{"", "Ask at every start"},
                                    {"0", "English"},
                                    {"1", "English (US)"},
                                    {"5", "Espa\u00f1ol"},
                                    {"10", "Nederlands"},
                                    {"11", "Svenska"},
                                    {"12", "Suomi"}},
                                   "The language the game starts in."));
    pages.push_back(std::move(display));

    Page graphics{"Graphics"};
    // A cap on what the game asks for: 4 on a phone unless said, 16 elsewhere.
#ifdef __ANDROID__
    graphics.items.push_back(Live(
        Choose("Anisotropic filtering, at most", "anisotropy", "4",
               {{"off", "Off"}, {"4", "4x"}, {"8", "8x"}},
               "Keeps textures sharp at a slant, the road above all. A phone's GPU pays for "
               "every pixel of it."),
#else
    graphics.items.push_back(Live(
        Choose("Anisotropic filtering, at most", "anisotropy", "16",
               {{"off", "Off"}, {"2", "2x"}, {"4", "4x"}, {"8", "8x"}, {"16", "16x"}},
               "Keeps textures sharp at a slant, the road above all. A cap on what the game "
               "asks for."),
#endif
        [] {
          const uint32_t level = MaxAnisotropy();
          return level <= 1 ? std::string("off") : std::to_string(level);
        },
        [](const std::string& v) { SetMaxAnisotropy(v == "off" ? 1 : uint32_t(std::atoi(v.c_str()))); }));
    pages.push_back(std::move(graphics));

    Page post{"Post-processing"};
    post.items.push_back(
        Toggle("Bloom", "bloom", false, "The glow around the sky and the lights."));
    post.items.push_back(
        Toggle("Motion blur", "motion_blur", false, "Blur at speed, and the radial blur."));
    post.items.push_back(Live(
        Slide("Colour grading", "colour_grade", 1.0f, 0.0f, 1.0f, 0.05f,
              "How much of the game's colour grading, its yellow cast, is kept. 1 is the console's "
              "look, 0 none."),
        [] { return FormatNumber(ColourGrade()); },
        [](const std::string& v) { SetColourGrade(std::strtof(v.c_str(), nullptr)); }));
    post.items.push_back(Live(
        Slide("Brightness", "brightness", 1.0f, 0.7f, 1.6f, 0.05f,
              "Lightens or darkens the 3D picture. 1 leaves it as the console drew it."),
        [] { return FormatNumber(Brightness()); },
        [](const std::string& v) { SetBrightness(std::strtof(v.c_str(), nullptr)); }));
    post.items.push_back(Live(
        Slide("Shadow lift", "shadow_lift", 0.0f, 0.0f, 0.3f, 0.01f,
              "Raises the darkest colours without brightening the rest. For cars that are "
              "nearly black, as on the console."),
        [] { return FormatNumber(ShadowLift()); },
        [](const std::string& v) { SetShadowLift(std::strtof(v.c_str(), nullptr)); }));
    post.items.push_back(Live(
        Choose("Anti-aliasing", "aa", "off", {{"off", "Off"}, {"fxaa", "FXAA"}},
               "Smooths the jagged edges of the finished frame. The interface is drawn into the "
               "same picture and softens a little with it."),
        [] { return std::string(GetAntiAliasing() == AntiAliasing::kFxaa ? "fxaa" : "off"); },
        [](const std::string& v) {
          SetAntiAliasing(v == "fxaa" ? AntiAliasing::kFxaa : AntiAliasing::kOff);
        }));
    post.items.push_back(Live(
        Slide("Sharpening", "sharpen", 0.0f, 0.0f, 1.0f, 0.05f,
              "AMD's contrast adaptive sharpening over the finished frame. 0 is off; it brings "
              "back what anti-aliasing softens."),
        [] { return FormatNumber(GetSharpening()); },
        [](const std::string& v) { SetSharpening(std::strtof(v.c_str(), nullptr)); }));
    pages.push_back(std::move(post));

    Page online{"Online"};
    online.items.push_back(Type("Gamertag", "gamertag",
                                "The name shown to others online. Empty: Player."));
    online.items.push_back(Toggle("Local multiplayer", "online", false,
                                  "The copies on one network find each other's games."));
    online.items.push_back(Type("Lobby server", "lobby_server",
                                "A server of your own, host or IP. Set, it is the only lobby "
                                "and local multiplayer is off."));
    pages.push_back(std::move(online));

#ifndef __ANDROID__
    // A phone has its on-screen controls instead.
    Page controls{"Controls"};
    controls.items.push_back(Toggle("Keyboard as a pad", "keyboard", false,
                                    "Play with the keyboard, using the keys below."));
    struct PadControl {
      const char* key;
      const char* label;
      const char* fallback;
    };
    static constexpr PadControl kControls[] = {
        {"keybind_a", "A", "Space"},
        {"keybind_b", "B", "Backspace"},
        {"keybind_x", "X", "E"},
        {"keybind_y", "Y", "R"},
        {"keybind_left_shoulder", "Left bumper (LB)", "Q"},
        {"keybind_right_shoulder", "Right bumper (RB)", "F"},
        {"keybind_left_trigger", "Left trigger (LT), brake", "S"},
        {"keybind_right_trigger", "Right trigger (RT), accelerate", "W"},
        {"keybind_lstick_up", "Left stick up", ""},
        {"keybind_lstick_down", "Left stick down", ""},
        {"keybind_lstick_left", "Left stick left, steer", "A"},
        {"keybind_lstick_right", "Left stick right, steer", "D"},
        {"keybind_lstick_press", "Left stick press", "C"},
        {"keybind_rstick_up", "Right stick up", "I"},
        {"keybind_rstick_down", "Right stick down", "K"},
        {"keybind_rstick_left", "Right stick left", "J"},
        {"keybind_rstick_right", "Right stick right", "L"},
        {"keybind_rstick_press", "Right stick press", "V"},
        {"keybind_dpad_up", "D-pad up", "Up"},
        {"keybind_dpad_down", "D-pad down", "Down"},
        {"keybind_dpad_left", "D-pad left", "Left"},
        {"keybind_dpad_right", "D-pad right", "Right"},
        {"keybind_start", "Start", "Enter"},
        {"keybind_back", "Back", "Escape"},
        {"keybind_guide", "Guide", ""},
    };
    for (const PadControl& control : kControls) {
      controls.items.push_back(Bind(control.label, control.key, control.fallback));
    }
    pages.push_back(std::move(controls));
#endif

    Page advanced{"Advanced"};
#ifndef __ANDROID__
    advanced.items.push_back(Choose("Renderer", "renderer", "plume",
                                    {{"plume", "plume"}, {"xenos", "Xenia's (xenos)"}},
                                    "plume is this project's renderer. This menu is only here "
                                    "with it."));
#endif
    advanced.items.push_back(Live(
        Toggle("Performance overlay", "perf_overlay", false,
               "FPS and frame-time graph in the corner."),
        [] { return std::string(BoolText(PerfOverlay())); },
        [](const std::string& v) { SetPerfOverlay(v == "true"); }));
    advanced.items.push_back(Toggle("Present asynchronously", "async_present", false,
                                    "Hand a frame to the GPU and go on at once. It once showed "
                                    "black frames now and then."));
    advanced.items.push_back(Toggle("Early submit", "early_submit", true,
                                    "Hand the frame to the GPU before a screen image is asked "
                                    "for."));
    advanced.items.push_back(Toggle("Packed vertices", "packed_vertices", true,
                                    "Send only the vertex data a draw's shader reads."));
    advanced.items.push_back(Toggle("Split reflections", "reflection_split", false,
                                    "Draw the cars' reflections in a pass of their own."));
    pages.push_back(std::move(advanced));

    Page debug{"Debug"};
    debug.items.push_back(Toggle("Debug mode", "debug", false,
                                 "Detailed logs for reporting a problem. The rest of this page "
                                 "decides what is logged."));
    debug.items.push_back(Choose("Log level", "debug_log_level", "debug",
                                 {{"trace", "trace"},
                                  {"debug", "debug"},
                                  {"info", "info"},
                                  {"warn", "warn"},
                                  {"error", "error"}},
                                 "How much is logged."));
    debug.items.push_back(Toggle("GPU trace", "debug_gpu_trace", true,
                                 "The renderer's per-draw and per-frame diagnostics. Large."));
    debug.items.push_back(
        Toggle("Movie trace", "debug_movie_trace", false, "The video player's states."));
    debug.items.push_back(
        Toggle("Pipeline log", "debug_pipeline_log", false, "Every render pipeline built."));
    debug.items.push_back(Toggle("Vertex trace", "debug_vertex_trace", true,
                                 "Vertex attributes that come out as NaN or absurd, and who "
                                 "writes them."));
    debug.items.push_back(Toggle("Constant trace", "debug_constant_trace", false,
                                 "Shader constants that are NaN or absurd, and who writes "
                                 "them."));
    debug.items.push_back(Choose("Menu video on", "debug_video_gpu", "",
                                 {{"", "The default (GPU)"}, {"0", "CPU"}, {"3", "GPU"}},
                                 "Where the menu videos are turned into RGB."));
    debug.items.push_back(Toggle("Noisy log", "debug_noisy", false,
                                 "The per-frame log lines as well. Very large."));
    pages.push_back(std::move(debug));

    for (Page& page : pages) {
      for (Item& item : page.items) {
        if (item.live()) {
          item.value = item.read();
        } else if (auto it = conf.find(item.key); it != conf.end()) {
          item.value = it->second;
        } else {
          item.value = item.default_value;
        }
      }
    }
  }

  // ------------------------------------------------------------------ editing

  bool Saves() const { return !conf_path.empty(); }

  Item* Selected() {
    Page& page = pages[size_t(current)];
    return page.items.empty() ? nullptr : &page.items[size_t(page.selected)];
  }

  // Whether the item can be changed here: one that applies after a restart is
  // saved into xerenge.conf, so without one there is nowhere to put it.
  bool Editable(const Item& item) const { return item.live() || Saves(); }

  void Notice(const std::string& text) {
    notice = text;
    notice_time = 3.0f;
  }

  // save: written to xerenge.conf as well; not while a slider is still being
  // dragged, when every step of it would be.
  void Commit(Item& item, const std::string& value, bool save = true) {
    if (!Editable(item)) {
      Notice("Not saved: the game was not started from its launcher (no xerenge.conf).");
      return;
    }
    item.value = value;
    if (item.live()) {
      item.apply(value);
    } else {
      restart_needed = true;
    }
    if (save && Saves() && !menu_conf::Write(conf_path, item.key, value)) {
      Notice("Could not write " + conf_path);
    }
  }

  void Change(Item& item, int direction) {
    switch (item.kind) {
      case Kind::Toggle:
        Commit(item, item.value == "true" ? "false" : "true");
        break;
      case Kind::Choice: {
        if (item.choices.empty()) {
          break;
        }
        int index = 0;
        for (size_t i = 0; i < item.choices.size(); ++i) {
          if (item.choices[i].value == item.value) {
            index = int(i);
          }
        }
        const int count = int(item.choices.size());
        index = ((index + direction) % count + count) % count;
        Commit(item, item.choices[size_t(index)].value);
        break;
      }
      case Kind::Slider: {
        const float now = std::strtof(item.value.c_str(), nullptr);
        const float steps = std::round((now - item.min) / item.step) + float(direction);
        const float next = std::clamp(item.min + steps * item.step, item.min, item.max);
        Commit(item, FormatNumber(next));
        break;
      }
      case Kind::Text:
      case Kind::Key:
        break;
    }
  }

  void BeginEditing(Item& item) {
    if (!Editable(item)) {
      Notice("Not saved: the game was not started from its launcher (no xerenge.conf).");
      return;
    }
    edit_backup = item.value;
    mode = item.kind == Kind::Text ? Mode::EditText : Mode::CaptureKey;
    if (mode == Mode::EditText) {
      SetTextInput(true);
    }
  }

  void StopEditing(bool keep) {
    Item* item = Selected();
    if (item && mode != Mode::Browse) {
      if (keep) {
        Commit(*item, item->value);
      } else {
        item->value = edit_backup;
      }
    }
    if (mode == Mode::EditText) {
      SetTextInput(false);
    }
    mode = Mode::Browse;
  }

  void SetTextInput(bool active) {
    if (!g_window) {
      return;
    }
    rex::ui::Window* window = g_window;
    window->app_context().CallInUIThread([window, active] { window->SetTextInputActive(active); });
  }

  void Select(int delta) {
    Page& page = pages[size_t(current)];
    const int count = int(page.items.size());
    if (count == 0) {
      return;
    }
    page.selected = ((page.selected + delta) % count + count) % count;
  }

  void ChangePage(int delta) {
    const int count = int(pages.size());
    current = ((current + delta) % count + count) % count;
  }

  void Act(Action action) {
    follow_selection = true;
    Item* item = Selected();
    switch (action) {
      case Action::Up:
        Select(-1);
        break;
      case Action::Down:
        Select(1);
        break;
      case Action::Left:
        if (item) {
          Change(*item, -1);
        }
        break;
      case Action::Right:
        if (item) {
          Change(*item, 1);
        }
        break;
      case Action::Accept:
        if (!item) {
          break;
        }
        if (item->kind == Kind::Text || item->kind == Kind::Key) {
          BeginEditing(*item);
        } else {
          Change(*item, 1);
        }
        break;
      case Action::Reset:
        if (item) {
          Commit(*item, item->default_value);
        }
        break;
      case Action::PreviousPage:
        ChangePage(-1);
        break;
      case Action::NextPage:
        ChangePage(1);
        break;
      case Action::Back:
        menu.Close(from_pad);
        break;
    }
  }

  void HandleKey(uint16_t key) {
    const VirtualKey vk = VirtualKey(key);
    from_pad = false;
    if (mode == Mode::CaptureKey) {
      Item* item = Selected();
      if (vk == VirtualKey::kEscape) {
        StopEditing(false);
      } else if (item && (vk == VirtualKey::kDelete || vk == VirtualKey::kBack)) {
        item->value.clear();
        StopEditing(true);
      } else if (item) {
        const std::string name = rex::ui::VirtualKeyToString(vk);
        if (!name.empty()) {
          item->value = name;
          StopEditing(true);
        }
      }
      return;
    }
    if (mode == Mode::EditText) {
      Item* item = Selected();
      if (vk == VirtualKey::kReturn) {
        StopEditing(true);
      } else if (vk == VirtualKey::kEscape) {
        StopEditing(false);
      } else if (vk == VirtualKey::kBack && item) {
        PopUtf8(&item->value);
      }
      return;
    }
    switch (vk) {
      case VirtualKey::kUp:
        Act(Action::Up);
        break;
      case VirtualKey::kDown:
        Act(Action::Down);
        break;
      case VirtualKey::kLeft:
        Act(Action::Left);
        break;
      case VirtualKey::kRight:
        Act(Action::Right);
        break;
      case VirtualKey::kReturn:
        Act(Action::Accept);
        break;
      case VirtualKey::kEscape:
        Act(Action::Back);
        break;
      case VirtualKey::kPrior:
        Act(Action::PreviousPage);
        break;
      case VirtualKey::kNext:
      case VirtualKey::kTab:
        Act(Action::NextPage);
        break;
      case VirtualKey::kDelete:
        Act(Action::Reset);
        break;
      default:
        break;
    }
  }

  // ------------------------------------------------------------------ pointer

  // Where things were drawn last frame, for the pointer to find them.
  struct Hit {
    enum class Kind { Close, Tab, Row, Bar } kind;
    int index;
    float left, top, right, bottom;
  };

  const Hit* HitAt(float x, float y, Hit::Kind kind) const {
    for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
      if (it->kind == kind && x >= it->left && x < it->right && y >= it->top && y < it->bottom) {
        return &*it;
      }
    }
    return nullptr;
  }

  // A slider set from where along its bar the pointer is.
  void SetFromBar(Item& item, const Hit& bar, float x, bool save) {
    // The hit is the bar with room for a finger on either side.
    const float left = bar.left + 12 * scale;
    const float right = bar.right - 12 * scale;
    const float fraction = std::clamp((x - left) / (right - left), 0.0f, 1.0f);
    const float steps = std::round(fraction * (item.max - item.min) / item.step);
    Commit(item, FormatNumber(std::clamp(item.min + steps * item.step, item.min, item.max)), save);
  }

  void Tap(float x, float y) {
    if (HitAt(x, y, Hit::Kind::Close)) {
      menu.Close(false);
      return;
    }
    if (const Hit* tab = HitAt(x, y, Hit::Kind::Tab)) {
      current = tab->index;
      return;
    }
    const Hit* row = HitAt(x, y, Hit::Kind::Row);
    if (!row) {
      return;
    }
    Page& page = pages[size_t(current)];
    page.selected = row->index;
    Item& item = page.items[size_t(row->index)];
    switch (item.kind) {
      case Kind::Toggle:
        Change(item, 1);
        break;
      case Kind::Choice:
        // The right of the row steps on, the left of what it shows steps back.
        Change(item, x < row->right - 180 * scale ? -1 : 1);
        break;
      case Kind::Slider:
        if (const Hit* bar = HitAt(x, y, Hit::Kind::Bar)) {
          SetFromBar(item, *bar, x, true);
        }
        break;
      case Kind::Text:
      case Kind::Key:
        BeginEditing(item);
        break;
    }
  }

  void HandlePointer(const std::vector<PlumeMenu::Pointer>& events, float rows) {
    Page& page = pages[size_t(current)];
    if (rows != 0.0f) {
      page.scroll += int(std::lround(rows));
    }
    for (const PlumeMenu::Pointer& event : events) {
      const float x = event.x * float(width);
      const float y = event.y * float(height);
      switch (event.event) {
        case PlumeMenu::PointerEvent::kDown: {
          touching = true;
          dragged = false;
          touch_x = x;
          touch_y = y;
          touch_scroll = page.scroll;
          bar_item = -1;
          if (mode != Mode::Browse) {
            StopEditing(true);
          }
          if (const Hit* bar = HitAt(x, y, Hit::Kind::Bar)) {
            // A slider is dragged along, not scrolled past.
            bar_item = bar->index;
            page.selected = bar->index;
            SetFromBar(page.items[size_t(bar_item)], *bar, x, false);
          }
          break;
        }
        case PlumeMenu::PointerEvent::kMove: {
          if (!touching) {
            break;
          }
          if (bar_item >= 0) {
            if (const Hit* bar = HitAt(touch_x, touch_y, Hit::Kind::Bar)) {
              SetFromBar(page.items[size_t(bar_item)], *bar, x, false);
            }
            break;
          }
          if (std::fabs(y - touch_y) > 14 * scale) {
            dragged = true;
          }
          if (dragged && row_height > 0.0f) {
            // The list follows the finger.
            page.scroll = touch_scroll - int(std::lround((y - touch_y) / row_height));
          }
          break;
        }
        case PlumeMenu::PointerEvent::kUp: {
          if (!touching) {
            break;
          }
          touching = false;
          if (bar_item >= 0) {
            Commit(page.items[size_t(bar_item)], page.items[size_t(bar_item)].value, true);
            bar_item = -1;
          } else if (!dragged) {
            Tap(touch_x, touch_y);
          }
          break;
        }
      }
    }
  }

  // Held directions of the pad, with their own repeat.
  struct Direction {
    bool down = false;
    float next = 0.0f;
  };

  void HandlePad(const xerenge::MenuPadState& pad, float seconds) {
    from_pad = true;
    const uint16_t pressed = pad.buttons & ~previous_buttons;
    previous_buttons = pad.buttons;
    if (mode == Mode::EditText || mode == Mode::CaptureKey) {
      // Typing and key capture belong to the keyboard; B gives up.
      if (pressed & kPadB) {
        StopEditing(false);
      }
      return;
    }
    if (pressed & kPadA) {
      Act(Action::Accept);
    }
    if (pressed & kPadB) {
      Act(Action::Back);
    }
    if (pressed & kPadX) {
      Act(Action::Reset);
    }
    if (pressed & kPadLeftShoulder) {
      Act(Action::PreviousPage);
    }
    if (pressed & kPadRightShoulder) {
      Act(Action::NextPage);
    }
    const bool wanted[4] = {
        (pad.buttons & kPadUp) != 0 || pad.thumb_ly > kStickThreshold,
        (pad.buttons & kPadDown) != 0 || pad.thumb_ly < -kStickThreshold,
        (pad.buttons & kPadLeft) != 0 || pad.thumb_lx < -kStickThreshold,
        (pad.buttons & kPadRight) != 0 || pad.thumb_lx > kStickThreshold,
    };
    const Action actions[4] = {Action::Up, Action::Down, Action::Left, Action::Right};
    for (int i = 0; i < 4; ++i) {
      Direction& direction = directions[i];
      if (!wanted[i]) {
        direction.down = false;
        continue;
      }
      if (!direction.down) {
        direction.down = true;
        direction.next = kRepeatDelay;
        Act(actions[i]);
      } else if ((direction.next -= seconds) <= 0.0f) {
        direction.next += kRepeatInterval;
        Act(actions[i]);
      }
    }
  }

  void Opened() {
    open = true;
    open_time = 0.0f;
    mode = Mode::Browse;
    previous_buttons = 0xFFFF;  // what is held now does not press anything
    for (Direction& direction : directions) {
      direction = Direction{};
    }
  }

  void Closed() {
    touching = false;
    if (mode != Mode::Browse) {
      StopEditing(false);
    }
    open = false;
  }

  void Update(float seconds) {
    std::vector<uint16_t> keys;
    std::vector<char32_t> characters;
    xerenge::MenuPadState pad;
    std::vector<PlumeMenu::Pointer> pointers;
    float scroll = 0.0f;
    {
      std::lock_guard lock(menu.input_mutex_);
      keys.swap(menu.pending_keys_);
      characters.swap(menu.pending_characters_);
      pointers.swap(menu.pending_pointers_);
      scroll = std::exchange(menu.pending_scroll_, 0.0f);
      pad = menu.pad_;
      if (std::chrono::steady_clock::now() - menu.pad_time_ > kPadStale) {
        pad = xerenge::MenuPadState{};
      }
    }
    open_time += seconds;
    notice_time -= seconds;
    for (const char32_t character : characters) {
      Item* item = Selected();
      if (mode == Mode::EditText && item && character >= 32 && character != 127 &&
          item->value.size() < kMaxTextLength) {
        AppendUtf8(&item->value, character);
      }
    }
    for (const uint16_t key : keys) {
      if (!menu.Visible()) {
        break;
      }
      HandleKey(key);
    }
    if (menu.Visible()) {
      HandlePad(pad, seconds);
      HandlePointer(pointers, scroll);
    }
  }

  // ------------------------------------------------------------------ drawing

  ImU32 Colour(int r, int g, int b, float a) const {
    return IM_COL32(r, g, b, int(std::clamp(a * fade, 0.0f, 1.0f) * 255.0f));
  }

  float TextWidth(const std::string& text, float size) const {
    return ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x;
  }

  void Text(ImDrawList* list, float x, float y, float size, ImU32 colour, const std::string& text) {
    list->AddText(ImGui::GetFont(), size, ImVec2(x, y), colour, text.c_str());
  }

  void TextRight(ImDrawList* list, float right, float y, float size, ImU32 colour,
                 const std::string& text) {
    Text(list, right - TextWidth(text, size), y, size, colour, text);
  }

  std::string ValueText(const Item& item) const {
    switch (item.kind) {
      case Kind::Toggle:
        return item.value == "true" ? "On" : "Off";
      case Kind::Choice:
        for (const Choice& choice : item.choices) {
          if (choice.value == item.value) {
            return choice.label;
          }
        }
        return item.value;
      case Kind::Slider:
        return item.value;
      case Kind::Text:
        return item.value;
      case Kind::Key:
        return item.value;
    }
    return {};
  }

  void DrawRow(ImDrawList* list, const Item& item, int index, bool selected, float left,
               float right, float top, float row, float s) {
    hits.push_back({Hit::Kind::Row, index, left - 14 * s, top, right + 14 * s, top + row});
    const bool editable = Editable(item);
    const float alpha = editable ? 1.0f : 0.45f;
    if (selected) {
      list->AddRectFilled(ImVec2(left - 14 * s, top), ImVec2(right + 14 * s, top + row),
                          Colour(250, 190, 40, 0.16f), 5 * s);
      list->AddRectFilled(ImVec2(left - 14 * s, top + 5 * s), ImVec2(left - 10 * s, top + row - 5 * s),
                          Colour(250, 190, 40, 1.0f), 2 * s);
    }
    const float size = 20 * s;
    const float text_y = top + (row - size) / 2;
    Text(list, left, text_y, size, Colour(240, 240, 240, alpha), item.label);
    if (!item.live()) {
      Text(list, left + TextWidth(item.label, size) + 12 * s, text_y + 3 * s, 14 * s,
           Colour(250, 190, 40, 0.75f * alpha), "restart");
    }

    const bool editing_this = selected && mode != Mode::Browse;
    std::string value = ValueText(item);
    switch (item.kind) {
      case Kind::Toggle: {
        const float w = 46 * s;
        const float h = 22 * s;
        const float x = right - w;
        const float y = top + (row - h) / 2;
        const bool on = item.value == "true";
        list->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h),
                            on ? Colour(250, 190, 40, alpha) : Colour(90, 90, 100, alpha), h / 2);
        const float knob = h - 6 * s;
        const float kx = on ? x + w - knob - 3 * s : x + 3 * s;
        list->AddRectFilled(ImVec2(kx, y + 3 * s), ImVec2(kx + knob, y + 3 * s + knob),
                            Colour(20, 20, 24, alpha), knob / 2);
        TextRight(list, x - 12 * s, text_y, size, Colour(200, 200, 205, alpha), value);
        break;
      }
      case Kind::Choice: {
        const std::string text = "<  " + value + "  >";
        TextRight(list, right, text_y, size, Colour(255, 255, 255, alpha), text);
        break;
      }
      case Kind::Slider: {
        const float w = 200 * s;
        const float h = 6 * s;
        const float x = right - 70 * s - w;
        const float y = top + (row - h) / 2;
        const float fraction = std::clamp(
            (std::strtof(item.value.c_str(), nullptr) - item.min) / (item.max - item.min), 0.0f, 1.0f);
        hits.push_back({Hit::Kind::Bar, index, x - 12 * s, top, x + w + 12 * s, top + row});
        list->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), Colour(90, 90, 100, alpha), h / 2);
        list->AddRectFilled(ImVec2(x, y), ImVec2(x + w * fraction, y + h),
                            Colour(250, 190, 40, alpha), h / 2);
        list->AddCircleFilled(ImVec2(x + w * fraction, y + h / 2), 8 * s,
                              Colour(255, 255, 255, alpha));
        TextRight(list, right, text_y, size, Colour(255, 255, 255, alpha), value);
        break;
      }
      case Kind::Text: {
        const float w = 280 * s;
        list->AddRectFilled(ImVec2(right - w, top + 4 * s), ImVec2(right, top + row - 4 * s),
                            Colour(0, 0, 0, editing_this ? 0.7f : 0.35f), 4 * s);
        if (editing_this) {
          value += "|";
        }
        const std::string shown = value.empty() ? "(empty)" : value;
        TextRight(list, right - 8 * s, text_y, size,
                  value.empty() ? Colour(150, 150, 155, alpha) : Colour(255, 255, 255, alpha), shown);
        break;
      }
      case Kind::Key: {
        const float w = 200 * s;
        list->AddRectFilled(ImVec2(right - w, top + 4 * s), ImVec2(right, top + row - 4 * s),
                            Colour(0, 0, 0, editing_this ? 0.7f : 0.35f), 4 * s);
        std::string shown = value.empty() ? "(none)" : value;
        if (editing_this) {
          shown = "Press a key";
        }
        TextRight(list, right - 8 * s, text_y, size,
                  editing_this ? Colour(250, 190, 40, 1.0f)
                  : value.empty() ? Colour(150, 150, 155, alpha)
                                  : Colour(255, 255, 255, alpha),
                  shown);
        break;
      }
    }
  }

  void Draw(uint32_t width_in, uint32_t height_in) {
    ImDrawList* list = ImGui::GetForegroundDrawList();
    width = width_in;
    height = height_in;
    // Large on every machine, as it was on a phone alone: half as large again
    // as 720 lines would have it, so it reads on a handheld's small screen and
    // from a sofa, and the panel takes the frame but for a margin. Fewer rows
    // fit; the list scrolls.
    const float s = std::max(float(height) / 720.0f, 0.4f) * 1.5f;
    scale = s;
    hits.clear();
    fade = std::clamp(open_time / 0.15f, 0.0f, 1.0f);

    list->AddRectFilled(ImVec2(0, 0), ImVec2(float(width), float(height)), Colour(0, 0, 0, 0.45f));
    const float panel_w = std::min(float(width) * 0.94f, 940 * s);
    const float panel_h = std::min(float(height) * 0.94f, 640 * s);
    const float x = (float(width) - panel_w) / 2;
    const float y = (float(height) - panel_h) / 2;
    list->AddRectFilled(ImVec2(x, y), ImVec2(x + panel_w, y + panel_h), Colour(18, 18, 24, 0.95f),
                        12 * s);
    list->AddRect(ImVec2(x, y), ImVec2(x + panel_w, y + panel_h), Colour(250, 190, 40, 0.55f),
                  12 * s, 0, 2 * s);

    const float inner_left = x + 36 * s;
    const float inner_right = x + panel_w - 36 * s;
    Text(list, inner_left, y + 20 * s, 30 * s, Colour(250, 190, 40, 1.0f), "Settings");
    // A button for the pointer to close it by.
    const float close_w = 96 * s;
    const float close_h = 38 * s;
    const float close_x = inner_right - close_w;
    const float close_y = y + 16 * s;
    list->AddRectFilled(ImVec2(close_x, close_y), ImVec2(close_x + close_w, close_y + close_h),
                        Colour(255, 255, 255, 0.10f), 8 * s);
    Text(list, close_x + (close_w - TextWidth("Close", 20 * s)) / 2, close_y + (close_h - 20 * s) / 2,
         20 * s, Colour(240, 240, 240, 1.0f), "Close");
    hits.push_back({Hit::Kind::Close, 0, close_x, close_y, close_x + close_w, close_y + close_h});
    if (restart_needed) {
      TextRight(list, close_x - 20 * s, y + 30 * s, 16 * s, Colour(250, 190, 40, 1.0f),
                "Restart the game to apply the changes marked restart");
    }

    // The tabs.
    float tab_size = 18 * s;
    float tabs_width = 0.0f;
    for (const Page& page : pages) {
      tabs_width += TextWidth(page.name, tab_size) + 26 * s;
    }
    if (tabs_width > inner_right - inner_left) {
      tab_size *= (inner_right - inner_left) / tabs_width;
    }
    float tab_x = inner_left;
    const float tab_y = y + 68 * s;
    for (size_t i = 0; i < pages.size(); ++i) {
      const float w = TextWidth(pages[i].name, tab_size);
      const bool here = int(i) == current;
      Text(list, tab_x, tab_y, tab_size,
           here ? Colour(255, 255, 255, 1.0f) : Colour(150, 150, 160, 1.0f), pages[i].name);
      if (here) {
        list->AddRectFilled(ImVec2(tab_x, tab_y + tab_size + 5 * s),
                            ImVec2(tab_x + w, tab_y + tab_size + 8 * s),
                            Colour(250, 190, 40, 1.0f), 1.5f * s);
      }
      hits.push_back({Hit::Kind::Tab, int(i), tab_x - 13 * s, tab_y - 10 * s, tab_x + w + 13 * s,
                      tab_y + tab_size + 14 * s});
      tab_x += w + 26 * s;
    }

    // The items of the page, as many as fit.
    Page& page = pages[size_t(current)];
    const float area_top = y + 112 * s;
    const float area_bottom = y + panel_h - 92 * s;
    const float row = 34 * s;
    const int rows = std::max(1, int((area_bottom - area_top) / row));
    const int count = int(page.items.size());
    row_height = row;
    if (follow_selection) {
      follow_selection = false;
      if (page.selected < page.scroll) {
        page.scroll = page.selected;
      }
      if (page.selected >= page.scroll + rows) {
        page.scroll = page.selected - rows + 1;
      }
    }
    page.scroll = std::clamp(page.scroll, 0, std::max(0, count - rows));
    for (int i = 0; i < rows && page.scroll + i < count; ++i) {
      const int index = page.scroll + i;
      DrawRow(list, page.items[size_t(index)], index, index == page.selected, inner_left,
              inner_right, area_top + float(i) * row, row, s);
    }
    if (page.scroll > 0) {
      TextRight(list, inner_right + 24 * s, area_top - 22 * s, 14 * s, Colour(150, 150, 160, 1.0f),
                "more above");
    }
    if (page.scroll + rows < count) {
      TextRight(list, inner_right + 24 * s, area_bottom - 4 * s, 14 * s,
                Colour(150, 150, 160, 1.0f), "more below");
    }

    // What the selected item is, and the keys.
    list->AddLine(ImVec2(inner_left - 14 * s, y + panel_h - 86 * s),
                  ImVec2(inner_right + 14 * s, y + panel_h - 86 * s), Colour(255, 255, 255, 0.12f));
    std::string help;
    if (notice_time > 0.0f) {
      help = notice;
    } else if (mode == Mode::EditText) {
      help = "Type the text. Enter keeps it, Esc leaves it as it was.";
    } else if (mode == Mode::CaptureKey) {
      help = "Press the key. Delete clears it, Esc leaves it as it was.";
    } else if (const Item* item = Selected()) {
      help = item->help + (item->live() ? " Applies at once." : " Applies after a restart.");
      if (!Editable(*item)) {
        help = item->help + " Cannot be changed here: the game was not started from its launcher.";
      }
    }
    // Wrapped to the panel's width, two lines at most.
    const float help_size = 16 * s;
    const float help_width = inner_right - inner_left;
    std::string line;
    float line_y = y + panel_h - 78 * s;
    int lines = 0;
    size_t start = 0;
    while (start < help.size() && lines < 2) {
      size_t end = help.size();
      while (end > start && TextWidth(help.substr(start, end - start), help_size) > help_width) {
        end = help.rfind(' ', end - 1);
        if (end == std::string::npos || end <= start) {
          end = help.size();
          break;
        }
      }
      Text(list, inner_left, line_y, help_size, Colour(215, 215, 220, 1.0f),
           help.substr(start, end - start));
      start = end + 1;
      line_y += help_size + 4 * s;
      ++lines;
    }
#ifdef __ANDROID__
    const std::string keys = "Tap an item to change it, drag the list to scroll.";
#else
    const std::string keys =
        "Pad: D-pad, A accept, B close, X default, LB RB page    "
        "Keyboard: arrows, Enter, Esc, Del default, Tab page    Mouse: click, wheel";
#endif
    // Made to fit, as the tabs are: the line for a desk is a little longer than
    // the panel is wide and ran over its edge.
    float keys_size = 13 * s;
    const float keys_width = TextWidth(keys, keys_size);
    if (keys_width > inner_right - inner_left) {
      keys_size *= (inner_right - inner_left) / keys_width;
    }
    Text(list, inner_left, y + panel_h - 30 * s, keys_size, Colour(140, 140, 150, 1.0f), keys);
    if (!Saves()) {
      TextRight(list, inner_right, y + 30 * s, 16 * s, Colour(250, 190, 40, 1.0f),
                restart_needed ? "" : "No xerenge.conf: changes last until the game is closed");
    }
  }

  PlumeMenu& menu;
  std::vector<Page> pages;
  // The layout of the last frame: its size, scale and the places the pointer
  // can find things at.
  uint32_t width = 0;
  uint32_t height = 0;
  float scale = 1.0f;
  float row_height = 0.0f;
  std::vector<Hit> hits;
  // A selection moved by the keys or the pad is brought into view; one
  // scrolled by the pointer is left where it is.
  bool follow_selection = true;
  // The finger or the button that is down.
  bool touching = false;
  bool dragged = false;
  float touch_x = 0.0f;
  float touch_y = 0.0f;
  int touch_scroll = 0;
  int bar_item = -1;
  int current = 0;
  Mode mode = Mode::Browse;
  std::string edit_backup;
  std::string conf_path;
  std::map<std::string, std::string> conf;
  bool restart_needed = false;
  std::string notice;
  float notice_time = 0.0f;
  bool from_pad = false;
  bool open = false;
  float open_time = 0.0f;
  float fade = 1.0f;
  uint16_t previous_buttons = 0;
  Direction directions[4];
  ImGuiContext* context = nullptr;
  std::chrono::steady_clock::time_point last_frame{};
};

PlumeMenu& PlumeMenu::Get() {
  static PlumeMenu menu;
  return menu;
}

PlumeMenu::PlumeMenu() {
  // XERENGE_MENU_OPEN=1: open from the start, for looking at it without a key.
  const char* open = std::getenv("XERENGE_MENU_OPEN");
  visible_ = open != nullptr && *open != '\0' && *open != '0';
}

PlumeMenu::~PlumeMenu() = default;

void PlumeMenu::AttachWindow(rex::ui::Window* window) {
  g_window = window;
  if (window) {
    // Above the SDK's own listeners (the keyboard standing in for a pad is at
    // zero), which are called from the highest down.
    window->AddInputListener(&Listener(), 100);
  }
}

void PlumeMenu::Toggle() {
  std::lock_guard lock(input_mutex_);
  visible_ = !visible_.load();
}

void PlumeMenu::Close(bool by_pad) {
  std::lock_guard lock(input_mutex_);
  visible_ = false;
  hold_pad_ = by_pad;
}

bool PlumeMenu::OnPad(uint32_t user, xerenge::MenuPadState* pad) {
  std::lock_guard lock(input_mutex_);
  if (user != 0) {
    return visible_;
  }
  pad_ = *pad;
  const auto now = std::chrono::steady_clock::now();
  pad_time_ = now;
  constexpr uint16_t kChordBits[2] = {kPadBack, kPadStart};
  const bool both = (pad->buttons & (kPadBack | kPadStart)) == (kPadBack | kPadStart);
  if (visible_ || hold_pad_) {
    // Open, Back and Start together close it: once for each time they are
    // pressed together.
    if (visible_ && both && !chord_was_down_) {
      visible_ = false;
      hold_pad_ = true;
    }
    chord_was_down_ = both;
    // Whichever of the two is down now is the menu's until it is let go.
    for (int i = 0; i < 2; ++i) {
      chord_[i].state =
          (pad->buttons & kChordBits[i]) ? ChordButton::kUsed : ChordButton::kUp;
      chord_[i].late_until = {};
    }
    if (visible_) {
      return true;
    }
    // The buttons that closed it are not for the title: Start, let go after
    // the menu was gone, would pause the race.
    if (pad->buttons == 0) {
      hold_pad_ = false;
    }
    return hold_pad_;
  }
  chord_was_down_ = both;

  // Closed: Back and Start are held back from the title when pressed. Both
  // down with one of them still held back is the chord: the menu opens, and the
  // title has seen at most the one held longer than kChordWindow. A button not
  // followed by the other within it is given to the title late.
  bool held_back = false;
  for (int i = 0; i < 2; ++i) {
    ChordState& button = chord_[i];
    if (!(pad->buttons & kChordBits[i])) {
      if (button.state == ChordButton::kHeldBack) {
        button.late_until = now + kLatePress;
      }
      button.state = ChordButton::kUp;
    } else if (button.state == ChordButton::kUp) {
      button.state = ChordButton::kHeldBack;
      button.since = now;
    } else if (button.state == ChordButton::kHeldBack && now - button.since >= kChordWindow) {
      button.state = ChordButton::kPassed;
    }
    held_back = held_back || button.state == ChordButton::kHeldBack;
  }
  if (both && held_back) {
    for (ChordState& button : chord_) {
      button.state = ChordButton::kUsed;
    }
    visible_ = true;
    return true;
  }
  for (int i = 0; i < 2; ++i) {
    const ChordState& button = chord_[i];
    if (button.state == ChordButton::kHeldBack || button.state == ChordButton::kUsed) {
      pad->buttons &= uint16_t(~kChordBits[i]);
    } else if (button.state == ChordButton::kUp && now < button.late_until) {
      pad->buttons |= kChordBits[i];
    }
  }
  return false;
}

void PlumeMenu::PushKey(uint16_t virtual_key) {
  std::lock_guard lock(input_mutex_);
  pending_keys_.push_back(virtual_key);
}

void PlumeMenu::PushPointer(PointerEvent event, float x, float y) {
  std::lock_guard lock(input_mutex_);
  if (visible_) {
    pending_pointers_.push_back({event, x, y});
  }
}

void PlumeMenu::PushScroll(float rows) {
  std::lock_guard lock(input_mutex_);
  pending_scroll_ += rows;
}

void PlumeMenu::PushCharacter(char32_t character) {
  std::lock_guard lock(input_mutex_);
  pending_characters_.push_back(character);
}

namespace {

// The frame-time graph: what a frame took, as the title swapped it, and how
// the host's presents compare. Drawn into the foreground list, so it is on
// top of the menu when that is open. `fs` is the text size; the panel is laid
// out in multiples of it, so it scales with the menu.
void DrawPerf(ImDrawList* list, ImVec2 origin, float fs) {
  uint64_t times[perf::kSwapHistory];
  float intervals[perf::kSwapHistory];
  const size_t n = perf::TitleSwapTimes(times, perf::kSwapHistory);
  size_t m = 0;
  for (size_t i = 1; i < n; ++i) {
    intervals[m++] = float(double(times[i] - times[i - 1]) / 1e6);
  }

  float fps = 0.0f, last = 0.0f, p50 = 0.0f, p99 = 0.0f, longest = 0.0f;
  if (m) {
    // The rate over the last second of swaps.
    const uint64_t newest = times[n - 1];
    size_t count = 0;
    while (count < n && newest - times[n - 1 - count] <= 1000000000ull) {
      ++count;
    }
    const uint64_t span = newest - times[n - count];
    if (count > 1 && span > 0) {
      fps = float(double(count - 1) * 1e9 / double(span));
    }
    last = intervals[m - 1];
    float sorted[perf::kSwapHistory];
    std::copy(intervals, intervals + m, sorted);
    std::sort(sorted, sorted + m);
    p50 = sorted[m / 2];
    p99 = sorted[size_t(float(m - 1) * 0.99f)];
    longest = sorted[m - 1];
  }

  // The host's presents in the last whole second: with frame generation or
  // Crash mode's smoothing this is more than the title's own rate.
  static auto window_started = std::chrono::steady_clock::now();
  static uint32_t window_presents = perf::HostPresents();
  static float window_rate = 0.0f;
  const auto now = std::chrono::steady_clock::now();
  const float elapsed = std::chrono::duration<float>(now - window_started).count();
  if (elapsed >= 1.0f) {
    const uint32_t presents = perf::HostPresents();
    window_rate = float(presents - window_presents) / elapsed;
    window_presents = presents;
    window_started = now;
  }

  char line1[128], line2[128];
  std::snprintf(line1, sizeof(line1), "%.1f fps   %.1f ms", fps, last);
  std::snprintf(line2, sizeof(line2), "p50 %.1f  p99 %.1f  max %.1f ms   window %.0f/s", p50, p99,
                longest, window_rate);
  ImFont* const font = ImGui::GetFont();
  const auto width_of = [&](const char* text) {
    return font->CalcTextSizeA(fs, FLT_MAX, 0.0f, text).x;
  };
  // As wide as the longer line: a fixed width let the stats run past the
  // backdrop at larger numbers, a 100 ms stall in the max.
  const float pad = fs * 0.6f;
  const float height = fs * 6.4f;
  const float text_width = std::max(width_of(line1), width_of(line2));
  const float width = std::max(text_width + 2 * pad, fs * 15.0f);
  const ImVec2 a = origin;
  const ImVec2 b(origin.x + width, origin.y + height);
  list->AddRectFilled(a, b, IM_COL32(8, 8, 10, 200), fs * 0.5f);
  list->AddText(font, fs, ImVec2(a.x + pad, a.y + pad * 0.7f), IM_COL32(255, 255, 255, 255), line1);
  list->AddText(font, fs, ImVec2(a.x + pad, a.y + pad * 0.7f + fs * 1.15f),
                IM_COL32(190, 190, 195, 255), line2);

  // The last 120 intervals against a 50 ms ceiling, the newest on the right;
  // the line is one frame at 60 Hz.
  const float graph_x = a.x + pad;
  const float graph_bottom = b.y - pad;
  const float graph_height = height - fs * 3.0f - pad;
  const float graph_width = width - 2 * pad;
  constexpr int kBars = 120;
  const float bar_width = graph_width / kBars;
  for (int i = 0; i < kBars && size_t(i) < m; ++i) {
    const float v = intervals[m - 1 - size_t(i)];
    const float f = std::min(v / 50.0f, 1.0f);
    const ImU32 colour = v <= 17.5f   ? IM_COL32(90, 210, 120, 255)
                         : v <= 25.0f ? IM_COL32(235, 200, 70, 255)
                                      : IM_COL32(235, 90, 80, 255);
    const float right = graph_x + graph_width - float(i) * bar_width;
    list->AddRectFilled(ImVec2(right - bar_width + 0.5f, graph_bottom - graph_height * f),
                        ImVec2(right, graph_bottom), colour);
  }
  const float line_y = graph_bottom - graph_height * (16.667f / 50.0f);
  list->AddLine(ImVec2(graph_x, line_y), ImVec2(graph_x + graph_width, line_y),
                IM_COL32(255, 255, 255, 90));
}

}  // namespace

ImDrawData* PlumeMenu::Frame(uint32_t width, uint32_t height) {
  const bool wanted = Visible();
  // The performance overlay is drawn with the menu closed; with it off, and
  // the menu closed, nothing here is touched.
  const bool overlay = settings::PerfOverlay();
  if (!state_ && !wanted && !overlay) {
    return nullptr;
  }
  if (!state_) {
    state_ = std::make_unique<State>(*this);
  }
  State& state = *state_;
  if (!wanted && state.open) {
    state.Closed();
  }
  if ((!wanted && !overlay) || width == 0 || height == 0) {
    return nullptr;
  }

  // The SDK keeps a Dear ImGui of its own, and whatever it had current is
  // given back.
  ImGuiContext* const previous = ImGui::GetCurrentContext();
  if (!state.context) {
    state.context = ImGui::CreateContext();
    ImGui::SetCurrentContext(state.context);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
    ImFontConfig font;
    font.OversampleH = 2;
    font.OversampleV = 2;
    font.PixelSnapH = false;
    io.Fonts->AddFontDefault(&font);
  }
  ImGui::SetCurrentContext(state.context);

  const auto now = std::chrono::steady_clock::now();
  const float seconds =
      state.last_frame.time_since_epoch().count() == 0
          ? 1.0f / 60.0f
          : std::clamp(std::chrono::duration<float>(now - state.last_frame).count(), 0.001f, 0.1f);
  state.last_frame = now;
  if (wanted) {
    if (!state.open) {
      state.Opened();
    }
    state.Update(seconds);
  }

  ImDrawData* data = nullptr;
  const bool menu_shown = wanted && Visible();
  if (menu_shown || overlay) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(width), float(height));
    io.DeltaTime = seconds;
    ImGui::NewFrame();
    if (menu_shown) {
      state.Draw(width, height);
    }
    if (overlay) {
      // 17 points at 720 high, half as large again on a phone, and no smaller
      // than that for a small frame to be read.
#ifdef __ANDROID__
      constexpr float kTouch = 1.5f;
#else
      constexpr float kTouch = 1.0f;
#endif
      const float fs = 17.0f * std::clamp(float(height) / 720.0f, 1.0f, 2.2f) * kTouch;
      DrawPerf(ImGui::GetForegroundDrawList(), ImVec2(fs * 0.6f, fs * 0.6f), fs);
    }
    ImGui::Render();
    data = ImGui::GetDrawData();
  }
  ImGui::SetCurrentContext(previous);
  return data;
}

}  // namespace rex::plume_renderer

extern "C" REX_GPU_PLUGIN_EXPORT bool xerenge_menu_pad(uint32_t user, xerenge::MenuPadState* pad) {
  return pad && rex::plume_renderer::PlumeMenu::Get().OnPad(user, pad);
}

extern "C" REX_GPU_PLUGIN_EXPORT void xerenge_menu_pointer(int event, float x, float y) {
  using rex::plume_renderer::PlumeMenu;
  PlumeMenu::Get().PushPointer(event == 0   ? PlumeMenu::PointerEvent::kDown
                               : event == 1 ? PlumeMenu::PointerEvent::kMove
                                            : PlumeMenu::PointerEvent::kUp,
                               x, y);
}

extern "C" REX_GPU_PLUGIN_EXPORT void xerenge_menu_toggle() {
  rex::plume_renderer::PlumeMenu::Get().Toggle();
}

extern "C" REX_GPU_PLUGIN_EXPORT bool xerenge_menu_visible() {
  return rex::plume_renderer::PlumeMenu::Get().Visible();
}
