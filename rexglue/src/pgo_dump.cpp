// A build that counts what the recompiled code executes (XERENGE_PGO_GENERATE)
// writes its counts when the process ends through exit() - and the title does
// not: it is closed from the window's thread. So the counts are written every
// few seconds from a thread of their own, each time over the last.
#ifdef XERENGE_PGO_GENERATE
#include <chrono>
#include <thread>

extern "C" int __llvm_profile_write_file(void);

namespace {
struct ProfileDumper {
  ProfileDumper() {
    std::thread([] {
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        __llvm_profile_write_file();
      }
    }).detach();
  }
} profile_dumper;
}  // namespace
#endif
