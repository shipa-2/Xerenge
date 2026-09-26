#include "dump_xex.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

#include "crypto.hpp"

namespace {

uint32_t BeU32(const std::vector<uint8_t>& data, size_t offset) {
    uint32_t v = 0;
    std::memcpy(&v, data.data() + offset, 4);
    return __builtin_bswap32(v);
}

void Aes128CbcDecrypt(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in, size_t in_len,
                      std::vector<uint8_t>* out) {
    out->resize(in_len);
    ::Aes128CbcDecrypt(key, iv, in, out->data(), in_len);
}

void DecompressBasic(const std::vector<uint8_t>& xex, uint32_t header_size, uint32_t fmt_off,
                     uint32_t image_size, const uint8_t session_key[16], bool encrypted,
                     std::vector<uint8_t>* image) {
    const uint32_t info_size = BeU32(xex, fmt_off);
    const uint16_t enc = static_cast<uint16_t>(xex[fmt_off + 4] << 8 | xex[fmt_off + 5]);
    const uint16_t comp = static_cast<uint16_t>(xex[fmt_off + 6] << 8 | xex[fmt_off + 7]);
    (void)enc;
    if (comp != 1) {
        throw std::runtime_error("unsupported XEX compression (need BASIC=1)");
    }
    const uint32_t block_count = (info_size - 8) / 8;
    std::vector<std::pair<uint32_t, uint32_t>> blocks;
    blocks.reserve(block_count);
    size_t off = fmt_off + 8;
    uint32_t total_data = 0;
    for (uint32_t n = 0; n < block_count; ++n) {
        const uint32_t data_size = BeU32(xex, off);
        const uint32_t zero_size = BeU32(xex, off + 4);
        blocks.emplace_back(data_size, zero_size);
        total_data += data_size;
        off += 8;
    }
    if (header_size + total_data > xex.size()) {
        throw std::runtime_error("XEX image is truncated");
    }

    std::vector<uint8_t> plaintext;
    if (encrypted) {
        const size_t cipher_len = total_data;
        const size_t pad = (16 - (cipher_len % 16)) % 16;
        std::vector<uint8_t> padded(cipher_len + pad, 0);
        std::memcpy(padded.data(), xex.data() + header_size, cipher_len);
        const uint8_t zero_iv[16] = {};
        Aes128CbcDecrypt(session_key, zero_iv, padded.data(), padded.size(), &plaintext);
    } else {
        plaintext.assign(xex.begin() + header_size, xex.begin() + header_size + total_data);
    }

    image->assign(image_size, 0);
    size_t dest = 0;
    size_t src = 0;
    for (const auto& [data_size, zero_size] : blocks) {
        if (dest + data_size > image->size() || src + data_size > plaintext.size()) {
            throw std::runtime_error("XEX block layout overflow");
        }
        std::memcpy(image->data() + dest, plaintext.data() + src, data_size);
        dest += data_size + zero_size;
        src += data_size;
    }
}

}  // namespace

void DumpXexToPe(const std::string& xex_path, const std::string& output_path) {
    std::ifstream in(xex_path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot read " + xex_path);
    }
    std::vector<uint8_t> xex((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (xex.size() < 24 || std::memcmp(xex.data(), "XEX2", 4) != 0) {
        throw std::runtime_error(xex_path + " is not an XEX2 image");
    }

    const uint32_t header_size = BeU32(xex, 8);
    const uint32_t security_offset = BeU32(xex, 16);
    const uint32_t header_count = BeU32(xex, 20);
    const uint32_t image_size = BeU32(xex, security_offset + 4);
    if (security_offset + 0x160 > xex.size()) {
        throw std::runtime_error("XEX security header out of range");
    }
    uint8_t aes_key[16];
    std::memcpy(aes_key, xex.data() + security_offset + 0x150, 16);

    uint32_t fmt_off = 0;
    bool found_fmt = false;
    size_t off = 24;
    for (uint32_t i = 0; i < header_count; ++i) {
        if (off + 8 > xex.size()) {
            break;
        }
        const uint32_t key = BeU32(xex, off);
        const uint32_t value = BeU32(xex, off + 4);
        off += 8;
        if (key == 0x000003FF) {
            fmt_off = value;
            found_fmt = true;
        }
    }
    if (!found_fmt) {
        throw std::runtime_error("XEX has no FILE_FORMAT_INFO header");
    }

    const uint16_t enc = static_cast<uint16_t>(xex[fmt_off + 4] << 8 | xex[fmt_off + 5]);
    if (enc != 0 && enc != 1) {
        throw std::runtime_error("unsupported XEX encryption");
    }

    static const uint8_t kRetailKey[16] = {0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
                                           0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91};
    static const uint8_t kDevkitKey[16] = {};
    const uint8_t zero_iv[16] = {};

    uint8_t session_key[16] = {};
    if (enc == 1) {
        std::vector<uint8_t> session_plain;
        Aes128CbcDecrypt(kRetailKey, zero_iv, aes_key, 16, &session_plain);
        std::memcpy(session_key, session_plain.data(), 16);
    }

    std::vector<uint8_t> image;
    DecompressBasic(xex, header_size, fmt_off, image_size, session_key, enc == 1, &image);

    if (image.size() < 2 || image[0] != 'M' || image[1] != 'Z') {
        uint8_t dev_session[16] = {};
        std::vector<uint8_t> session_plain;
        Aes128CbcDecrypt(kDevkitKey, zero_iv, aes_key, 16, &session_plain);
        std::memcpy(dev_session, session_plain.data(), 16);
        DecompressBasic(xex, header_size, fmt_off, image_size, dev_session, enc == 1, &image);
        if (image.size() < 2 || image[0] != 'M' || image[1] != 'Z') {
            throw std::runtime_error("decrypted image is not a PE (MZ)");
        }
    }

    std::filesystem::create_directories(std::filesystem::path(output_path).parent_path());
    std::ofstream out(output_path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("cannot write " + output_path);
    }
    out.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
}
