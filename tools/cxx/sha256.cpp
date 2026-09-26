#include "sha256.hpp"

#include <cstdio>
#include <fstream>

#include "crypto.hpp"

namespace {

std::string ToHex(const uint8_t digest[32]) {
    static const char hex[] = "0123456789abcdef";
    std::string out(64, '\0');
    for (unsigned i = 0; i < 32; ++i) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0xF];
    }
    return out;
}

}  // namespace

std::string Sha256Hex(std::string_view data) {
    Sha256 hash;
    hash.Update(data.data(), data.size());
    uint8_t digest[32];
    hash.Final(digest);
    return ToHex(digest);
}

bool Sha256FileHex(const std::string& path, std::string* out_hex, int* progress_percent) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);

    Sha256 hash;
    std::vector<char> chunk(1 << 22);
    std::streamoff done = 0;
    while (in) {
        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const std::streamsize got = in.gcount();
        if (got <= 0) {
            break;
        }
        hash.Update(chunk.data(), static_cast<size_t>(got));
        done += got;
        if (size > 0) {
            const int percent = static_cast<int>(100 * done / size);
            if (progress_percent) {
                *progress_percent = percent;
            }
            std::fprintf(stderr, "\rchecking the image: %3d%%", percent);
        }
    }
    if (size > 0) {
        std::fprintf(stderr, "\r\033[K");
    }

    uint8_t digest[32];
    hash.Final(digest);
    *out_hex = ToHex(digest);
    return true;
}
