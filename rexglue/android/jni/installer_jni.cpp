// The native half of the Android installer (libxerenge_installer.so): checks
// the disc image against the retail hash and extracts it, with the same code
// the desktop installer's extract-image runs (tools/cxx). Kept apart from the
// game so the installer does not load 60 MB of recompiled code.
//
// The image comes from the system file picker as a descriptor, and is read
// through it: reopening it by path (/proc/self/fd/N) is checked against the
// storage permissions the app does not have.

#include <jni.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "crypto.hpp"
#include "xdvdfs.hpp"

namespace {

constexpr const char* kRetailSha256 =
    "34c1bd4d549c2c53f29d814fa5e5d1c04c533c5ca0c39e57b6c2538f44ff59b4";

std::string ToHex(const uint8_t digest[32]) {
  static const char hex[] = "0123456789abcdef";
  std::string out(64, '\0');
  for (unsigned i = 0; i < 32; ++i) {
    out[i * 2] = hex[digest[i] >> 4];
    out[i * 2 + 1] = hex[digest[i] & 0xF];
  }
  return out;
}

std::string FromJava(JNIEnv* env, jstring text) {
  const char* chars = env->GetStringUTFChars(text, nullptr);
  std::string out(chars ? chars : "");
  if (chars) {
    env->ReleaseStringUTFChars(text, chars);
  }
  return out;
}

// progress.onProgress(int phase, long done, long total): phase 0 checking,
// 1 extracting (done = files, total = bytes so far).
struct Progress {
  JNIEnv* env;
  jobject target;
  jmethodID method;
  void Report(int phase, int64_t done, int64_t total) const {
    env->CallVoidMethod(target, method, jint(phase), jlong(done), jlong(total));
  }
};

}  // namespace

extern "C" JNIEXPORT jstring JNICALL Java_com_xerenge_burnout_InstallerActivity_nativeInstall(
    JNIEnv* env, jclass, jint fd, jstring destination, jboolean verify, jobject progress_object) {
  Progress progress{env, progress_object,
                    env->GetMethodID(env->GetObjectClass(progress_object), "onProgress", "(IJJ)V")};
  try {
    if (verify) {
      struct stat info {};
      if (fstat(fd, &info) != 0) {
        return env->NewStringUTF("cannot read the image");
      }
      const int64_t size = info.st_size;
      Sha256 hash;
      std::vector<char> chunk(size_t(1) << 22);
      int64_t done = 0;
      for (;;) {
        const ssize_t got = pread(fd, chunk.data(), chunk.size(), off_t(done));
        if (got < 0) {
          return env->NewStringUTF("cannot read the image");
        }
        if (got == 0) {
          break;
        }
        hash.Update(chunk.data(), size_t(got));
        done += got;
        progress.Report(0, done, size);
      }
      uint8_t digest[32];
      hash.Final(digest);
      if (ToHex(digest) != kRetailSha256) {
        return env->NewStringUTF(
            "this is not the retail Burnout Revenge disc image (the checksum does not match)");
      }
    }
    XdvdfsImage xdvdfs{static_cast<int>(fd)};
    uint32_t root_sector = 0;
    uint32_t root_size = 0;
    xdvdfs.Root(&root_sector, &root_size);
    int64_t files = 0;
    int64_t bytes = 0;
    const XdvdfsReportFn report = [&](const std::string& path, uint64_t length) {
      if (!path.empty() && path.back() != '/') {
        ++files;
        bytes += int64_t(length);
        progress.Report(1, files, bytes);
      }
    };
    XdvdfsExtract(xdvdfs, root_sector, root_size, FromJava(env, destination), false, report);
  } catch (const std::exception& error) {
    return env->NewStringUTF(error.what());
  }
  return nullptr;
}
