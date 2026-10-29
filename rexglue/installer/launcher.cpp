// The game's launcher, which the installer puts beside bin/ and the shortcuts
// point at: Burnout Revenge.exe on Windows, burnout_revenge elsewhere. It is
// what Steam is given (GitHub #26: on Windows a batch file in between lost the
// game for Steam Input), and what the game's Settings start to restart it
// (xerenge_restart.cpp) - so that the settings chosen there take effect.
//
// It reads xerenge.conf beside itself at every start - the settings the
// installer and the game's Settings write, KEY = VALUE lines, # for comments -
// turns them into the game's environment and arguments, and starts
// bin/burnout with them (its own arguments go after). On Windows it waits for
// the game and exits with its exit code; elsewhere it becomes the game (exec).
//
// Built on its own, with nothing but the C++ library:
//   c++ -std=c++17 -O2 launcher.cpp -o burnout_revenge
// (Windows: the installer's CMake builds it, as a GUI program.)
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

namespace {

std::string Trim(const std::string& text) {
  const size_t begin = text.find_first_not_of(" \t\r");
  if (begin == std::string::npos) {
    return {};
  }
  return text.substr(begin, text.find_last_not_of(" \t\r") - begin + 1);
}

std::string Lower(std::string text) {
  for (auto& c : text) {
    c = char(std::tolower(static_cast<unsigned char>(c)));
  }
  return text;
}

// xerenge.conf: KEY = VALUE, in the order written (the keybind_* lines are
// passed on in it). An empty value is no value.
struct Config {
  std::vector<std::pair<std::string, std::string>> lines;
  std::map<std::string, std::string> values;

  std::string Get(const std::string& key) const {
    const auto found = values.find(key);
    return found == values.end() ? std::string() : found->second;
  }
  bool Has(const std::string& key) const { return !Get(key).empty(); }
  bool Is(const std::string& key, const char* value) const { return Lower(Get(key)) == value; }
};

Config ReadConfig(const std::filesystem::path& path) {
  Config config;
  std::ifstream file(path);
  std::string raw;
  while (std::getline(file, raw)) {
    const std::string line = Trim(raw);
    if (line.empty() || line[0] == '#') {
      continue;
    }
    const size_t equals = line.find('=');
    if (equals == std::string::npos) {
      continue;
    }
    const std::string key = Trim(line.substr(0, equals));
    const std::string value = Trim(line.substr(equals + 1));
    if (!key.empty()) {
      config.lines.emplace_back(key, value);
      config.values[key] = value;
    }
  }
  return config;
}

// This launcher's own directory: where the game is installed.
std::filesystem::path OwnDirectory() {
#if defined(_WIN32)
  wchar_t self[MAX_PATH * 4];
  const DWORD length = GetModuleFileNameW(nullptr, self, DWORD(std::size(self)));
  return std::filesystem::path(std::wstring(self, length)).parent_path();
#elif defined(__APPLE__)
  char self[4096];
  uint32_t size = sizeof(self);
  if (_NSGetExecutablePath(self, &size) == 0) {
    std::error_code error;
    return std::filesystem::canonical(self, error).parent_path();
  }
  return std::filesystem::current_path();
#else
  std::error_code error;
  return std::filesystem::read_symlink("/proc/self/exe", error).parent_path();
#endif
}

#if defined(_WIN32)
std::wstring Widen(const std::string& text) {
  if (text.empty()) {
    return {};
  }
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
  std::wstring wide(size, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), wide.data(), size);
  return wide;
}

// One argument as CreateProcess's command line wants it.
std::wstring Quote(const std::wstring& arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) {
    return arg;
  }
  std::wstring quoted = L"\"";
  size_t backslashes = 0;
  for (const wchar_t c : arg) {
    if (c == L'\\') {
      ++backslashes;
      continue;
    }
    quoted.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
    backslashes = 0;
    quoted += c;
  }
  quoted.append(backslashes * 2, L'\\');
  return quoted + L"\"";
}

// What follows the program's own name on this launcher's command line.
std::wstring OwnArguments() {
  const wchar_t* line = GetCommandLineW();
  bool quoted = false;
  while (*line && (quoted || (*line != L' ' && *line != L'\t'))) {
    if (*line == L'"') {
      quoted = !quoted;
    }
    ++line;
  }
  while (*line == L' ' || *line == L'\t') {
    ++line;
  }
  return line;
}

void Env(const char* name, const std::string& value) {
  SetEnvironmentVariableW(Widen(name).c_str(), Widen(value).c_str());
}
#else
void Env(const char* name, const std::string& value) {
  setenv(name, value.c_str(), 1);
}
#endif

