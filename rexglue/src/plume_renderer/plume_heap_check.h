#pragma once

// XERENGE_HEAP_CHECK=1 (Windows): the process heap validated at the renderer's
// checkpoints - each encode stage, the title's draws, the worker jobs - and the
// first checkpoint that finds it corrupt written to heap-check.txt beside the
// log, with the last one that found it whole. Something was corrupting the
// heap on the Xbox One a few seconds into the title screen, and a thread then
// crashing inside the heap with its lock held hung the whole game; the pair
// of checkpoints says between which two points of whose work it happened.
// The report is written through a handle opened while the heap was whole, as
// a corrupt heap can take nothing more from it.

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <rex/thread.h>

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace rex::plume_renderer {

inline void HeapCheck(const char* where) {
  static const bool enabled = [] {
    const char* v = std::getenv("XERENGE_HEAP_CHECK");
    return v && *v && *v != '0';
  }();
  if (!enabled) {
    return;
  }
  static HANDLE out = [] {
    wchar_t path[MAX_PATH] = L"heap-check.txt";
    if (const char* log = std::getenv("XERENGE_FREEZE_FILE")) {
      // Beside the freeze dump: LocalState on the Xbox.
      const char* slash = std::strrchr(log, '\\');
      const size_t dir = slash ? size_t(slash - log + 1) : 0;
      size_t n = 0;
      for (; n < dir && n < MAX_PATH - 16; ++n) {
        path[n] = wchar_t(log[n]);
      }
      const wchar_t* name = L"heap-check.txt";
      for (size_t i = 0; name[i] && n < MAX_PATH - 1; ++i) {
        path[n++] = name[i];
      }
      path[n] = 0;
    }
    return CreateFile2(path, GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS, nullptr);
  }();
  static std::atomic<const char*> last_whole{"(start)"};
  static std::atomic<bool> reported{false};
  if (reported.load(std::memory_order_relaxed)) {
    return;
  }
  if (HeapValidate(GetProcessHeap(), 0, nullptr)) {
    last_whole.store(where, std::memory_order_relaxed);
    rex::thread::SetDebugCheckpoint(where);
    return;
  }
  if (reported.exchange(true) || out == INVALID_HANDLE_VALUE) {
    return;
  }
  char line[512];
  size_t n = 0;
  const auto put = [&](const char* s) {
    while (*s && n < sizeof(line) - 2) line[n++] = *s++;
  };
  char thread[16];
  DWORD id = GetCurrentThreadId();
  int t = 0;
  char rev[16];
  do {
    rev[t++] = char('0' + id % 10);
    id /= 10;
  } while (id && t < 15);
  for (int i = 0; i < t; ++i) thread[i] = rev[t - 1 - i];
  thread[t] = 0;
  put("heap corrupt at: ");
  put(where);
  put(" (thread ");
  put(thread);
  put("); last found whole at: ");
  put(last_whole.load(std::memory_order_relaxed));
  line[n++] = '\n';
  DWORD written = 0;
  WriteFile(out, line, DWORD(n), &written, nullptr);
  FlushFileBuffers(out);
}

}  // namespace rex::plume_renderer

#else

namespace rex::plume_renderer {
inline void HeapCheck(const char*) {}
}  // namespace rex::plume_renderer

#endif
