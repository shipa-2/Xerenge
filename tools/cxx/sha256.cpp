#include "sha256.hpp"

#include <cstdio>
#include <fstream>

#include <openssl/evp.h>

std::string Sha256Hex(std::string_view data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, data.data(), data.size());
    EVP_DigestFinal_ex(ctx, digest, &len);
    EVP_MD_CTX_free(ctx);

    static const char hex[] = "0123456789abcdef";
    std::string out(len * 2, '\0');
    for (unsigned i = 0; i < len; ++i) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0xF];
    }
    return out;
}

bool Sha256FileHex(const std::string& path, std::string* out_hex, int* progress_percent) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);

    std::vector<char> chunk(1 << 22);
    std::streamoff done = 0;
    while (in) {
        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const std::streamsize got = in.gcount();
        if (got <= 0) {
            break;
        }
        EVP_DigestUpdate(ctx, chunk.data(), static_cast<size_t>(got));
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

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_DigestFinal_ex(ctx, digest, &len);
    EVP_MD_CTX_free(ctx);

    static const char hex[] = "0123456789abcdef";
    out_hex->resize(len * 2);
    for (unsigned i = 0; i < len; ++i) {
        (*out_hex)[i * 2] = hex[digest[i] >> 4];
        (*out_hex)[i * 2 + 1] = hex[digest[i] & 0xF];
    }
    return true;
}
