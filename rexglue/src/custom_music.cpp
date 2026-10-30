// Your own music in place of EA Trax: Driver Details > Song Manager switches
// between the game's 41 songs and the files of a music folder (music/ beside
// the game, or music_dir in xerenge.conf), and the races play the chosen ones.
//
// The game was made for this - the Xbox 360's own playlists - and shipped
// with it switched off: the screen's movie has the switch (playlist_option,
// shown when the native HasCustomPlaylists says so, X calling SetNextPlaylist
// and building the list again), but HasCustomPlaylists (sub_82103ED8) always
// answers no and SetNextPlaylist (sub_8212BB98) does nothing. Here they do:
//   - the soundtrack's object (at 0x82A52568) holds the songs as a table of 41
//     (artist, title, album) string pointers at +236 and their count at +732,
//     which the list, the race's "now playing" and the playlist are made from;
//     switching puts the folder's songs there, or the game's back;
//   - a song is played from GAME:\eatrax\NN.wma; while the folder's songs are
//     chosen, XMP reads song NN of the folder instead (rex::kernel::xam's song
//     path resolver), through a mount of the folder at xmusic:.
// The movie's side (settings_movie.cpp): LT switches, RT changes the order.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <rex/filesystem/devices/host_path_device.h>
#include <rex/hook.h>
#include <rex/kernel/xam/xmp.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>

#include "custom_music.h"
#include "guest_memory.h"
#include "mods.h"
#include "xerenge_settings.h"

