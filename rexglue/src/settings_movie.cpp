// Driver Details > Settings with Xerenge's own buttons: GRAPHICS, HACKS and
// DEBUG under the game's sound rows, each opening a page of settings.
//
// The screen is an Apt movie (tools/aptdump shows it). Apt is handed a movie
// by CB4AptManager::AptCBLoadAnimation (0x821FF408) through sub_824277A0
// (file, data, constants, entry): the movie's "Apt Data" blob and its
// "Apt constant file", raw, as the front-end package holds them. For the
// settings movie the two are rebuilt here into new guest memory and handed
// over instead - the original data at the same offsets, with what is added
// after it, and the constants with entries of our own - so the game's own
// rows, code and offsets stay as they were.
//
// Added to the movie, as xerenge_settings.cpp lays the pages out:
//   - an import of b4Components/B4LongMenuOption (the Xbox Live options' EA
//     TRAX button, "B4LongMenuOption") as a new character;
//   - on the main page, under Music Volume, a Language row and the buttons
//     GRAPHICS, HACKS and DEBUG; on ours, our rows in place of the game's
//     and the page's name in place of [SETTINGS];
//   - each row's fields set when it loads, as the game's rows set theirs:
//     a choice row (MenuChoice, like Takedown Camera) on the natives
//     XerengeGet/XerengeSet; a button with an A press handler that calls
//     MenuScreenManager.SelectedOption(its id), which the menu service hook
//     (xerenge_menu.cpp) takes.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>

#include "custom_music.h"
#include "guest_memory.h"
#include "xerenge_settings.h"

extern "C" void __imp__sub_824277A0(PPCContext& __restrict, uint8_t*);

// The ids the buttons hand to SelectedOption (xerenge_menu.cpp).
constexpr uint32_t kXerengeOptionBase = 0x58000;

namespace {

// --- blobs ---------------------------------------------------------------

struct Blob {
  std::vector<uint8_t> bytes;

  uint32_t U32(uint32_t at) const {
    return uint32_t(bytes[at]) << 24 | uint32_t(bytes[at + 1]) << 16 | uint32_t(bytes[at + 2]) << 8 |
           bytes[at + 3];
  }
  void Set32(uint32_t at, uint32_t value) {
    bytes[at] = uint8_t(value >> 24);
    bytes[at + 1] = uint8_t(value >> 16);
    bytes[at + 2] = uint8_t(value >> 8);
    bytes[at + 3] = uint8_t(value);
  }
  void Align(uint32_t to) {
    while (bytes.size() % to) {
      bytes.push_back(0);
    }
  }
  uint32_t Here() const { return uint32_t(bytes.size()); }
  void Put8(uint8_t value) { bytes.push_back(value); }
  void Put32(uint32_t value) {
    bytes.resize(bytes.size() + 4);
    Set32(Here() - 4, value);
  }
  uint32_t PutString(const std::string& text) {
    Align(4);
    const uint32_t at = Here();
    bytes.insert(bytes.end(), text.begin(), text.end());
    bytes.push_back(0);
    return at;
  }
  std::string Str(uint32_t at) const {
    std::string text;
    while (at < bytes.size() && bytes[at]) {
      text += char(bytes[at++]);
    }
    return text;
  }
};

// The constant file: its entries, rebuilt with the strings laid out anew.
struct Constants {
  uint8_t header[20] = {};
  uint32_t root = 0;
  struct Entry {
    uint32_t type, value;
    std::string text;
  };
  std::vector<Entry> entries;

  bool Read(const uint8_t* raw, size_t size) {
    if (size < 32 || std::memcmp(raw, "Apt constant file", 17) != 0) {
      return false;
    }
    Blob b;
    b.bytes.assign(raw, raw + size);
    std::memcpy(header, raw, 20);
    root = b.U32(20);
    const uint32_t count = b.U32(24);
    if (32 + count * 8 > size) {
      return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
      Entry e{b.U32(32 + i * 8), b.U32(32 + i * 8 + 4), {}};
      if (e.type == 1) {
        e.text = b.Str(e.value);
      }
      entries.push_back(std::move(e));
    }
    return true;
  }

  uint32_t String(const std::string& text) {
    for (uint32_t i = 0; i < entries.size(); ++i) {
      if (entries[i].type == 1 && entries[i].text == text) {
        return i;
      }
    }
    entries.push_back({1, 0, text});
    return uint32_t(entries.size() - 1);
  }
  uint32_t Value(uint32_t type, uint32_t value) {
    for (uint32_t i = 0; i < entries.size(); ++i) {
      if (entries[i].type == type && entries[i].value == value) {
        return i;
      }
    }
    entries.push_back({type, value, {}});
    return uint32_t(entries.size() - 1);
  }
  uint32_t Int(int32_t value) { return Value(7, uint32_t(value)); }
  uint32_t Bool(bool value) { return Value(5, value ? 1 : 0); }

  Blob Write() const {
    Blob b;
    b.bytes.assign(header, header + 20);
    b.Put32(root);
    b.Put32(uint32_t(entries.size()));
    b.Put32(32);
    const uint32_t table = b.Here();
    b.bytes.resize(table + entries.size() * 8);
    for (uint32_t i = 0; i < entries.size(); ++i) {
      uint32_t value = entries[i].value;
      if (entries[i].type == 1) {
        value = b.PutString(entries[i].text);
      }
      b.Set32(table + i * 8, entries[i].type);
      b.Set32(table + i * 8 + 4, value);
    }
    b.Align(16);
    return b;
  }
};

// --- bytecode --------------------------------------------------------------

// ActionScript into the data blob. PushData's lists go after the code, and
// their offsets are filled in at the end (Finish).
class Code {
 public:
  explicit Code(Blob& blob) : b_(blob) {}

  uint32_t Start() const { return b_.Here(); }

  void Op(uint8_t op) { b_.Put8(op); }
  void Aligned(uint8_t op) {
    b_.Put8(op);
    b_.Align(4);
  }
  void Push(std::vector<uint32_t> constants) {
    Aligned(0x96);
    b_.Put32(uint32_t(constants.size()));
    lists_.push_back({b_.Here(), std::move(constants)});
    b_.Put32(0);
  }
  void PushRegister(uint8_t reg) {
    Op(0xB9);
    Op(reg);
  }
  // DefineFunction2 with parameters by name, and the header the game's own
  // handlers have (two registers, _global in r1). Returns where the body size goes.
  uint32_t BeginFunction(uint32_t param_name) { return BeginFunction(std::vector<uint32_t>{param_name}); }
  uint32_t BeginFunction(std::vector<uint32_t> param_names) {
    Aligned(0x8E);
    b_.Put32(empty_name_);
    b_.Put32(uint32_t(param_names.size()));
    b_.Put32(0x0002012A);
    params_.push_back({b_.Here(), std::move(param_names)});
    b_.Put32(0);  // the parameter list, placed in Finish
    const uint32_t size_at = b_.Here();
    b_.Put32(0);
    b_.Put32(0x98765432);
    b_.Put32(0x12345678);
    return size_at;
  }
  void EndFunction(uint32_t size_at) { b_.Set32(size_at, b_.Here() - (size_at + 12)); }

  void SetEmptyName(uint32_t at) { empty_name_ = at; }

  // A branch whose target comes later: Land() at the target.
  uint32_t BranchIfTrue() { return BranchOp(0x9D); }
  uint32_t BranchAlways() { return BranchOp(0x99); }
  void Land(uint32_t at) { b_.Set32(at, b_.Here() - (at + 4)); }

  void Finish() {
    for (auto& list : lists_) {
      b_.Align(4);
      const uint32_t at = b_.Here();
      for (uint32_t c : list.second) {
        b_.Put32(c);
      }
      b_.Set32(list.first, at);
    }
    for (const auto& [at, param_names] : params_) {
      b_.Align(4);
      const uint32_t list = b_.Here();
      for (uint32_t name : param_names) {
        b_.Put32(0);  // not in a register
        b_.Put32(name);
      }
      b_.Set32(at, list);
    }
    lists_.clear();
    params_.clear();
  }

 private:
  uint32_t BranchOp(uint8_t op) {
    Aligned(op);
    const uint32_t at = b_.Here();
    b_.Put32(0);
    return at;
  }

