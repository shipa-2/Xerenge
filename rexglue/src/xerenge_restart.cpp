// Starting the game again, for settings that take effect only at a start (the
// Settings screen's restart popup, xerenge_menu.cpp). The launcher beside the
// installed game is what reads xerenge.conf into the game's arguments and
// environment, so it is the launcher that is started: Burnout Revenge.exe on
// Windows, burnout_revenge elsewhere (installer/launcher.cpp). A
// copy run from a checkout (no launcher) starts its own executable again with
// the same arguments. On Android, BurnoutActivity schedules the package
// launcher with AlarmManager before this process ends. Then this one ends.
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fstream>
#include <spawn.h>
#include <unistd.h>

extern char** environ;
#endif

#if defined(__ANDROID__)
#include <jni.h>
#include <SDL3/SDL.h>
#endif

REXCVAR_DECLARE(std::string, game_data_root);

namespace {

std::filesystem::path InstallDir() {
  const std::string root = REXCVAR_GET(game_data_root);
  return root.empty() ? std::filesystem::path() : std::filesystem::path(root).parent_path();
}

}  // namespace

// Whether the game can start itself again here.
bool XerengeCanRestart() {
  return true;
}

void XerengeRestartGame() {
#if defined(__ANDROID__)
  // BurnoutActivity schedules the package's launcher with AlarmManager. The
  // new activity must not be started in this process only for _Exit to kill it.
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (!env || !activity) {
    return;
  }
  jclass activity_class = env->GetObjectClass(activity);
  jmethodID restart = env->GetMethodID(activity_class, "restartGame", "()V");
  if (!restart) {
    env->ExceptionClear();
    return;
  }
  env->CallVoidMethod(activity, restart);
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    return;
  }
  REXLOG_INFO("menu: Android will relaunch the game");
  std::_Exit(0);
#elif defined(_WIN32)
  const std::filesystem::path launcher = InstallDir() / "Burnout Revenge.exe";
  std::wstring command;
  if (std::filesystem::exists(launcher)) {
    command = L"\"" + launcher.wstring() + L"\"";
  } else {
    command = GetCommandLineW();
  }
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  const std::wstring dir = InstallDir().wstring();
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr,
                      dir.empty() ? nullptr : dir.c_str(), &startup, &process)) {
    REXLOG_ERROR("menu: cannot start the game again (error {})", GetLastError());
    return;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  REXLOG_INFO("menu: the game starts again");
  std::_Exit(0);
#else
  const std::filesystem::path launcher = InstallDir() / "burnout_revenge";
  std::vector<std::string> args;
  if (std::filesystem::exists(launcher)) {
    args.push_back(launcher.string());
  } else {
#if defined(__linux__)
    // A checkout's run: this executable and its arguments, as it was started.
    std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
    std::string arg;
    while (std::getline(cmdline, arg, '\0')) {
      args.push_back(arg);
    }
    if (!args.empty()) {
      std::error_code error;
      args[0] = std::filesystem::read_symlink("/proc/self/exe", error).string();
    }
    // Its libraries where this copy found them: run.sh gives their directory
    // relative to where it started, which the game has left since.
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
      const size_t at = line.find('/');
      if (at != std::string::npos && line.find("librexruntime") != std::string::npos) {
        const std::string dir = std::filesystem::path(line.substr(at)).parent_path().string();
        const char* old = std::getenv("LD_LIBRARY_PATH");
        setenv("LD_LIBRARY_PATH", (dir + (old && *old ? std::string(":") + old : "")).c_str(), 1);
        break;
      }
    }
#endif
  }
  if (args.empty() || args[0].empty()) {
    REXLOG_ERROR("menu: no launcher to start the game again with");
    return;
  }
  // Through a shell that waits a second first, for this copy to let go of the
  // window, the audio and the files; in a session of its own, so that it
  // outlives this one. posix_spawn rather than fork: this process has threads.
  std::vector<std::string> shell = {"/bin/sh", "-c", "sleep 1; exec \"$@\"", "sh"};
  shell.insert(shell.end(), args.begin(), args.end());
  std::vector<char*> argv;
  for (auto& a : shell) {
    argv.push_back(a.data());
  }
  argv.push_back(nullptr);
  posix_spawnattr_t attributes;
  posix_spawnattr_init(&attributes);
#if defined(POSIX_SPAWN_SETSID)
  posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
#endif
  pid_t child = 0;
  const int error = posix_spawn(&child, "/bin/sh", nullptr, &attributes, argv.data(), environ);
  posix_spawnattr_destroy(&attributes);
  if (error != 0) {
    REXLOG_ERROR("menu: cannot start the game again (error {})", error);
    return;
  }
  REXLOG_INFO("menu: the game starts again ({}, pid {})", args[0], child);
  std::_Exit(0);
#endif
}