extern "C" void __imp__sub_821EBDF0(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_821EBE28(PPCContext& __restrict, uint8_t*);

namespace {

constexpr uint32_t kSoundtrack = 0x82A52568;  // the soundtrack's object
constexpr uint32_t kTable = 236;              // in its data: (artist, title, album) x 41
constexpr uint32_t kCount = 732;              // in its data: how many of them
constexpr uint32_t kSlots = (kCount - kTable) / 12;

struct Track {
  std::filesystem::path file;
  std::string title, artist, album;
};

std::mutex g_mutex;
bool g_scanned = false;
bool g_mounted = false;
std::filesystem::path g_dir;
std::vector<Track> g_tracks;
bool g_custom = false;   // the folder's songs chosen
bool g_applied = false;  // the table holds what g_custom says
uint32_t g_saved_data = 0;
uint8_t g_saved_table[kSlots * 12] = {};
uint32_t g_saved_count = 0;
std::vector<uint32_t> g_strings;  // ours in guest memory: artist, title, album per track
bool g_utf16 = true;

uint32_t Load32(uint8_t* base, uint32_t at) {
  uint32_t raw = 0;
  std::memcpy(&raw, xerenge::GuestPointer(base, at), 4);
  return __builtin_bswap32(raw);
}

void Store32(uint8_t* base, uint32_t at, uint32_t value) {
  const uint32_t raw = __builtin_bswap32(value);
  std::memcpy(xerenge::GuestPointer(base, at), &raw, 4);
}

std::string Lower(std::string text) {
  for (auto& c : text) {
    c = char(std::tolower(static_cast<unsigned char>(c)));
  }
  return text;
}

// The soundtrack's songs: its object's data, or - before the object is set,
// when the menus' first song is already playing and its names are read - the
// data's own place, once the game has filled it.
constexpr uint32_t kSoundtrackData = 0x8268748C;

uint32_t SoundtrackData(uint8_t* base) {
  if (const uint32_t soundtrack = Load32(base, kSoundtrack)) {
    return Load32(base, soundtrack);
  }
  return Load32(base, kSoundtrackData + kCount) != 0 ? kSoundtrackData : 0;
}

// --- a song's tags ----------------------------------------------------------

struct Tags {
  std::string title, artist, album;
};

void PutUtf8(std::string& out, uint32_t point) {
  if (point < 0x80) {
    out += char(point);
  } else if (point < 0x800) {
    out += char(0xC0 | (point >> 6));
    out += char(0x80 | (point & 0x3F));
  } else if (point < 0x10000) {
    out += char(0xE0 | (point >> 12));
    out += char(0x80 | ((point >> 6) & 0x3F));
    out += char(0x80 | (point & 0x3F));
  } else {
    out += char(0xF0 | (point >> 18));
    out += char(0x80 | ((point >> 12) & 0x3F));
    out += char(0x80 | ((point >> 6) & 0x3F));
    out += char(0x80 | (point & 0x3F));
  }
}

std::string FromUtf16(const uint8_t* data, size_t size, bool big_endian) {
  std::string out;
  for (size_t i = 0; i + 1 < size; i += 2) {
    uint32_t unit = big_endian ? (data[i] << 8 | data[i + 1]) : (data[i + 1] << 8 | data[i]);
    if (unit == 0) {
      break;
    }
    if (unit >= 0xD800 && unit < 0xDC00 && i + 3 < size) {
      const uint32_t low = big_endian ? (data[i + 2] << 8 | data[i + 3]) : (data[i + 3] << 8 | data[i + 2]);
      unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
      i += 2;
    }
    PutUtf8(out, unit);
  }
  return out;
}

std::string Latin1(const uint8_t* data, size_t size) {
  std::string out;
  for (size_t i = 0; i < size && data[i]; ++i) {
    PutUtf8(out, data[i]);
  }
  return out;
}

std::string TrimText(std::string text) {
  while (!text.empty() && (text.back() == ' ' || text.back() == '\0')) {
    text.pop_back();
  }
  return text;
}

// An ID3v2 text frame's text, by its encoding byte.
std::string Id3Text(const uint8_t* data, size_t size) {
  if (size < 2) {
    return {};
  }
  const uint8_t encoding = data[0];
  ++data;
  --size;
  switch (encoding) {
    case 1:  // UTF-16 with a byte order mark
      if (size >= 2 && data[0] == 0xFE && data[1] == 0xFF) {
        return FromUtf16(data + 2, size - 2, true);
      }
      return FromUtf16(data + (size >= 2 && data[0] == 0xFF ? 2 : 0), size - 2, false);
    case 2:
      return FromUtf16(data, size, true);
    case 3:
      return std::string(reinterpret_cast<const char*>(data), strnlen(reinterpret_cast<const char*>(data), size));
    default:
      return Latin1(data, size);
  }
}

Tags ReadMp3Tags(const std::vector<uint8_t>& f) {
  Tags tags;
  if (f.size() > 10 && std::memcmp(f.data(), "ID3", 3) == 0) {
    const uint8_t version = f[3];
    const size_t end = std::min(f.size(), size_t(10 + ((f[6] & 0x7F) << 21 | (f[7] & 0x7F) << 14 |
                                                       (f[8] & 0x7F) << 7 | (f[9] & 0x7F))));
    size_t at = 10;
    if (f[5] & 0x40 && at + 4 <= end) {  // an extended header, skipped
      const size_t ext = version == 4 ? ((f[at] & 0x7F) << 21 | (f[at + 1] & 0x7F) << 14 |
                                         (f[at + 2] & 0x7F) << 7 | (f[at + 3] & 0x7F))
                                      : (f[at] << 24 | f[at + 1] << 16 | f[at + 2] << 8 | f[at + 3]) + 4;
      at += ext;
    }
    while (version >= 3 && at + 10 <= end && f[at]) {
      const std::string id(reinterpret_cast<const char*>(&f[at]), 4);
      const size_t size = version == 4 ? ((f[at + 4] & 0x7F) << 21 | (f[at + 5] & 0x7F) << 14 |
                                          (f[at + 6] & 0x7F) << 7 | (f[at + 7] & 0x7F))
                                       : (size_t(f[at + 4]) << 24 | f[at + 5] << 16 | f[at + 6] << 8 | f[at + 7]);
      at += 10;
      if (at + size > end) {
        break;
      }
      if (id == "TIT2") tags.title = Id3Text(&f[at], size);
      if (id == "TPE1") tags.artist = Id3Text(&f[at], size);
      if (id == "TALB") tags.album = Id3Text(&f[at], size);
      at += size;
    }
  }
  // ID3v1 at the end, for what v2 did not have.
  if (f.size() >= 128 && std::memcmp(&f[f.size() - 128], "TAG", 3) == 0) {
    const uint8_t* v1 = &f[f.size() - 128];
    if (tags.title.empty()) tags.title = TrimText(Latin1(v1 + 3, 30));
    if (tags.artist.empty()) tags.artist = TrimText(Latin1(v1 + 33, 30));
    if (tags.album.empty()) tags.album = TrimText(Latin1(v1 + 63, 30));
  }
  return tags;
}

// ASF (WMA): the Content Description object (title, author) and the Extended
// Content Description's WM/AlbumTitle, among the header's objects.
Tags ReadWmaTags(const std::vector<uint8_t>& f) {
  static const uint8_t kHeader[16] = {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
                                      0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
  static const uint8_t kContent[16] = {0x33, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
                                       0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
  static const uint8_t kExtended[16] = {0x40, 0xA4, 0xD0, 0xD2, 0x07, 0xE3, 0xD2, 0x11,
                                        0x97, 0xF0, 0x00, 0xA0, 0xC9, 0x5E, 0xA8, 0x50};
  Tags tags;
  const auto u16 = [&](size_t at) { return size_t(f[at] | f[at + 1] << 8); };
  const auto u64 = [&](size_t at) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = v << 8 | f[at + i];
    return v;
  };
  if (f.size() < 30 || std::memcmp(f.data(), kHeader, 16) != 0) {
    return tags;
  }
  const size_t header_end = std::min<size_t>(f.size(), u64(16));
  for (size_t at = 30; at + 24 <= header_end;) {
    const size_t size = size_t(u64(at + 16));
    if (size < 24 || at + size > header_end) {
      break;
    }
    if (std::memcmp(&f[at], kContent, 16) == 0 && size >= 34) {
      const size_t title = u16(at + 24), author = u16(at + 26);
      size_t text = at + 34;
      if (text + title + author <= at + size) {
        tags.title = FromUtf16(&f[text], title, false);
        tags.artist = FromUtf16(&f[text + title], author, false);
      }
    } else if (std::memcmp(&f[at], kExtended, 16) == 0 && size >= 26) {
      size_t p = at + 26;
      for (size_t n = u16(at + 24); n > 0 && p + 2 <= at + size; --n) {
        const size_t name_len = u16(p);
        if (p + 2 + name_len + 4 > at + size) break;
        const std::string name = FromUtf16(&f[p + 2], name_len, false);
        const size_t type = u16(p + 2 + name_len), value_len = u16(p + 4 + name_len);
        const size_t value = p + 6 + name_len;
        if (value + value_len > at + size) break;
        if (type == 0 && name == "WM/AlbumTitle") tags.album = FromUtf16(&f[value], value_len, false);
        p = value + value_len;
      }
    }
    at += size;
  }
  return tags;
}

// The start of a file, where its tags are (and its end, for ID3v1).
Tags ReadTags(const std::filesystem::path& file) {
  std::vector<uint8_t> data;
  if (FILE* in = std::fopen(file.string().c_str(), "rb")) {
    std::fseek(in, 0, SEEK_END);
    const long size = std::ftell(in);
    std::fseek(in, 0, SEEK_SET);
    constexpr long kMax = 4 << 20;  // tags with a large picture still fit
    data.resize(size_t(std::min(size, kMax)));
    data.resize(std::fread(data.data(), 1, data.size(), in));
    if (size > kMax && size >= 128) {  // ID3v1 from the real end
      std::vector<uint8_t> tail(128);
      std::fseek(in, size - 128, SEEK_SET);
      if (std::fread(tail.data(), 1, 128, in) == 128 && std::memcmp(tail.data(), "TAG", 3) == 0) {
        data.insert(data.end(), tail.begin(), tail.end());
      }
    }
    std::fclose(in);
  }
  return Lower(file.extension().string()) == ".wma" ? ReadWmaTags(data) : ReadMp3Tags(data);
}

// A localisation mod's fonts, loaded whatever the language once a mod has
// them (mods.cpp), hold Cyrillic: with them, the names go as they are.
bool FontsHaveCyrillic() {
  return xerenge::mods::FontsHaveModLetters();
}

// The game's fonts hold Latin-1 only (language/fonts/*.bin): Cyrillic is
// written in Latin letters, the rest of what they lack as '?' - unless a
// localisation mod's fonts are in use.
std::string ForGameFont(const std::vector<uint32_t>& points) {
  if (FontsHaveCyrillic()) {
    std::string out;
    for (uint32_t point : points) PutUtf8(out, point);
    return out;
  }
  static const char* const kCyrillic[64] = {
      "A", "B", "V", "G", "D", "E", "Zh", "Z", "I", "Y", "K", "L", "M", "N", "O", "P",
      "R", "S", "T", "U", "F", "Kh", "Ts", "Ch", "Sh", "Shch", "", "Y", "", "E", "Yu", "Ya",
      "a", "b", "v", "g", "d", "e", "zh", "z", "i", "y", "k", "l", "m", "n", "o", "p",
      "r", "s", "t", "u", "f", "kh", "ts", "ch", "sh", "shch", "", "y", "", "e", "yu", "ya"};
  std::string out;
  for (uint32_t point : points) {
    if (point >= 0x410 && point <= 0x44F) {
      out += kCyrillic[point - 0x410];
    } else if (point == 0x401) {
      out += "Yo";
    } else if (point == 0x451) {
      out += "yo";
    } else if (point == 0x404 || point == 0x454) {
      out += point == 0x404 ? "Ye" : "ye";
    } else if (point == 0x406 || point == 0x456) {
      out += point == 0x406 ? "I" : "i";
    } else if (point == 0x407 || point == 0x457) {
      out += point == 0x407 ? "Yi" : "yi";
    } else if (point < 0x100) {
      PutUtf8(out, point);
    } else {
      out += '?';
    }
  }
  return out;
}

std::vector<uint32_t> CodePoints(const std::string& utf8) {
  std::vector<uint32_t> points;
  for (size_t i = 0; i < utf8.size();) {
    const uint8_t c = uint8_t(utf8[i]);
    const int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    uint32_t point = extra ? c & (0x3F >> extra) : c;
    for (int k = 1; k <= extra && i + k < utf8.size(); ++k) {
      point = point << 6 | (uint8_t(utf8[i + k]) & 0x3F);
    }
    points.push_back(point);
    i += size_t(extra) + 1;
  }
  return points;
}

// The folder's songs: the files XMP can play, named by their tags (ID3, ASF),
// or by the file's name - "Artist - Title" split into the two.
void Scan() {
  if (g_scanned) {
    return;
  }
  g_scanned = true;
  if (const char* env = std::getenv("XERENGE_MUSIC_DIR"); env && *env) {
    g_dir = env;
  } else if (const std::string conf = xerenge::settings::ConfString("music_dir", ""); !conf.empty()) {
    g_dir = conf;
  } else {
    g_dir = xerenge::settings::GameFolder() / "music";
  }
  std::error_code ec;
  std::vector<std::filesystem::path> files;
  const auto take = [&](const std::filesystem::directory_entry& entry) {
    const std::string ext = Lower(entry.path().extension().string());
    if (entry.is_regular_file(ec) && (ext == ".mp3" || ext == ".wma")) {
      files.push_back(entry.path());
    }
  };
  // recursive_scan = true in xerenge.conf: the folders inside it too (a
  // library sorted by artist and album).
  if (xerenge::settings::ConfString("recursive_scan", "false") == "true") {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             g_dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
      take(entry);
    }
  } else {
    for (const auto& entry : std::filesystem::directory_iterator(g_dir, ec)) {
      take(entry);
    }
  }
  std::sort(files.begin(), files.end());
  if (files.size() > kSlots) {
    REXLOG_WARN("music: {} songs in {}, the first {} of them taken", files.size(), g_dir.string(), kSlots);
    files.resize(kSlots);
  }
  for (const auto& file : files) {
    Track track;
    track.file = file;
    const std::string stem = file.stem().string();
    const size_t dash = stem.find(" - ");
    if (dash != std::string::npos) {
      track.artist = stem.substr(0, dash);
      track.title = stem.substr(dash + 3);
    } else {
      track.title = stem;
    }
    track.album = file.parent_path().filename().string();
    // The file's own tags, where it has them, over what its name says.
    const Tags tags = ReadTags(file);
    if (!tags.title.empty()) track.title = tags.title;
    if (!tags.artist.empty()) track.artist = tags.artist;
    if (!tags.album.empty()) track.album = tags.album;
    g_tracks.push_back(std::move(track));
  }
  REXLOG_INFO("music: {} song(s) in {}", g_tracks.size(), g_dir.string());
  if (g_tracks.empty()) {
    return;
  }
  g_custom = xerenge::settings::ConfString("music_source", "eatrax") == "custom";

  // The folder, for XMP to read from.
  if (g_mounted) {
    return;
  }
  g_mounted = true;
  auto device = std::make_unique<rex::filesystem::HostPathDevice>("\\Device\\XerengeMusic", g_dir, true);
  if (device->Initialize() && REX_KERNEL_FS()->RegisterDevice(std::move(device))) {
    REX_KERNEL_FS()->RegisterSymbolicLink("xmusic:", "\\Device\\XerengeMusic");
  }
  rex::kernel::xam::SetXmpSongPathResolver([](const std::string& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const std::string lower = Lower(path);
    const size_t at = lower.find("eatrax\\");
    if (!g_custom || g_tracks.empty() || at == std::string::npos || at + 9 > lower.size() ||
        !std::isdigit(static_cast<unsigned char>(lower[at + 7])) ||
        !std::isdigit(static_cast<unsigned char>(lower[at + 8]))) {
      return path;
    }
    const size_t number = size_t((lower[at + 7] - '0') * 10 + (lower[at + 8] - '0'));
    // Its path under the folder, subfolders and all (recursive_scan).
    std::string relative = g_tracks[number % g_tracks.size()].file.lexically_relative(g_dir).string();
    std::replace(relative.begin(), relative.end(), '/', '\\');
    return "xmusic:\\" + relative;
  });
}

// A string as the game's table holds them: big-endian UTF-16 (from the string
// table) or bytes, as the game's own entries are found to be.
uint32_t GuestString(uint8_t* base, const std::string& utf8) {
  const std::vector<uint32_t> points = CodePoints(ForGameFont(CodePoints(utf8)));
  std::vector<uint8_t> bytes;
  for (uint32_t point : points) {
    if (!g_utf16) {
      bytes.push_back(point < 0x80 ? uint8_t(point) : uint8_t('?'));
      continue;
    }
    const auto put = [&](uint32_t unit) {
      bytes.push_back(uint8_t(unit >> 8));
      bytes.push_back(uint8_t(unit));
    };
    if (point >= 0x10000) {
      point -= 0x10000;
      put(0xD800 | (point >> 10));
      put(0xDC00 | (point & 0x3FF));
    } else {
      put(point);
    }
  }
  bytes.insert(bytes.end(), g_utf16 ? 2 : 1, 0);
  const uint32_t at = REX_KERNEL_MEMORY()->SystemHeapAlloc(uint32_t(bytes.size()), 0x10);
  std::memcpy(xerenge::GuestPointer(base, at), bytes.data(), bytes.size());
  return at;
}

// The table as g_custom says: the folder's songs, or the game's.
void Apply(uint8_t* base) {
  const uint32_t data = SoundtrackData(base);
  if (!data || g_tracks.empty()) {
    return;
  }
  if (g_saved_data != data) {
    // The game's songs, as it made them: kept to put back.
    g_saved_data = data;
    std::memcpy(g_saved_table, xerenge::GuestPointer(base, data + kTable), sizeof(g_saved_table));
    g_saved_count = Load32(base, data + kCount);
    const uint32_t first = Load32(base, data + kTable);
    if (first) {
      const uint8_t* text = xerenge::GuestPointer(base, first);
      g_utf16 = text[0] == 0 && text[1] != 0;
    }
  }
  if (g_custom) {
    if (g_strings.empty()) {
      for (const Track& track : g_tracks) {
        // The table's order: artist, title, album (the list shows the title first).
        g_strings.push_back(GuestString(base, track.artist));
        g_strings.push_back(GuestString(base, track.title));
        g_strings.push_back(GuestString(base, track.album));
      }
    }
    for (uint32_t i = 0; i < g_tracks.size(); ++i) {
      for (uint32_t k = 0; k < 3; ++k) {
        Store32(base, data + kTable + i * 12 + k * 4, g_strings[i * 3 + k]);
      }
    }
    Store32(base, data + kCount, uint32_t(g_tracks.size()));
    REXLOG_INFO("music: the folder's {} song(s) put in the soundtrack at {:08X}", g_tracks.size(), data);
  } else {
    std::memcpy(xerenge::GuestPointer(base, data + kTable), g_saved_table, sizeof(g_saved_table));
    Store32(base, data + kCount, g_saved_count);
  }
  g_applied = true;
}

void EnsureApplied(uint8_t* base) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Scan();
  const uint32_t data = SoundtrackData(base);
  if (g_custom && (!g_applied || data != g_saved_data)) {
    Apply(base);
  }
}

}  // namespace