  Blob& b_;
  std::vector<std::pair<uint32_t, std::vector<uint32_t>>> lists_;
  std::vector<std::pair<uint32_t, std::vector<uint32_t>>> params_;
  uint32_t empty_name_ = 0;
};

// --- the settings movie ----------------------------------------------------
// The buttons on the main page, each opening one of ours.
struct Button {
  const char* name;
  const char* label;
  xerenge::settings::Page page;
};
constexpr Button kButtons[] = {
    {"xerenge_graphics_btn", "GRAPHICS", xerenge::settings::kGraphics},
    {"xerenge_hacks_btn", "HACKS", xerenge::settings::kHacks},
    {"xerenge_debug_btn", "DEBUG", xerenge::settings::kDebug},
};
// Where rows go: the game's rows are 40 apart, from Takedown Camera's 148; the
// main page's additions follow Music Volume, from Speaker Config's 267 (the
// row the game hides on an Xbox). Our pages hold up to 8 rows, a little closer.
constexpr float kChoiceX = 22.25f;
constexpr float kButtonX = 10.6f;  // as the Xbox Live options place EA TRAX
constexpr float kMainFirstY = 267.0f;
constexpr float kMainStep = 40.0f;
constexpr float kPageFirstY = 148.0f;
constexpr float kPageStep = 34.0f;
// The pause menu: its volume rows are 42 apart at x -80, its own items centred
// on 240 at x 217.8. Our options: the two volumes and GRAPHICS around 240; the
// GRAPHICS page's four rows likewise.
constexpr float kPauseChoiceX = -80.0f;
constexpr float kPauseButtonX = 217.8f;
constexpr float kPauseMainY[] = {256.0f, 298.0f, 340.0f};  // the volumes where the game has them
constexpr float kPausePageFirstY = 177.0f;
constexpr float kPauseStep = 42.0f;
// The game's rows, left out of our pages.
constexpr const char* kGameRows[] = {"takedown_btn", "effectsvol_btn", "musicvol_btn", "speaker_btn"};

uint32_t FloatBits(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  return bits;
}

bool Holds(const uint8_t* data, size_t size, const char* text) {
  return std::search(data, data + size, text, text + std::strlen(text)) != data + size;
}

// The rebuilt blobs, or false to leave the movie as it is.
bool Rebuild(const uint8_t* data, size_t data_size, const uint8_t* constants, size_t constants_size,
             bool pause, Blob& out_data, Blob& out_constants) {
  Constants c;
  if (!c.Read(constants, constants_size)) {
    REXLOG_WARN("menu: settings movie: its constants do not read");
    return false;
  }
  Blob& b = out_data;
  b.bytes.assign(data, data + data_size);
  const uint32_t root = c.root;
  if (b.U32(root) != 9 || b.U32(root + 4) != 0x09876543) {
    REXLOG_WARN("menu: settings movie: no movie at {:X}", root);
    return false;
  }
  b.Align(16);

  // The imports: new characters after the movie's own.
  //   Settings: the button its pages open from (the Xbox Live options' EA TRAX).
  //   The pause menu: its own buttons' component, for GRAPHICS, and the choice
  //   row of the race's menus (InGameMenuChoice2: MenuChoice's fields; MenuChoice
  //   itself is in the front end's package only), named as the race's packages
  //   name their components.
  const uint32_t imports = b.U32(root + 40), import_list = b.U32(root + 44);
  const uint32_t characters = b.U32(root + 20), character_list = b.U32(root + 24);
  std::vector<std::pair<const char*, const char*>> added;
  if (pause) {
    added = {{"b4Components/B4GameMenuOption", "GameMenuOption"}, {"b4InGameMenuChoice2", "InGameMenuChoice2"}};
  } else {
    added = {{"b4Components/B4LongMenuOption", "B4LongMenuOption"}};
  }
  const uint32_t button_character = characters;  // the first import
  std::vector<std::pair<uint32_t, uint32_t>> names;
  for (const auto& [movie, name] : added) {
    names.emplace_back(b.PutString(movie), b.PutString(name));
  }
  b.Align(4);
  const uint32_t new_imports = b.Here();
  b.bytes.insert(b.bytes.end(), data + import_list, data + import_list + imports * 16);
  for (size_t i = 0; i < names.size(); ++i) {
    b.Put32(names[i].first);
    b.Put32(names[i].second);
    b.Put32(characters + uint32_t(i));
    b.Put32(0);
  }
  b.Set32(root + 40, imports + uint32_t(names.size()));
  b.Set32(root + 44, new_imports);
  const uint32_t new_characters = b.Here();
  b.bytes.insert(b.bytes.end(), data + character_list, data + character_list + characters * 4);
  for (size_t i = 0; i < names.size(); ++i) {
    b.Put32(0);
  }
  b.Set32(root + 20, characters + uint32_t(names.size()));
  b.Set32(root + 24, new_characters);

  // The root's first frame: its items, and the template the buttons copy - the
  // hidden Speaker Config row's placement (flags, colour, clip event header).
  const uint32_t frame = b.U32(root + 12);
  const uint32_t item_count = b.U32(frame), item_list = b.U32(frame + 4);
  uint32_t speaker = 0;
  for (uint32_t i = 0; i < item_count; ++i) {
    const uint32_t item = b.U32(item_list + i * 4);
    if (b.U32(item) == 3 && (b.U32(item + 4) & 0x20) &&
        b.Str(b.U32(item + 52)) == (pause ? "btn_volume_effects" : "speaker_btn")) {
      speaker = item;
    }
  }
  if (!speaker) {
    REXLOG_WARN("menu: settings movie: no speaker_btn row to copy");
    return false;
  }
  const uint32_t speaker_events = b.U32(speaker + 60);
  const uint32_t event_flags = b.U32(b.U32(speaker_events + 4));

  Code code(b);
  code.SetEmptyName(b.PutString(""));
  const uint32_t param_name = b.PutString("m");  // before any code: nothing may go in between
  // The A press handler: MenuScreenManager.SelectedOption(m.xid), m the button.
  const auto define_handler = [&] {
    const uint32_t body = code.BeginFunction(param_name);
    // MenuSoundManager.PlaySound(SfxEnum.Select), as the Xbox Live options' EA TRAX does.
    code.Push({c.String("SfxEnum")});
    code.Op(0x1C);
    code.Push({c.String("Select")});
    code.Op(0x4E);
    code.Push({c.Int(1), c.String("MenuSoundManager")});
    code.Op(0x1C);
    code.Push({c.String("PlaySound")});
    code.Op(0x52);
    code.Op(0x17);
    code.Push({c.String("m")});
    code.Op(0x1C);  // GetVariable
    code.Push({c.String("xid")});
    code.Op(0x4E);  // GetMember
    code.Push({c.Int(1), c.String("MenuScreenManager")});
    code.Op(0x1C);
    code.Push({c.String("SelectedOption")});
    code.Op(0x52);  // CallMethod
    code.Op(0x17);  // Pop
    code.EndFunction(body);
  };
  const auto set = [&](const std::string& name, uint32_t constant) {
    code.Push({c.String(name), constant});
    code.Op(0x1D);  // SetVariable
  };
  // name = []; name[i] = values[i]
  const auto set_array = [&](const std::string& name, const std::vector<uint32_t>& values) {
    code.Push({c.String(name), c.Int(0)});
    code.Op(0x42);  // InitArray
    code.Op(0x1D);
    for (size_t i = 0; i < values.size(); ++i) {
      code.Push({c.String(name)});
      code.Op(0x1C);
      code.Push({c.Int(int32_t(i)), values[i]});
      code.Op(0x4F);  // SetMember
    }
  };
  const auto common = [&](int tab, bool first, bool shown = true) {
    set("m_bEnabled", c.Bool(shown));
    set("m_refFM", c.String("_root.gFM"));
    set("m_initiallySelected", c.Bool(first));
    set("m_tabIndex", c.Int(tab));
    set("m_bVisible", c.Bool(shown));
  };
  // A choice row's fields, as the game's Takedown Camera row sets them, with
  // our natives behind it.
  const auto choice_row = [&](const xerenge::settings::Row& row, int tab, bool first, bool shown = true) {
    const uint32_t start = code.Start();
    set("m_focusDirs", c.String("Up/Down"));
    set("m_choiceFunc", c.String("XerengeGet"));
    set("m_choiceFuncParam", c.Int(row.id));
    set("m_label", c.String(row.label));
    std::vector<uint32_t> options, funcs, params;
    for (size_t i = 0; i < row.options.size(); ++i) {
      options.push_back(c.String(row.options[i]));
      funcs.push_back(c.String("XerengeSet"));
      params.push_back(c.Int(row.id * 16 + int32_t(i)));
    }
    set_array("m_options", options);
    set_array("m_serviceFuncs", funcs);
    set_array("m_serviceFuncParams", params);
    set("m_serviceName", c.String(pause ? "PauseOptionsManager" : "DriverDetailsSettings"));
    set("m_showIcon", c.Bool(false));
    common(tab, first, shown);
    code.Op(0x00);
    code.Finish();
    return start;
  };
  // A button's fields, as the Driver Details menu's set them; with an id, A on
  // it hands the id to SelectedOption.
  const auto button_row = [&](const std::string& label, uint32_t id, int tab, bool first) {
    const uint32_t start = code.Start();
    set("m_focusDirs", c.String("Up/Down"));
    set("m_label", c.String(label));
    common(tab, first);
    if (id) {
      set("xid", c.Int(int32_t(id)));
      code.Push({c.String("xerengeSelect")});
      define_handler();
      code.Op(0x1D);
      for (const char* input : {"MENUPAGEFORWARD", "MENUPAGEFORWARD2"}) {
        // this.setOnInput(xerengeSelect, INPUTCODE.<input>)
        code.Push({c.String("xerengeSelect")});
        code.Op(0x1C);
        code.Push({c.String("INPUTCODE")});
        code.Op(0x1C);
        code.Push({c.String(input)});
        code.Op(0x4E);
        code.Push({c.Int(2)});
        code.Op(0x56);  // PushThis
        code.Push({c.String("setOnInput")});
        code.Op(0x52);
        code.Op(0x17);
      }
    }
    code.Op(0x00);
    code.Finish();
    return start;
  };
  std::vector<uint32_t> new_items;
  std::vector<std::pair<std::string, uint32_t>> buttons;  // placed buttons and their ids (0: none)
  uint32_t depth = 80;
  // A placement of ours: the template's, at its own depth, place and name, and
  // the given code run when it loads.
  const auto place = [&](uint32_t character, const std::string& name, float x, float y, uint32_t on_load) {
    b.Align(4);
    const uint32_t events = b.Here();
    b.Put32(1);
    b.Put32(events + 8);
    b.Put32(event_flags);
    b.Put32(0);
    b.Put32(on_load);
    b.Align(4);
    const uint32_t at = b.Here();
    const std::vector<uint8_t> templ(b.bytes.begin() + speaker, b.bytes.begin() + speaker + 64);
    b.bytes.insert(b.bytes.end(), templ.begin(), templ.end());
    b.Set32(at + 8, depth);
    depth += 10;
    b.Set32(at + 12, character);
    b.Set32(at + 16, FloatBits(1.0f));
    b.Set32(at + 20, 0);
    b.Set32(at + 24, 0);
    b.Set32(at + 28, FloatBits(1.0f));
    b.Set32(at + 32, FloatBits(x));
    b.Set32(at + 36, FloatBits(y));
    b.Set32(at + 52, b.PutString(name));
    b.Set32(at + 60, events);
    new_items.push_back(at);
  };
  const uint32_t choice_character = pause ? button_character + 1 : b.U32(speaker + 12);  // the choice row

  const auto page = pause ? xerenge::settings::kPause : xerenge::settings::CurrentPage();
  const auto rows = xerenge::settings::Rows(page);
  std::vector<std::string> left_out;
  if (pause) {
    // The options, in the middle of the screen as the pause menu's own items
    // are: the game's two volume rows, then GRAPHICS; and, in their place
    // once GRAPHICS is chosen, our rows - laid out now, hidden by the action below.
    const char* kVolumes[] = {"btn_volume_effects", "btn_volume_music"};
    for (uint32_t i = 0; i < item_count; ++i) {
      const uint32_t item = b.U32(item_list + i * 4);
      if (b.U32(item) != 3 || !(b.U32(item + 4) & 0x20)) {
        continue;
      }
      const std::string name = b.Str(b.U32(item + 52));
      for (int v = 0; v < 2; ++v) {
        if (name == kVolumes[v]) {
          b.Set32(item + 36, FloatBits(kPauseMainY[v]));
        }
      }
    }
    place(button_character, "xerenge_graphics_btn", kPauseButtonX, kPauseMainY[2],
          button_row("GRAPHICS", 0, 2, false));
    std::vector<std::string> graphics;
    int tab = 3;
    float y = kPausePageFirstY;
    for (const auto& row : rows) {
      const std::string name = "xerenge_row" + std::to_string(row.id);
      place(choice_character, name, kPauseChoiceX, y, choice_row(row, tab++, false));
      graphics.push_back(name);
      y += kPauseStep;
    }

    // The action, after the screen's own has set it up (its constructor
    // binds its key handler):
    //   xerengeShow(true|false): one page or the other - its rows shown and
    //     enabled, the other's hidden and disabled, the focus on its first
    //     row, the title naming it;
    //   A on GRAPHICS: xerengeShow(true);
    //   the key handler: back on GRAPHICS' page goes to the volumes, anything
    //     else to the screen's own handler (PauseOptionsMenu.mo_thisClass).
    const uint32_t name_page = b.PutString("page");
    const uint32_t name_k = b.PutString("k");
    const uint32_t name_c = b.PutString("c");
    const uint32_t action_code = code.Start();
    const auto call0 = [&](const std::string& object, const char* method) {
      code.Push({c.Int(0), c.String(object)});
      code.Op(0x1C);
      code.Push({c.String(method)});
      code.Op(0x52);
      code.Op(0x17);
    };
    const auto page_rows = [&](const std::vector<std::string>& names, bool on) {
      for (const auto& name : names) {
        if (!on) {
          call0(name, "unhighlight");  // hidden highlighted, it stayed so
        }
        call0(name, on ? "show" : "hide");
        call0(name, on ? "enable" : "disable");
      }
    };
    const std::vector<std::string> main_rows = {"btn_volume_effects", "btn_volume_music", "xerenge_graphics_btn"};
    const auto show_page = [&](bool to_graphics) {
      page_rows(to_graphics ? main_rows : graphics, false);
      page_rows(to_graphics ? graphics : main_rows, true);
      if (to_graphics) {
        // row.mIsVisible = true: goNext/goPrevious do nothing without it, and
        // only the row's showOptions sets it, which a row hidden at first missed.
        for (const auto& name : graphics) {
          code.Push({c.String(name)});
          code.Op(0x1C);
          code.Push({c.String("mIsVisible"), c.Bool(true)});
          code.Op(0x4F);
        }
      }
      // gFM.selectItemByIdx(first row of the page)
      code.Push({c.Int(to_graphics ? 3 : 2), c.Int(1), c.String("gFM")});
      code.Op(0x1C);
      code.Push({c.String("selectItemByIdx")});
      code.Op(0x52);
      code.Op(0x17);
      // gStdTitle.screen.setText("$PauseTitle", subtitle)
      code.Push({to_graphics ? c.String("[GRAPHICS]") : c.String("$PauseSubOptions"), c.String("$PauseTitle"), c.Int(2),
                 c.String("gStdTitle")});
      code.Op(0x1C);
      code.Push({c.String("screen")});
      code.Op(0x4E);
      code.Push({c.String("setText")});
      code.Op(0x52);
      code.Op(0x17);
      set("xerengeOnGraphics", c.Bool(to_graphics));
    };
    set("xerengeOnGraphics", c.Bool(false));
    // The GRAPHICS page's rows, set up like any (hidden from the start, they came
    // up without their values), put away until GRAPHICS is chosen.
    page_rows(graphics, false);
    // xerengeShowGraphics = function(m) { sound; show_page(true) }
    code.Push({c.String("xerengeShowGraphics")});
    {
      const uint32_t body = code.BeginFunction(param_name);
      code.Push({c.String("SfxEnum")});
      code.Op(0x1C);
      code.Push({c.String("Select")});
      code.Op(0x4E);
      code.Push({c.Int(1), c.String("MenuSoundManager")});
      code.Op(0x1C);
      code.Push({c.String("PlaySound")});
      code.Op(0x52);
      code.Op(0x17);
      show_page(true);
      code.EndFunction(body);
    }
    code.Op(0x1D);
    // xerengeShowMain = function(page) { sound; show_page(false) }
    code.Push({c.String("xerengeShowMain")});
    {
      const uint32_t body = code.BeginFunction(name_page);
      code.Push({c.String("SfxEnum")});
      code.Op(0x1C);
      code.Push({c.String("Back")});
      code.Op(0x4E);
      code.Push({c.Int(1), c.String("MenuSoundManager")});
      code.Op(0x1C);
      code.Push({c.String("PlaySound")});
      code.Op(0x52);
      code.Op(0x17);
      show_page(false);
      code.EndFunction(body);
    }
    code.Op(0x1D);
    // xerengeKeys = function(k, c) { ... }
    code.Push({c.String("xerengeKeys")});
    {
      const uint32_t body = code.BeginFunction(std::vector<uint32_t>{name_k, name_c});
      // Probe (XERENGE_MENU_PROBE): PauseOptionsManager.XerengeSet(-k), then -1000 if the
      // focus manager has a handler of its own for the key, -2000 if not.
      if (std::getenv("XERENGE_MENU_PROBE")) {
        code.Push({c.Int(0), c.String("k")});
        code.Op(0x1C);
        code.Op(0x0B);  // Subtract: 0 - k
        code.Push({c.Int(1), c.String("PauseOptionsManager")});
        code.Op(0x1C);
        code.Push({c.String("XerengeSet")});
        code.Op(0x52);
        code.Op(0x17);
      }
      code.Push({c.String("xerengeOnGraphics")});
      code.Op(0x1C);
      code.Op(0x12);  // Not
      const uint32_t not_on_graphics = code.BranchIfTrue();
      code.Push({c.String("k")});
      code.Op(0x1C);
      code.Push({c.String("INPUTCODE")});
      code.Op(0x1C);
      code.Push({c.String("MENUPAGEBACK")});
      code.Op(0x4E);
      code.Op(0x49);  // Equals2
      code.Op(0x4C);  // PushDuplicate
      const uint32_t is_back = code.BranchIfTrue();
      code.Op(0x17);  // Pop
      code.Push({c.String("k")});
      code.Op(0x1C);
      code.Push({c.String("INPUTCODE")});
      code.Op(0x1C);
      code.Push({c.String("MENUPAGEBACK2")});
      code.Op(0x4E);
      code.Op(0x49);
      code.Land(is_back);
      code.Op(0x12);
      const uint32_t not_back = code.BranchIfTrue();
      // xerengeShowMain(); return
      code.Push({c.Int(0), c.String("xerengeShowMain")});
      code.Op(0x3D);  // CallFunction
      code.Op(0x17);
      code.Push({c.Value(3, 0)});
      code.Op(0x3E);  // Return
      code.Land(not_on_graphics);
      code.Land(not_back);
      // PauseOptionsMenu.mo_thisClass.localEventHandler(k, c)
      code.Push({c.String("c")});
      code.Op(0x1C);
      code.Push({c.String("k")});
      code.Op(0x1C);
      code.Push({c.Int(2), c.String("PauseOptionsMenu")});
      code.Op(0x1C);
      code.Push({c.String("mo_thisClass")});
      code.Op(0x4E);
      code.Push({c.String("localEventHandler")});
      code.Op(0x52);
      code.Op(0x17);
      code.EndFunction(body);
    }
    code.Op(0x1D);
    // xerengeArrows = function(m) { gFM's LEFT and RIGHT to m.goNext and
    // m.goPrevious }, as a row's own setInputs binds them; on each of our
    // rows' focus, as the volume rows' bindings stayed otherwise.
    code.Push({c.String("xerengeArrows")});
    {
      const uint32_t body = code.BeginFunction(param_name);
      for (const auto& [input, method] : {std::pair{"LEFT", "goNext"}, std::pair{"RIGHT", "goPrevious"}}) {
        code.Push({c.String("m")});
        code.Op(0x1C);
        code.Push({c.String("m")});
        code.Op(0x1C);
        code.Push({c.String(method)});
        code.Op(0x4E);
        code.Push({c.String("INPUTCODE")});
        code.Op(0x1C);
        code.Push({c.String(input)});
        code.Op(0x4E);
        code.Push({c.Int(3), c.String("gFM")});
        code.Op(0x1C);
        code.Push({c.String("setOnInput")});
        code.Op(0x52);
        code.Op(0x17);
      }
      code.EndFunction(body);
    }
    code.Op(0x1D);
    for (const auto& name : graphics) {
      code.Push({c.String("xerengeArrows")});
      code.Op(0x1C);
      code.Push({c.Int(1), c.String(name)});
      code.Op(0x1C);
      code.Push({c.String("setOnFocus")});
      code.Op(0x52);
      code.Op(0x17);
    }
    // gEH.setLocalEventHandler(xerengeKeys)
    code.Push({c.String("xerengeKeys")});
    code.Op(0x1C);
    code.Push({c.Int(1), c.String("gEH")});
    code.Op(0x1C);
    code.Push({c.String("setLocalEventHandler")});
    code.Op(0x52);
    code.Op(0x17);
    // xerenge_graphics_btn.setOnInput(INPUTCODE.<A>, xerengeShowGraphics)
    for (const char* input : {"MENUPAGEFORWARD", "MENUPAGEFORWARD2"}) {
      code.Push({c.String("xerengeShowGraphics")});
      code.Op(0x1C);
      code.Push({c.String("INPUTCODE")});
      code.Op(0x1C);
      code.Push({c.String(input)});
      code.Op(0x4E);
      code.Push({c.Int(2), c.String("xerenge_graphics_btn")});
      code.Op(0x1C);
      code.Push({c.String("setOnInput")});
      code.Op(0x52);
      code.Op(0x17);
    }
    code.Op(0x00);
    code.Finish();
    b.Align(4);
    const uint32_t action = b.Here();
    b.Put32(1);
    b.Put32(action_code);
    new_items.push_back(action);
  } else if (page == xerenge::settings::kMain) {
    // The game's rows (tabs 0-2), then ours: Language, and the buttons. Speaker
    // Config, which the game hides on an Xbox, goes: Language takes its place.
    left_out.push_back("speaker_btn");
    float y = kMainFirstY;
    int tab = 3;  // Speaker Config's, then on: the focus manager wants them without gaps
    for (const auto& row : rows) {
      place(choice_character, "xerenge_row" + std::to_string(row.id), kChoiceX, y,
            choice_row(row, tab++, false));
      y += kMainStep;
    }
    for (const Button& button : kButtons) {
      buttons.push_back({button.name, kXerengeOptionBase + button.page});
      place(button_character, button.name, kButtonX, y,
            button_row(button.label, kXerengeOptionBase + button.page, tab++, false));
      y += kMainStep;
    }
  } else {
    // The page's rows in place of the game's.
    left_out.assign(std::begin(kGameRows), std::end(kGameRows));
    float y = kPageFirstY;
    int tab = 0;
    for (const auto& row : rows) {
      const std::string name = "xerenge_row" + std::to_string(tab);
      if (row.options.empty()) {
        buttons.push_back({name, 0});
        place(button_character, name, kButtonX, y, button_row(row.label, 0, tab, tab == 0));
      } else {
        place(choice_character, name, kChoiceX, y, choice_row(row, tab, tab == 0));
      }
      ++tab;
      y += kPageStep;
    }
    // The page's name under DRIVER DETAILS, in place of [SETTINGS].
    for (auto& entry : c.entries) {
      if (entry.type == 1 && entry.text == "$FEDRIVERDETSETTING") {
        entry.text = xerenge::settings::PageTitle(page);
      }
    }
  }

  // An action after the placements, once the buttons have set themselves up
  // (a control clears its inputs when it does, so what its onLoad bound is
  // gone): A on each button with an id to the handler; and, as a button gets
  // the focus, LEFT and RIGHT taken back from the focus manager - a choice
  // row binds them there for itself (MenuChoice's setInputs) and leaves them
  // bound when the focus moves on.
  if (!buttons.empty()) {
    const uint32_t action_code = code.Start();
    code.Push({c.String("xerengeSelect")});
    define_handler();
    code.Op(0x1D);
    code.Push({c.String("xerengeNoArrows")});
    const uint32_t body = code.BeginFunction(param_name);
    for (const char* input : {"LEFT", "RIGHT"}) {
      // _root.gFM.setOnInput(INPUTCODE.<input>, undefined, undefined)
      code.Push({c.Value(3, 0), c.Value(3, 0)});  // undefined twice, as the game's code pushes it
      code.Push({c.String("INPUTCODE")});
      code.Op(0x1C);
      code.Push({c.String(input)});
      code.Op(0x4E);
      code.Push({c.Int(3), c.String("_root")});
      code.Op(0x1C);
      code.Push({c.String("gFM")});
      code.Op(0x4E);
      code.Push({c.String("setOnInput")});
      code.Op(0x52);
      code.Op(0x17);
    }
    code.EndFunction(body);
    code.Op(0x1D);
    for (const auto& [name, id] : buttons) {
      if (id) {
        for (const char* input : {"MENUPAGEFORWARD", "MENUPAGEFORWARD2"}) {
          // name.setOnInput(INPUTCODE.<input>, xerengeSelect)
          code.Push({c.String("xerengeSelect")});
          code.Op(0x1C);
          code.Push({c.String("INPUTCODE")});
          code.Op(0x1C);
          code.Push({c.String(input)});
          code.Op(0x4E);
          code.Push({c.Int(2), c.String(name)});
          code.Op(0x1C);
          code.Push({c.String("setOnInput")});
          code.Op(0x52);
          code.Op(0x17);
        }
      }
      // name.setOnFocus(xerengeNoArrows)
      code.Push({c.String("xerengeNoArrows")});
      code.Op(0x1C);
      code.Push({c.Int(1), c.String(name)});
      code.Op(0x1C);
      code.Push({c.String("setOnFocus")});
      code.Op(0x52);
      code.Op(0x17);
    }
    code.Op(0x00);
    code.Finish();
    b.Align(4);
    const uint32_t action = b.Here();
    b.Put32(1);
    b.Put32(action_code);
    new_items.push_back(action);
  }

  // The frame's item list: the game's (less its rows on our pages), then ours.
  b.Align(4);
  const uint32_t new_item_list = b.Here();
  uint32_t kept = 0;
  for (uint32_t i = 0; i < item_count; ++i) {
    const uint32_t item = b.U32(item_list + i * 4);
    if (b.U32(item) == 3 && (b.U32(item + 4) & 0x20)) {
      const std::string name = b.Str(b.U32(item + 52));
      if (std::find(left_out.begin(), left_out.end(), name) != left_out.end()) {
        continue;
      }
    }
    b.Put32(item);
    ++kept;
  }
  for (uint32_t item : new_items) {
    b.Put32(item);
  }
  b.Set32(frame, kept + uint32_t(new_items.size()));
  b.Set32(frame + 4, new_item_list);
  b.Align(16);

  out_constants = c.Write();
  return true;
}

// Driver Details > Player File with an ACHIEVEMENTS button above Load Profile: a
// copy of New Profile's (SmallMenuOption), whose A hands kAchievements to
// SelectedOption, which opens the Settings movie on that page (xerenge_menu.cpp).
bool RebuildProfile(const uint8_t* data, size_t data_size, const uint8_t* constants,
                    size_t constants_size, Blob& out_data, Blob& out_constants) {
  Constants c;
  if (!c.Read(constants, constants_size)) {
    REXLOG_WARN("menu: player file movie: its constants do not read");
    return false;
  }
  Blob& b = out_data;
  b.bytes.assign(data, data + data_size);
  const uint32_t root = c.root;
  if (b.U32(root) != 9 || b.U32(root + 4) != 0x09876543) {
    return false;
  }
  b.Align(16);
  const uint32_t frame = b.U32(root + 12);
  const uint32_t item_count = b.U32(frame), item_list = b.U32(frame + 4);
  uint32_t templ = 0;
  for (uint32_t i = 0; i < item_count; ++i) {
    const uint32_t item = b.U32(item_list + i * 4);
    if (b.U32(item) == 3 && (b.U32(item + 4) & 0x20) && b.Str(b.U32(item + 52)) == "new_btn") {
      templ = item;
    }
  }
  if (!templ) {
    REXLOG_WARN("menu: player file movie: no new_btn to copy");
    return false;
  }
  // The game's rows one place further in the focus order (Load Profile 0 to
  // Autosave 3 become 1 to 4): in each one's load code, the PushData of
  // "m_tabIndex" and a number gets a new constant, one more, in the number's
  // place; the shared constants stay as they are. A PushData's items are
  // constants, or (type 8) entries of the ConstantPool the code set up first,
  // as Autosave's are. A negative index for ours left it out of the order.
  const auto resolve = [&](uint32_t index, const std::vector<uint32_t>& pool) {
    if (index < c.entries.size() && c.entries[index].type == 8 && c.entries[index].value < pool.size()) {
      return pool[c.entries[index].value];
    }
    return index;
  };
  int moved = 0;
  for (uint32_t i = 0; i < item_count; ++i) {
    const uint32_t item = b.U32(item_list + i * 4);
    if (b.U32(item) != 3 || !(b.U32(item + 4) & 0x20) || !b.U32(item + 60)) {
      continue;
    }
    const std::string row = b.Str(b.U32(item + 52));
    if (row != "load_btn" && row != "save_btn" && row != "new_btn" && row != "autosave_btn") {
      continue;
    }
    const uint32_t row_events = b.U32(item + 60);
    const uint32_t code_at = b.U32(b.U32(row_events + 4) + 8);
    if (code_at >= data_size) {
      continue;
    }
    std::vector<uint32_t> pool;
    if (b.bytes[code_at] == 0x88) {
      const uint32_t at = (code_at + 4) & ~3u;
      const uint32_t count = b.U32(at), list = b.U32(at + 4);
      for (uint32_t k = 0; k < count && list < data_size && k < (data_size - list) / 4; ++k) {
        pool.push_back(b.U32(list + k * 4));
      }
    }
    // A PushData: its count and where its list lies, any word pair that may be
    // one, its bounds checked without overflowing.
    const uint32_t end = std::min<uint32_t>(uint32_t(data_size), code_at + 0x200);
    for (uint32_t p = (code_at + 3) & ~3u; p + 8 <= end; p += 4) {
      const uint32_t count = b.U32(p), list = b.U32(p + 4);
      if (count != 2 || list >= data_size || (data_size - list) < 8) {
        continue;
      }
      const uint32_t name = resolve(b.U32(list), pool);
      if (name >= c.entries.size() || c.entries[name].type != 1 || c.entries[name].text != "m_tabIndex") {
        continue;
      }
      const uint32_t value = resolve(b.U32(list + 4), pool);
      if (value >= c.entries.size()) {
        continue;
      }
      const auto& entry = c.entries[value];
      float number = -1.0f;
      if (entry.type == 7) {
        number = float(int32_t(entry.value));
      } else if (entry.type == 6) {
        std::memcpy(&number, &entry.value, 4);
      }
      if (number >= 0.0f && number <= 3.0f) {
        b.Set32(list + 4, c.Int(int32_t(number) + 1));
        ++moved;
      }
      break;
    }
  }
  if (moved != 4) {
    REXLOG_WARN("menu: player file movie: {} of its 4 rows moved in the focus order", moved);
  }
  const uint32_t templ_events = b.U32(templ + 60);
  const uint32_t event_flags = b.U32(b.U32(templ_events + 4));

  Code code(b);
  code.SetEmptyName(b.PutString(""));
  const uint32_t param_name = b.PutString("m");
  const auto set = [&](const std::string& name, uint32_t constant) {
    code.Push({c.String(name), constant});
    code.Op(0x1D);  // SetVariable
  };
  // The button's fields, as New Profile's.
  const uint32_t on_load = code.Start();
  set("m_focusDirs", c.String("Up/Down"));
  set("m_label", c.String("ACHIEVEMENTS"));
  set("m_bEnabled", c.Bool(true));
  set("m_refFM", c.String("_root.gFM"));
  set("m_initiallySelected", c.Bool(false));
  set("m_tabIndex", c.Int(0));  // first; the game's rows move down one, below
  set("m_bVisible", c.Bool(true));
  set("xid", c.Int(kXerengeOptionBase + xerenge::settings::kAchievements));
  code.Op(0x00);
  code.Finish();

  // A on it (either of its codes), bound by a frame action after the screen's
  // own setup, as Player File binds its buttons': set in the load event, the
  // component's own setup cleared it. xerengeSelect(m), m the id passed as
  // setOnInput's third argument: MenuSoundManager.PlaySound(SfxEnum.Select)
  // and MenuScreenManager.SelectedOption(m).
  const uint32_t action_code = code.Start();
  code.Push({c.String("xerengeSelect")});
  const uint32_t body = code.BeginFunction(param_name);
  code.Push({c.String("SfxEnum")});
  code.Op(0x1C);
  code.Push({c.String("Select")});
  code.Op(0x4E);
  code.Push({c.Int(1), c.String("MenuSoundManager")});
  code.Op(0x1C);
  code.Push({c.String("PlaySound")});
  code.Op(0x52);
  code.Op(0x17);
  // m: the id, setOnInput's third argument, as Player File's own buttons pass theirs.
  code.Push({c.String("m")});
  code.Op(0x1C);
  code.Push({c.Int(1), c.String("MenuScreenManager")});
  code.Op(0x1C);
  code.Push({c.String("SelectedOption")});
  code.Op(0x52);
  code.Op(0x17);
  code.EndFunction(body);
  code.Op(0x1D);
  for (const char* input : {"MENUPAGEFORWARD", "MENUPAGEFORWARD2"}) {
    // xerenge_achievements_btn.setOnInput(INPUTCODE.<input>, xerengeSelect, id)
    code.Push({c.Int(int32_t(kXerengeOptionBase + xerenge::settings::kAchievements))});
    code.Push({c.String("xerengeSelect")});
    code.Op(0x1C);
    code.Push({c.String("INPUTCODE")});
    code.Op(0x1C);
    code.Push({c.String(input)});
    code.Op(0x4E);
    code.Push({c.Int(3)});
    code.Push({c.String("xerenge_achievements_btn")});
    code.Op(0x1C);
    code.Push({c.String("setOnInput")});
    code.Op(0x52);
    code.Op(0x17);
  }
  code.Op(0x00);
  code.Finish();

  // The placement: New Profile's, above Load Profile (236.75), at a depth of its own.
  b.Align(4);
  const uint32_t events = b.Here();
  b.Put32(1);
  b.Put32(events + 8);
  b.Put32(event_flags);
  b.Put32(0);
  b.Put32(on_load);
  b.Align(4);
  const uint32_t at = b.Here();
  const std::vector<uint8_t> copy(b.bytes.begin() + templ, b.bytes.begin() + templ + 64);
  b.bytes.insert(b.bytes.end(), copy.begin(), copy.end());
  b.Set32(at + 8, 80);
  b.Set32(at + 36, FloatBits(196.75f));
  b.Set32(at + 52, b.PutString("xerenge_achievements_btn"));
  b.Set32(at + 60, events);
  b.Align(4);
  const uint32_t action = b.Here();
  b.Put32(1);
  b.Put32(action_code);

  b.Align(4);
  const uint32_t new_item_list = b.Here();
  for (uint32_t i = 0; i < item_count; ++i) {
    b.Put32(b.U32(item_list + i * 4));
  }
  b.Put32(at);
  b.Put32(action);
  b.Set32(frame, item_count + 2);
  b.Set32(frame + 4, new_item_list);
  b.Align(16);
  out_constants = c.Write();
  return true;
}

// The achievements, in Driver Details > Stats's movie: its list (b4StatsList)
// takes its rows from _global[m_serviceManager][m_serviceManagerFunc]() -
// {page: [{title, element: [{first, second, third}, ...]}]} - which for Stats is
// DriverDetailsStatsService.GetProfileStatistics, a native. Here it is
// XerengeAchievements.Get, a function of ActionScript defined by a placement
// of ours before the list loads, with the achievements written into it; the
// title under DRIVER DETAILS is the achievements' count.
bool RebuildAchievements(const uint8_t* data, size_t data_size, const uint8_t* constants,
                         size_t constants_size, Blob& out_data, Blob& out_constants) {
  Constants c;
  if (!c.Read(constants, constants_size)) {
    REXLOG_WARN("menu: achievements movie: its constants do not read");
    return false;
  }
  Blob& b = out_data;
  b.bytes.assign(data, data + data_size);
  const uint32_t root = c.root;
  if (b.U32(root) != 9 || b.U32(root + 4) != 0x09876543) {
    return false;
  }
  b.Align(16);
  const uint32_t frame = b.U32(root + 12);
  const uint32_t item_count = b.U32(frame), item_list = b.U32(frame + 4);
  uint32_t list_item = 0, holder_item = 0;
  for (uint32_t i = 0; i < item_count; ++i) {
    const uint32_t item = b.U32(item_list + i * 4);
    if (b.U32(item) == 3 && (b.U32(item + 4) & 0x20)) {
      const std::string name = b.Str(b.U32(item + 52));
      if (name == "list_component") {
        list_item = item;
      } else if (name == "bg_holder") {
        holder_item = item;
      }
    }
  }
  if (!list_item || !holder_item) {
    REXLOG_WARN("menu: achievements movie: no list_component or bg_holder");
    return false;
  }
  for (auto& entry : c.entries) {
    if (entry.type != 1) {
      continue;
    }
    if (entry.text == "GetProfileStatistics") {
      entry.text = "Get";
    } else if (entry.text == "DriverDetailsStatsService") {
      entry.text = "XerengeAchievements";
    } else if (entry.text == "$FEDRIVERDETSTAT") {
      entry.text = xerenge::settings::PageTitle(xerenge::settings::kAchievements);
    }
  }

  Code code(b);
  code.SetEmptyName(b.PutString(""));
  // _global.XerengeAchievements = {Get: function() { return {page: [{title,
  // element: [{first, second, third}, ...]}]}; }}
  const auto rows = xerenge::settings::AchievementRows();
  const uint32_t on_load = code.Start();
  code.Push({c.String("_global")});
  code.Op(0x1C);  // GetVariable
  code.Push({c.String("XerengeAchievements"), c.String("Get")});
  const uint32_t body = code.BeginFunction(std::vector<uint32_t>{});
  code.Push({c.String("page"), c.String("title"), c.String("ACHIEVEMENTS"), c.String("element")});
  for (size_t i = rows.size(); i-- > 0;) {  // InitArray takes them last first
    code.Push({c.String("first"), c.String(rows[i].name), c.String("second"), c.String(rows[i].score),
               c.String("third"), c.String(rows[i].state), c.Int(3)});
    code.Op(0x43);  // InitObject
  }
  code.Push({c.Int(int32_t(rows.size()))});
  code.Op(0x42);  // InitArray
  code.Push({c.Int(2)});
  code.Op(0x43);
  code.Push({c.Int(1)});
  code.Op(0x42);
  code.Push({c.Int(1)});
  code.Op(0x43);
  code.Op(0x3E);  // Return
  code.EndFunction(body);
  code.Push({c.Int(1)});
  code.Op(0x43);
  code.Op(0x4F);  // SetMember
  code.Op(0x00);
  code.Finish();

  // The placement: the list's (for its clip event header), holding
  // bg_holder's sprite, scaled to nothing, before the list in the frame.
  const uint32_t list_events = b.U32(list_item + 60);
  const uint32_t event_flags = b.U32(b.U32(list_events + 4));
  b.Align(4);
  const uint32_t events = b.Here();
  b.Put32(1);
  b.Put32(events + 8);
  b.Put32(event_flags);
  b.Put32(0);
  b.Put32(on_load);
  b.Align(4);
  const uint32_t at = b.Here();
  const std::vector<uint8_t> copy(b.bytes.begin() + list_item, b.bytes.begin() + list_item + 64);
  b.bytes.insert(b.bytes.end(), copy.begin(), copy.end());
  b.Set32(at + 8, 200);
  b.Set32(at + 12, b.U32(holder_item + 12));
  b.Set32(at + 16, 0);
  b.Set32(at + 28, 0);
  b.Set32(at + 52, b.PutString("xerenge_achievements_data"));
  b.Set32(at + 60, events);

  // The buttons shown at the bottom right (b4IconOptionDisplay, at 591, 418):
  // Back B and, for the popup, Info A - its load code ours, as Player File's
  // Buttons_Continue sets Back and Select.
  for (uint32_t i = 0; i < item_count; ++i) {
    const uint32_t item = b.U32(item_list + i * 4);
    if (b.U32(item) != 3 || !(b.U32(item + 4) & 0x80) || !b.U32(item + 60) ||
        b.U32(item + 32) != FloatBits(591.0f) || b.U32(item + 36) != FloatBits(418.0f)) {
      continue;
    }
    const uint32_t buttons_code = code.Start();
    for (const auto& [name, values] :
         {std::pair<const char*, std::vector<const char*>>{"displayOptions", {"$FEBACK", "$XerengeInfo"}},
          std::pair<const char*, std::vector<const char*>>{"InputCodes", {"MENUPAGEBACK", "MENUPAGEFORWARD"}}}) {
      code.Push({c.String(name), c.Int(0)});
      code.Op(0x42);  // InitArray
      code.Op(0x1D);  // SetVariable
      for (size_t v = 0; v < values.size(); ++v) {
        code.Push({c.String(name)});
        code.Op(0x1C);
        code.Push({c.Int(int32_t(v)), c.String(values[v])});
        code.Op(0x4F);  // SetMember
      }
    }
    code.Op(0x00);
    code.Finish();
    b.Set32(b.U32(b.U32(item + 60) + 4) + 8, buttons_code);
  }

  // A on an achievement: its popup. The list's key handler (gEH's, set as it
  // started) takes none; an action after the placements puts one of ours in
  // front of it -
  //   _global.xerengeListKeys = list_component.localEventHandler;
  //   gEH.setLocalEventHandler(function(k, c) {
  //     if (k == INPUTCODE.MENUPAGEFORWARD || k == INPUTCODE.MENUPAGEFORWARD2)
  //       MenuScreenManager.SelectedOption(0x58100 + list_component.mnActiveItem);
  //     else _global.xerengeListKeys(k, c);
  //   });
  // and the hook on SelectedOption (xerenge_menu.cpp) shows it.
  const uint32_t name_k = b.PutString("k");
  const uint32_t name_c = b.PutString("c");
  const uint32_t action_code = code.Start();
  code.Push({c.String("_global")});
  code.Op(0x1C);
  code.Push({c.String("xerengeListKeys"), c.String("list_component")});
  code.Op(0x1C);
  code.Push({c.String("localEventHandler")});
  code.Op(0x4E);  // GetMember
  code.Op(0x4F);  // SetMember
  const uint32_t keys = code.BeginFunction(std::vector<uint32_t>{name_k, name_c});
  std::vector<uint32_t> to_select;
  for (const char* input : {"MENUPAGEFORWARD", "MENUPAGEFORWARD2"}) {
    code.Push({c.String("k")});
    code.Op(0x1C);
    code.Push({c.String("INPUTCODE")});
    code.Op(0x1C);
    code.Push({c.String(input)});
    code.Op(0x4E);
    code.Op(0x49);  // Equals2
    to_select.push_back(code.BranchIfTrue());
  }
  // Not A: the list's own.
  code.Push({c.String("c")});
  code.Op(0x1C);
  code.Push({c.String("k")});
  code.Op(0x1C);
  code.Push({c.Int(2), c.String("_global")});
  code.Op(0x1C);
  code.Push({c.String("xerengeListKeys")});
  code.Op(0x52);  // CallMethod
  code.Op(0x17);  // Pop
  const uint32_t done = code.BranchAlways();
  for (const uint32_t branch : to_select) {
    code.Land(branch);
  }
  code.Push({c.Int(int32_t(kXerengeOptionBase + 0x100)), c.String("list_component")});
  code.Op(0x1C);
  code.Push({c.String("mnActiveItem")});
  code.Op(0x4E);
  code.Op(0x47);  // Add2
  code.Push({c.Int(1), c.String("MenuScreenManager")});
  code.Op(0x1C);
  code.Push({c.String("SelectedOption")});
  code.Op(0x52);
  code.Op(0x17);
  code.Land(done);
  code.EndFunction(keys);
  code.Push({c.Int(1), c.String("gEH")});
  code.Op(0x1C);
  code.Push({c.String("setLocalEventHandler")});
  code.Op(0x52);
  code.Op(0x17);
  code.Op(0x00);
  code.Finish();
  b.Align(4);
  const uint32_t action = b.Here();
  b.Put32(1);
  b.Put32(action_code);

  b.Align(4);
  const uint32_t new_item_list = b.Here();
  for (uint32_t i = 0; i < item_count; ++i) {
    const uint32_t item = b.U32(item_list + i * 4);
    if (item == list_item) {
      b.Put32(at);
    }
    b.Put32(item);
  }
  b.Put32(action);
  b.Set32(frame, item_count + 2);
  b.Set32(frame + 4, new_item_list);
  b.Align(16);
  out_constants = c.Write();
  return true;
}

// The main menu's button hints (b4IconOptionDisplay, at 591.1, 418.05): Quit B
// beside Continue A, B opening the quit popup (xerenge_menu.cpp) - their load
// code ours, as the game's sets Continue alone.
bool RebuildMainMenu(const uint8_t* data, size_t data_size, const uint8_t* constants,
                     size_t constants_size, Blob& out_data, Blob& out_constants) {
  Constants c;
  if (!c.Read(constants, constants_size)) {
    return false;
  }
  Blob& b = out_data;
  b.bytes.assign(data, data + data_size);
  const uint32_t root = c.root;
  if (b.U32(root) != 9 || b.U32(root + 4) != 0x09876543) {
    return false;
  }
  b.Align(16);
  const uint32_t frame = b.U32(root + 12);
  const uint32_t item_count = b.U32(frame), item_list = b.U32(frame + 4);
  Code code(b);
  bool done = false;
  for (uint32_t i = 0; i < item_count; ++i) {
    const uint32_t item = b.U32(item_list + i * 4);
    if (b.U32(item) != 3 || !(b.U32(item + 4) & 0x80) || !b.U32(item + 60) ||
        b.U32(item + 32) != FloatBits(591.1f) || b.U32(item + 36) != FloatBits(418.05f)) {
      continue;
    }
    const uint32_t hints = code.Start();
    for (const auto& [name, values] :
         {std::pair<const char*, std::vector<const char*>>{"displayOptions", {"$XerengeQuitHint", "$Continue"}},
          std::pair<const char*, std::vector<const char*>>{"XBoxIcons", {"B-Button", "A-Button"}}}) {
      code.Push({c.String(name), c.Int(0)});
      code.Op(0x42);  // InitArray
      code.Op(0x1D);  // SetVariable
      for (size_t v = 0; v < values.size(); ++v) {
        code.Push({c.String(name)});
        code.Op(0x1C);
        code.Push({c.Int(int32_t(v)), c.String(values[v])});
        code.Op(0x4F);  // SetMember
      }
    }
    code.Op(0x00);
    code.Finish();
    b.Set32(b.U32(b.U32(item + 60) + 4) + 8, hints);
    done = true;
  }
  if (!done) {
    REXLOG_WARN("menu: main menu movie: no button hints at 591.1, 418.05");
    return false;
  }
  b.Align(16);
  out_constants = c.Write();
  return true;
}

// Every PushData's and ConstantPool's list in the movie: where a constant
// stands in one, the other in its place.
void SwapConstant(Blob& b, uint32_t from, uint32_t to) {
  const uint32_t size = uint32_t(b.bytes.size());
  for (uint32_t op = 0; op + 12 < size; ++op) {
    if (b.bytes[op] != 0x96 && b.bytes[op] != 0x88) {
      continue;
    }
    const uint32_t at = (op + 4) & ~3u;
    bool padded = true;
    for (uint32_t p = op + 1; p < at; ++p) {
      padded = padded && b.bytes[p] == 0;
    }
    const uint32_t count = b.U32(at), list = b.U32(at + 4);
    if (!padded || count == 0 || count > 256 || (list & 3) || list <= at || list + count * 4 > size) {
      continue;
    }
    for (uint32_t i = 0; i < count; ++i) {
      if (b.U32(list + i * 4) == from) {
        b.Set32(list + i * 4, to);
      }
    }
  }
}

// The aligned operand of the op at `op`, and a PushData's (count, list) there.
uint32_t Operand(uint32_t op) { return (op + 4) & ~3u; }

bool FixPreviewFallThrough(Blob& b, Constants& c) {
  const uint32_t size = uint32_t(b.bytes.size());
  // The branch into the check: its target right after a CallMethod and Pop
  // (Preview Track's call), and the check - a PushData of true, then its
  // BranchIfTrue past the switch's call - within the next few dozen bytes.
  for (uint32_t op = 0; op + 8 < size; ++op) {
    if (b.bytes[op] != 0x9D) {
      continue;
    }
    const uint32_t operand = Operand(op);
    const uint32_t check = operand + 4 + b.U32(operand);
    if (check < 2 || check + 0x40 > size || b.bytes[check - 2] != 0x52 || b.bytes[check - 1] != 0x17) {
      continue;
    }
    // Equals2, Not, BranchIfTrue; the PushData just before them pushes true.
    uint32_t push_true = 0, skip = 0;
    for (uint32_t at = check + 12; at + 3 < check + 0x40; ++at) {
      if (b.bytes[at] == 0x49 && b.bytes[at + 1] == 0x12 && b.bytes[at + 2] == 0x9D) {
        for (uint32_t push = at - 12; push < at; ++push) {
          if (b.bytes[push] == 0x96 && Operand(push) + 8 == at && b.U32(Operand(push)) == 1) {
            push_true = b.U32(Operand(push) + 4);
          }
        }
        skip = at + 2;
        break;
      }
    }
    if (!push_true || !skip) {
      continue;
    }
    // The switch's branch to the call itself; the check, which only the fall
    // through now comes to, asks for the opposite, so it skips the call.
    b.Set32(operand, Operand(skip) + 4 - (operand + 4));
    b.Set32(push_true, c.Bool(false));
    return true;
  }
  return false;
}

// changeCustomPlaylist (removeCurrentList, a click, SetNextPlaylist,
// setUpTracks) sets the screen's title again too: the title was set once, on
// entering, and the music folder's page in it (custom_music.cpp's
// [MY MUSIC 3/35]) did not follow LB/RB. The click's code - from the PushData
// naming SfxEnum to its CallMethod and Pop - becomes gStdTitle.screen.setText
// and a branch over what is left of it: the function keeps its size (a branch
// out of a function's body ends the function here), and loses the click.
bool RetitleOnSwitch(Blob& b, Constants& c) {
  const uint32_t set_next = c.String("SetNextPlaylist");
  const uint32_t sfx = c.String("SfxEnum");
  const uint32_t size = uint32_t(b.bytes.size());
  // A PushData's items are constants or (type 8) entries of the ConstantPool
  // set up before it - the class code's are.
  std::vector<uint32_t> pool;
  const auto items_hold = [&](uint32_t at, uint32_t wanted) {
    const uint32_t count = b.U32(Operand(at)), list = b.U32(Operand(at) + 4);
    for (uint32_t i = 0; i < count && i < 16 && list + i * 4 + 4 <= size; ++i) {
      uint32_t item = b.U32(list + i * 4);
      if (item < c.entries.size() && c.entries[item].type == 8 && c.entries[item].value < pool.size()) {
        item = pool[c.entries[item].value];
      }
      if (item == wanted) {
        return true;
      }
    }
    return false;
  };
  for (uint32_t op = 0; op + 40 < size; ++op) {
    if (b.bytes[op] == 0x88) {
      const uint32_t count = b.U32(Operand(op)), list = b.U32(Operand(op) + 4);
      if (count > 0 && count < 4096 && !(list & 3) && list + count * 4 <= size) {
        pool.assign(count, 0);
        for (uint32_t i = 0; i < count; ++i) pool[i] = b.U32(list + i * 4);
      }
      continue;
    }
    if (b.bytes[op] != 0x8E) {
      continue;
    }
    const uint32_t size_at = Operand(op) + 16;
    const uint32_t body = size_at + 12, body_size = b.U32(size_at);
    if (body_size < 16 || body_size > 256 || body + body_size > size) {
      continue;
    }
    bool holds = false;
    uint32_t click = 0;
    for (uint32_t at = body; at < body + body_size; ++at) {
      if (b.bytes[at] == 0x96) {
        holds = holds || items_hold(at, set_next);
        if (!click && items_hold(at, sfx)) {
          click = at;
        }
      }
    }
    if (!holds || !click) {
      continue;
    }
    uint32_t end = 0;
    for (uint32_t at = click; at + 1 < body + body_size; ++at) {
      if (b.bytes[at] == 0x52 && b.bytes[at + 1] == 0x17) {
        end = at + 2;
        break;
      }
    }
    if (!end) {
      continue;
    }
    // The PushData lists, after the movie's data.
    const auto list = [&](std::vector<uint32_t> items) {
      b.Align(4);
      const uint32_t at = b.Here();
      for (uint32_t item : items) b.Put32(item);
      return at;
    };
    struct Push {
      uint32_t count, list;
    };
    const std::vector<Push> pushes = {
        // The subtitle's key with the time after it (GetTimer, the op - the movies'
        // getTimer() was undefined): a text field set to the
        // string it already holds is not looked up again, and the page in it
        // would stay as it was (xerenge_menu.cpp takes the key by its start).
        {1, list({c.String("$FEDRIVERDETEATRAX")})},
        {4, list({c.String("$FESONGMANAGER"), c.Int(2), c.Value(4, 2), c.String("gStdTitle")})},
        {1, list({c.String("screen")})},
        {1, list({c.String("setText")})}};
    // After each PushData: GetTimer and Add2; GetMember; GetMember; CallMethod.
    const std::vector<std::vector<uint8_t>> after = {{0x34, 0x47}, {0x4E}, {0x4E}, {0x52}};
    // How far it goes, before anything is written: past the click's end, it
    // would write over setUpTracks.
    {
      uint32_t q = click;
      for (size_t i = 0; i < pushes.size(); ++i) {
        q = Operand(q) + 8 + uint32_t(after[i].size());
      }
      if (q + 1 > end) {
        REXLOG_WARN("menu: song manager movie: the title's code does not fit in the click's");
        return false;
      }
    }
    uint32_t p = click;
    const auto put_push = [&](const Push& push) {
      b.bytes[p] = 0x96;
      const uint32_t operand = Operand(p);
      for (uint32_t q = p + 1; q < operand; ++q) b.bytes[q] = 0;
      b.Set32(operand, push.count);
      b.Set32(operand + 4, push.list);
      p = operand + 8;
    };
    for (size_t i = 0; i < pushes.size(); ++i) {
      put_push(pushes[i]);
      for (uint8_t op_after : after[i]) b.bytes[p++] = op_after;
    }
    b.bytes[p++] = 0x17;  // Pop
    // A branch over the rest of the click's code, or Padding where no branch fits.
    if (Operand(p) + 4 <= end) {
      b.bytes[p] = 0x99;
      const uint32_t operand = Operand(p);
      for (uint32_t q = p + 1; q < operand; ++q) b.bytes[q] = 0;
      b.Set32(operand, end - (operand + 4));
      p = operand + 4;
    }
    if (p > end) {
      REXLOG_WARN("menu: song manager movie: the title's code does not fit in the click's");
      return false;
    }
    for (; p < end; ++p) {
      b.bytes[p] = 0xFF;
    }
    return true;
  }
  return false;
}

// Driver Details > Song Manager (custom_music.cpp): its playlist switch shown
// (by HasCustomPlaylists) on LT, and the playing order on RT in place of LT.
// What the screen's input handler compares the key with, INPUTCODE.CYCLESONG
// (LT) and INPUTCODE.XBUTTON, becomes RTRIGGER and LTRIGGER; the icons follow
// (left1 is LT's, right1 RT's); the switch's word (PLAYLIST) is
// left out.
bool RebuildSongManager(const uint8_t* data, size_t data_size, const uint8_t* constants,
                        size_t constants_size, Blob& out_data, Blob& out_constants) {
  Constants c;
  if (!c.Read(constants, constants_size)) {
    return false;
  }
  Blob& b = out_data;
  b.bytes.assign(data, data + data_size);
  const uint32_t root = c.root;
  if (b.U32(root) != 9 || b.U32(root + 4) != 0x09876543) {
    return false;
  }
  SwapConstant(b, c.String("CYCLESONG"), c.String("RTRIGGER"));
  SwapConstant(b, c.String("XBUTTON"), c.String("LTRIGGER"));
  SwapConstant(b, c.String("left1"), c.String("right1"));
  SwapConstant(b, c.String("X-button"), c.String("left1"));
  // The input handler's A (Preview Track) falls through into the switch's
  // code (no branch after it - unseen on a console, where the switch never
  // showed): A switched the list. The switch's own branch is sent past the
  // check that begins that code, and the check made one that always fails.
  if (!FixPreviewFallThrough(b, c)) {
    REXLOG_WARN("menu: song manager movie: Preview Track's fall-through not found");
  }
  // The natives as the game names them: the movie asks for HasCustomPlayLists
  // and SetNextPlayList, which no one answers, and the switch never showed.
  SwapConstant(b, c.String("HasCustomPlayLists"), c.String("HasCustomPlaylists"));
  SwapConstant(b, c.String("SetNextPlayList"), c.String("SetNextPlaylist"));
  if (!RetitleOnSwitch(b, c)) {
    REXLOG_WARN("menu: song manager movie: changeCustomPlaylist not found");
  }
  // The switch's word, PLAYLIST (, which the online
  // menus share): a key of ours in its place, which answers spaces as wide -
  // the icon alone, where the movie's layout by the word's width puts it.
  bool blanked = false;
  for (auto& entry : c.entries) {
    if (entry.type == 1 && entry.text == "$OnlinePlayListSelectTitle") {
      entry.text = "$XerengeBlankWord";
      blanked = true;
    }
  }
  const char* const kWord = "$OnlinePlayListSelectTitle";
  for (size_t at = 0; at + std::strlen(kWord) < b.bytes.size(); ++at) {
    if (std::memcmp(&b.bytes[at], kWord, std::strlen(kWord) + 1) == 0) {
      std::memcpy(&b.bytes[at], "$XerengeBlankWord", 18);
      blanked = true;
    }
  }
  if (!blanked) {
    REXLOG_WARN("menu: song manager movie: its PLAYLIST key not found");
  }
  b.Align(16);
  out_constants = c.Write();
  return true;
}

// The constant file's size: up to the end of its last string.
size_t ConstantsSize(const uint8_t* raw) {
  Blob header;
  header.bytes.assign(raw, raw + 32);
  const uint32_t count = header.U32(24);
  size_t end = 32 + size_t(count) * 8;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* entry = raw + 32 + i * 8;
    const uint32_t type = uint32_t(entry[0]) << 24 | uint32_t(entry[1]) << 16 | uint32_t(entry[2]) << 8 | entry[3];
    if (type == 1) {
      const uint32_t at = uint32_t(entry[4]) << 24 | uint32_t(entry[5]) << 16 | uint32_t(entry[6]) << 8 | entry[7];
      end = std::max(end, at + std::strlen(reinterpret_cast<const char*>(raw + at)) + 1);
    }
  }
  return end;
}