// The game's environment and arguments from the settings, as the batch file
// and the shell script the installer used to write set them.
std::vector<std::string> Prepare(const std::filesystem::path& dir, const Config& conf) {
  if (!conf.Is("bloom", "true")) Env("XERENGE_NO_BLOOM", "1");
  if (!conf.Is("motion_blur", "true")) Env("XERENGE_NO_MOTION_BLUR", "1");

  const std::string renderer = conf.Is("renderer", "xenos") ? "xenos" : "plume";
  if (renderer == "plume") {
    for (const char* name : {"XERENGE_D3D_TARGETS", "XERENGE_D3D_UI", "XERENGE_SKIP_LOGOS",
                             "XERENGE_REAL_SHADERS", "XERENGE_D3D_DRAWS", "XERENGE_SECONDARY_TICKS"}) {
      Env(name, "1");
    }
  }

  if (conf.Has("language")) Env("XERENGE_LANGUAGE", conf.Get("language"));
  if (conf.Is("async_present", "true")) Env("XERENGE_ASYNC_PRESENT", "1");
  if (conf.Is("early_submit", "false")) Env("XERENGE_ACQUIRE_FIRST", "1");
  if (conf.Is("packed_vertices", "false")) Env("XERENGE_FULL_VERTICES", "1");
  if (conf.Is("culling", "true")) Env("XERENGE_CULL", "1");
  if (conf.Is("reflection_split", "true")) Env("XERENGE_REFLECTION_SPLIT", "1");
  if (conf.Has("render_resolution")) Env("XERENGE_RENDER_RESOLUTION", conf.Get("render_resolution"));
  if (conf.Is("anisotropy", "off")) {
    Env("XERENGE_MAX_ANISOTROPY", "1");
  } else if (conf.Has("anisotropy")) {
    Env("XERENGE_MAX_ANISOTROPY", conf.Get("anisotropy"));
  }
  if (conf.Has("colour_grade")) Env("XERENGE_COLOUR_GRADE", conf.Get("colour_grade"));
  if (conf.Has("aa")) Env("XERENGE_AA", conf.Get("aa"));
  if (conf.Has("sharpen")) Env("XERENGE_SHARPEN", conf.Get("sharpen"));
  if (conf.Has("brightness")) Env("XERENGE_BRIGHTNESS", conf.Get("brightness"));
  if (conf.Has("shadow_lift")) Env("XERENGE_SHADOW_LIFT", conf.Get("shadow_lift"));
  if (conf.Has("power_mode")) Env("XERENGE_POWER_MODE", conf.Get("power_mode"));
  if (conf.Has("spin_lead_ms")) Env("XERENGE_SPIN_LEAD_MS", conf.Get("spin_lead_ms"));
  if (conf.Is("fps_counter", "true")) Env("XERENGE_FPS_SHOW", "1");
  if (conf.Has("prompts")) Env("XERENGE_PROMPTS", conf.Get("prompts"));
  if (conf.Is("renderdoc", "false")) Env("XERENGE_NO_RENDERDOC", "1");
  if (conf.Is("fps_30", "true")) Env("XERENGE_FPS_30", "1");
  if (conf.Is("frame_generation", "true")) Env("XERENGE_FRAME_GENERATION", "120");
  if (conf.Is("crash_interpolation", "false")) Env("XERENGE_CRASH_INTERPOLATION", "0");
  if (!conf.Is("aspect_16_9", "false")) Env("XERENGE_ASPECT_16_9", "1");
  if (conf.Has("menu_video")) Env("XERENGE_MENU_VIDEO", conf.Get("menu_video"));

  // Debug mode: the debug_* settings decide what is logged.
  std::string level = "info";
  std::vector<std::string> extra;
  if (conf.Is("debug", "true")) {
    level = conf.Has("debug_log_level") ? conf.Get("debug_log_level") : "debug";
    Env("XERENGE_DEBUG", "1");
    if (conf.Is("debug_gpu_trace", "true")) Env("XERENGE_GPU_TRACE", "1");
    if (conf.Is("debug_movie_trace", "true")) Env("XERENGE_MOVIE_TRACE", "1");
    if (conf.Is("debug_pipeline_log", "true")) Env("XERENGE_PIPELINE_LOG", "1");
    if (!conf.Is("debug_vertex_trace", "false")) Env("XERENGE_VERTEX_TRACE", "1");
    if (conf.Is("debug_constant_trace", "true")) Env("XERENGE_CONSTANT_TRACE", "1");
    if (conf.Has("debug_video_gpu")) Env("XERENGE_VIDEO_GPU", conf.Get("debug_video_gpu"));
    if (conf.Is("debug_noisy", "true")) extra.push_back("--log_noisy=true");
  }

  // Where the logs go.
  std::filesystem::path logs;
#if defined(_WIN32)
  if (const char* local = std::getenv("LOCALAPPDATA")) {
    logs = std::filesystem::path(local) / "xerenge-burnout";
  }
#elif defined(__APPLE__)
  if (const char* home = std::getenv("HOME")) {
    logs = std::filesystem::path(home) / "Library" / "Logs" / "xerenge-burnout";
  }
#else
  if (const char* state = std::getenv("XDG_STATE_HOME"); state && *state) {
    logs = std::filesystem::path(state) / "xerenge-burnout";
  } else if (const char* home = std::getenv("HOME")) {
    logs = std::filesystem::path(home) / ".local" / "state" / "xerenge-burnout";
  }
  // The window: native Wayland in a Wayland session (plume makes its Vulkan
  // surface through SDL, which knows Wayland), X11 otherwise and for xenos,
  // whose instance enables only the xcb surface; video_driver in the conf,
  // or SDL_VIDEODRIVER already set, decides instead.
  const char* driver = std::getenv("SDL_VIDEODRIVER");
  if (conf.Has("video_driver")) {
    Env("SDL_VIDEODRIVER", conf.Get("video_driver"));
  } else if (!driver || !*driver) {
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    Env("SDL_VIDEODRIVER", wayland && *wayland && renderer == "plume" ? "wayland" : "x11");
  }
  // The game's libraries beside it.
  const char* library_path = std::getenv("LD_LIBRARY_PATH");
  Env("LD_LIBRARY_PATH", (dir / "bin").string() + (library_path && *library_path ? std::string(":") + library_path : ""));
#endif
  // Portable (GitHub #42): the saves, the caches and the logs in the game's own
  // folder too, so it moves as a whole - an SD card from one machine to another.
  const bool portable = conf.Is("portable", "true");
  if (portable) {
    logs = dir / "logs";
  }
  if (conf.Has("log_path")) {
    logs = conf.Get("log_path");
  }
  std::error_code error;
  std::filesystem::create_directories(logs, error);

  std::vector<std::string> args = {(dir / "bin" / "burnout").string(),
                                   "--game_data_root", (dir / "game").string(),
                                   "--gpu_plugin", renderer,
                                   "--gpu_backend", renderer,
                                   "--no-vulkan_async_skip_incomplete_frames",
                                   "--log_level", level,
                                   "--log_file", (logs / "burnout.log").string(),
                                   "--log_max_file_size_mb", "32",
                                   "--log_max_files", "3"};
#if defined(_WIN32)
  args[0] += ".exe";
#endif
  if (portable) {
    args.insert(args.end(), {"--user_data_root", (dir / "userdata").string(), "--cache_root", (dir / "cache").string()});
  }

  // Fullscreen unless windowed; the keyboard as a pad with its keybind_* lines.
  args.push_back(conf.Is("windowed", "true") ? "--no-fullscreen" : "--fullscreen");
  if (conf.Is("keyboard", "true")) {
    args.push_back("--mnk_mode=true");
    for (const auto& [key, value] : conf.lines) {
      if (key.rfind("keybind_", 0) == 0 && !value.empty()) {
        args.push_back("--" + key + "=" + value);
      }
    }
  }
  args.insert(args.end(), extra.begin(), extra.end());

  if (conf.Has("gamertag")) {
    args.push_back("--gamertag=" + conf.Get("gamertag"));
  }
  // Online: a server address means that server and nothing local; otherwise this copy
  // runs a lobby of its own when local multiplayer is on; otherwise no online mode.
  if (conf.Has("lobby_server") || conf.Is("online", "true")) {
    args.push_back("--online");
    args.push_back("--online_fake_lobby=false");
    if (conf.Has("lobby_server")) {
      args.push_back("--lobby_server=" + conf.Get("lobby_server"));
    }
  }
  return args;
}

}  // namespace