namespace xerenge::music {

void Rescan(uint8_t* base) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const bool had = !g_tracks.empty();
  g_scanned = false;
  g_tracks.clear();
  g_strings.clear();  // the old ones stay in guest memory, a few bytes a visit
  Scan();
  if (!g_tracks.empty()) {
    if (g_custom) {
      Apply(base);
    }
  } else if (had && g_saved_data) {
    // The folder emptied while its songs were chosen: the game's back.
    g_custom = false;
    std::memcpy(xerenge::GuestPointer(base, g_saved_data + kTable), g_saved_table, sizeof(g_saved_table));
    Store32(base, g_saved_data + kCount, g_saved_count);
  }
}

void Prepare(uint8_t* base) {
  EnsureApplied(base);
}

bool Custom() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_custom;
}

}  // namespace xerenge::music

// HasCustomPlaylists: songs in the folder. The two are the game's "answer no"
// and "do nothing", shared by many callers (one body each, folded together):
// only the natives' calls (by their return addresses) are ours.
extern "C" void __imp__sub_82103ED8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8212BB98(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_82103ED8) {
  if (uint32_t(ctx.lr) != 0x821C8D14) {
    __imp__sub_82103ED8(ctx, base);
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  Scan();
  ctx.r3.u64 = g_tracks.empty() ? 0 : 1;
}

// SetNextPlaylist: the folder's songs or the game's, the other of the two.
REX_HOOK_RAW(sub_8212BB98) {
  if (uint32_t(ctx.lr) != 0x821C8E80) {
    __imp__sub_8212BB98(ctx, base);
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  Scan();
  if (g_tracks.empty()) {
    return;
  }
  g_custom = !g_custom;
  Apply(base);
  xerenge::settings::SetConfString("music_source", g_custom ? "custom" : "eatrax");
  REXLOG_INFO("music: {} chosen", g_custom ? "the music folder" : "EA Trax");
}

// The song count and a song's strings, as the list and the race ask for them:
// the folder's songs in place first when they were chosen at an earlier start.
REX_HOOK_RAW(sub_821EBDF0) {
  EnsureApplied(base);
  __imp__sub_821EBDF0(ctx, base);
}

REX_HOOK_RAW(sub_821EBE28) {
  EnsureApplied(base);
  __imp__sub_821EBE28(ctx, base);
}

// Where the soundtrack's object is set (two setters): the folder's songs put in
// it at once, before the first song's names are read for the music player -
// the menus' first song otherwise showed the game's names over the folder's.
extern "C" void __imp__sub_821AAB28(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_821AAB88(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_821AAB28) {
  __imp__sub_821AAB28(ctx, base);
  EnsureApplied(base);
}

REX_HOOK_RAW(sub_821AAB88) {
  __imp__sub_821AAB88(ctx, base);
  EnsureApplied(base);
}