uint32_t ToGuest(uint8_t* base, const Blob& blob) {
  const uint32_t at = REX_KERNEL_MEMORY()->SystemHeapAlloc(uint32_t(blob.bytes.size()), 0x10);
  std::memcpy(xerenge::GuestPointer(base, at), blob.bytes.data(), blob.bytes.size());
  return at;
}

}  // namespace

REX_HOOK_RAW(sub_824277A0) {
  const uint32_t data = ctx.r4.u32;
  const uint32_t constants = ctx.r5.u32;
  if (data && constants > data && constants - data <= 0x40000) {
    const uint8_t* raw_data = xerenge::GuestPointer(base, data);
    const size_t data_size = constants - data;
    // Driver Details > Settings, or the race's pause menu's Options.
    const bool settings = Holds(raw_data, data_size, "__Packages.DriverDetailsSettingsClass");
    const bool pause = !settings && Holds(raw_data, data_size, "__Packages.PauseOptionsMenu");
    // The main menu: Quit B beside Continue A.
    if (!settings && !pause && Holds(raw_data, data_size, "__Packages.MainMenu")) {
      const uint8_t* raw_constants = xerenge::GuestPointer(base, constants);
      Blob new_data, new_constants;
      if (RebuildMainMenu(raw_data, data_size, raw_constants, ConstantsSize(raw_constants), new_data,
                          new_constants)) {
        ctx.r4.u64 = ToGuest(base, new_data);
        ctx.r5.u64 = ToGuest(base, new_constants);
        REXLOG_INFO("menu: main menu movie rebuilt");
      }
    }
    // Driver Details > Song Manager: LT switches to the music folder, RT the order.
    const uint8_t* song_constants = xerenge::GuestPointer(base, constants);
    if (!settings && !pause && Holds(song_constants, ConstantsSize(song_constants), "togglePlayOrder") &&
        Holds(song_constants, ConstantsSize(song_constants), "changeCustomPlaylist")) {
      const uint8_t* raw_constants = song_constants;
      Blob new_data, new_constants;
      if (RebuildSongManager(raw_data, data_size, raw_constants, ConstantsSize(raw_constants), new_data,
                             new_constants)) {
        ctx.r4.u64 = ToGuest(base, new_data);
        ctx.r5.u64 = ToGuest(base, new_constants);
        REXLOG_INFO("menu: song manager movie rebuilt");
        xerenge::music::Rescan(base);
      }
    }
    // Driver Details > Stats's movie, asked for by that button: the achievements.
    if (!settings && !pause && xerenge::settings::CurrentPage() == xerenge::settings::kAchievements &&
        Holds(raw_data, data_size, "__Packages.DriverDetailsStats")) {
      const uint8_t* raw_constants = xerenge::GuestPointer(base, constants);
      Blob new_data, new_constants;
      if (RebuildAchievements(raw_data, data_size, raw_constants, ConstantsSize(raw_constants), new_data,
                              new_constants)) {
        ctx.r4.u64 = ToGuest(base, new_data);
        ctx.r5.u64 = ToGuest(base, new_constants);
        REXLOG_INFO("menu: achievements movie rebuilt: data {:X} -> {:X} bytes", data_size,
                    new_data.bytes.size());
      }
    }
    // Driver Details > Player File: the ACHIEVEMENTS button.
    if (!settings && !pause && Holds(raw_data, data_size, "DriverDetailsProfile") &&
        Holds(raw_data, data_size, "new_btn")) {
      const uint8_t* raw_constants = xerenge::GuestPointer(base, constants);
      Blob new_data, new_constants;
      if (RebuildProfile(raw_data, data_size, raw_constants, ConstantsSize(raw_constants), new_data,
                         new_constants)) {
        ctx.r4.u64 = ToGuest(base, new_data);
        ctx.r5.u64 = ToGuest(base, new_constants);
        REXLOG_INFO("menu: player file movie rebuilt: data {:X} -> {:X} bytes", data_size,
                    new_data.bytes.size());
      }
    }
    if (settings || pause) {
      const uint8_t* raw_constants = xerenge::GuestPointer(base, constants);
      Blob new_data, new_constants;
      // The American release (a byte of its image that differs from the European
      // one's: 0 there, 1 here) has one language.
      xerenge::settings::SetSingleLanguage(*xerenge::GuestPointer(base, 0x825F7632) == 0);
      if (Rebuild(raw_data, data_size, raw_constants, ConstantsSize(raw_constants), pause, new_data, new_constants)) {
        // Kept for as long as the movie may run: a screen's worth, not freed.
        ctx.r4.u64 = ToGuest(base, new_data);
        ctx.r5.u64 = ToGuest(base, new_constants);
        REXLOG_INFO("menu: {} movie rebuilt: data {:X} -> {:X} bytes, constants {:X} bytes, at {:08X}/{:08X}",
                    pause ? "pause options" : "settings", data_size, new_data.bytes.size(),
                    new_constants.bytes.size(), ctx.r4.u32, ctx.r5.u32);
        // XERENGE_DUMP_MENU_MOVIE=<file>: the rebuilt blobs, data then constants, for tools/aptdump.
        if (const char* path = std::getenv("XERENGE_DUMP_MENU_MOVIE")) {
          if (FILE* file = std::fopen(path, "wb")) {
            std::fwrite(new_data.bytes.data(), 1, new_data.bytes.size(), file);
            std::fwrite(new_constants.bytes.data(), 1, new_constants.bytes.size(), file);
            std::fclose(file);
          }
        }
      }
    }
  }
  __imp__sub_824277A0(ctx, base);
}