#if defined(_WIN32)
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  const std::filesystem::path dir = OwnDirectory();
  const std::vector<std::string> args = Prepare(dir, ReadConfig(dir / "xerenge.conf"));

  std::wstring line;
  for (const auto& arg : args) {
    line += (line.empty() ? L"" : L" ") + Quote(Widen(arg));
  }
  if (const std::wstring own = OwnArguments(); !own.empty()) {
    line += L" " + own;
  }
  const std::wstring game = Widen(args[0]);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(game.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.wstring().c_str(),
                      &startup, &process)) {
    const std::wstring text = L"Cannot start " + game + L" (error " + std::to_wstring(GetLastError()) +
                              L"). Run the installer again to repair the installation.";
    MessageBoxW(nullptr, text.c_str(), L"Burnout Revenge", MB_OK | MB_ICONERROR);
    return 1;
  }
  CloseHandle(process.hThread);
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hProcess);
  return int(code);
}
#else
int main(int argc, char** argv) {
  const std::filesystem::path dir = OwnDirectory();
  const Config conf = ReadConfig(dir / "xerenge.conf");
  std::vector<std::string> args = Prepare(dir, conf);
  for (int i = 1; i < argc; ++i) {
    args.push_back(argv[i]);
  }
  // Started from a shortcut there is no terminal to show why a start failed
  // before the game's own log opened: what it prints goes to a file.
  if (!isatty(2)) {
    std::string log = "launcher.log";
    for (size_t i = 0; i + 1 < args.size(); ++i) {
      if (args[i] == "--log_file") {
        log = (std::filesystem::path(args[i + 1]).parent_path() / "launcher.log").string();
      }
    }
    if (std::freopen(log.c_str(), "w", stderr)) {
      dup2(fileno(stderr), 1);
    }
  }
  std::vector<char*> exec_args;
  for (auto& arg : args) {
    exec_args.push_back(arg.data());
  }
  exec_args.push_back(nullptr);
  if (chdir(dir.c_str()) != 0) {
    std::perror("chdir");
  }
  execv(exec_args[0], exec_args.data());
  std::fprintf(stderr, "cannot start %s\n", exec_args[0]);
  std::perror("execv");
  return 127;
}
#endif
