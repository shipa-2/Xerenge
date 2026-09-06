#include <GLFW/glfw3.h>
#include <GL/gl.h>
#include <openssl/evp.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#ifdef XERENGE_HAS_PPC
#include <sys/mman.h>

#include "ppc_recomp_shared.h"
#endif

namespace
{
struct XexImportLibrary
{
    std::string name;
    uint16_t importCount = 0;
    std::vector<uint32_t> firstThunks;
};

struct XexInfo
{
    uint64_t size = 0;
    uint32_t moduleFlags = 0;
    uint32_t headerSize = 0;
    uint32_t securityOffset = 0;
    uint32_t headerCount = 0;
    uint32_t securityHeaderSize = 0;
    uint32_t imageSize = 0;
    uint32_t imageFlags = 0;
    uint32_t loadAddress = 0;
    uint32_t entryPoint = 0;
    uint32_t imageBase = 0;
    uint16_t encryptionType = 0xffff;
    uint16_t compressionType = 0xffff;
    uint32_t fileFormatInfoOffset = 0;
    uint32_t fileFormatInfoSize = 0;
    std::array<uint8_t, 16> encryptedImageKey{};
    std::vector<XexImportLibrary> importLibraries;
};

struct PeSection
{
    std::string name;
    uint32_t virtualAddress = 0;
    uint32_t virtualSize = 0;
    uint32_t rawSize = 0;
    uint32_t characteristics = 0;
};

struct MappedImage
{
    uint32_t base = 0;
    uint32_t entryPoint = 0;
    uint32_t size = 0;
    std::vector<uint8_t> memory;
    std::vector<PeSection> sections;
};

#ifdef XERENGE_HAS_PPC
class PpcGuestMemory
{
public:
    PpcGuestMemory() = default;

    ~PpcGuestMemory()
    {
        if (base_ != nullptr)
            munmap(base_, kGuestAddressSpaceSize);
    }

    PpcGuestMemory(const PpcGuestMemory&) = delete;
    PpcGuestMemory& operator=(const PpcGuestMemory&) = delete;

    bool initialize(const MappedImage& image)
    {
        const uint64_t imageBase = image.base;
        if (imageBase > kGuestAddressSpaceSize || image.memory.size() > kGuestAddressSpaceSize - imageBase)
        {
            std::cerr << "guest PE image does not fit the PPC address space\n";
            return false;
        }

        base_ = static_cast<uint8_t*>(mmap(nullptr, kGuestAddressSpaceSize, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
        if (base_ == MAP_FAILED)
        {
            base_ = nullptr;
            std::cerr << "could not reserve the 4 GiB PPC guest address space\n";
            return false;
        }
        std::memcpy(base_ + image.base, image.memory.data(), image.memory.size());

        for (PPCFuncMapping* mapping = PPCFuncMappings; mapping->host != nullptr; ++mapping)
        {
            if (mapping->guest < PPC_CODE_BASE)
            {
                std::cerr << "PPC function mapping precedes the game code: 0x" << std::hex
                          << mapping->guest << std::dec << '\n';
                return false;
            }
            const uint64_t tableOffset = PPC_IMAGE_BASE + PPC_IMAGE_SIZE +
                (static_cast<uint64_t>(static_cast<uint32_t>(mapping->guest) - PPC_CODE_BASE) * 2);
            if (tableOffset > kGuestAddressSpaceSize - sizeof(mapping->host))
            {
                std::cerr << "PPC function table exceeds guest address space\n";
                return false;
            }
            std::memcpy(base_ + tableOffset, &mapping->host, sizeof(mapping->host));
            ++functionCount_;
            if (mapping->guest == image.entryPoint)
                entryPoint_ = mapping->host;
        }

        if (entryPoint_ == nullptr)
        {
            std::cerr << "no recompiled PPC function for entry point 0x" << std::hex
                      << image.entryPoint << std::dec << '\n';
            return false;
        }
        initializeLoaderState();
        return true;
    }

    size_t functionCount() const { return functionCount_; }
    bool hasEntryPoint() const { return entryPoint_ != nullptr; }

    void invokeEntryPoint()
    {
        context_ = PPCContext{};
        context_.r1.u64 = 0x70000000u;
        entryPoint_(context_, base_);
    }

private:
    void storeGuestU32(uint32_t address, uint32_t value)
    {
        const uint32_t bigEndianValue = __builtin_bswap32(value);
        std::memcpy(base_ + address, &bigEndianValue, sizeof(bigEndianValue));
    }

    void initializeLoaderState()
    {
        // The XEX loader normally creates this callback-list sentinel before
        // transferring control to _xstart.  It belongs to BSS, so it is absent
        // from the PE image and must be initialized by the host runtime.
        constexpr uint32_t kLoaderCallbackList = 0x826AFCD4u;
        storeGuestU32(kLoaderCallbackList, kLoaderCallbackList);
    }

    static constexpr size_t kGuestAddressSpaceSize = size_t{1} << 32;
    uint8_t* base_ = nullptr;
    PPCFunc* entryPoint_ = nullptr;
    PPCContext context_{};
    size_t functionCount_ = 0;
};
#endif

#ifdef XERENGE_HAS_PPC
uint64_t gPpcServiceCalls = 0;

extern "C" void PPCImportedServiceTrap(const char* service, PPCContext& ctx, uint8_t*)
{
    // The generated import wrappers arrive here until a service is implemented.
    // Returning STATUS_NOT_IMPLEMENTED keeps the ABI explicit and gives the
    // launcher a deterministic failure instead of an unresolved host symbol.
    ++gPpcServiceCalls;
    if (gPpcServiceCalls <= 20)
        std::cerr << "unimplemented Xbox service invoked: " << service << '\n';
    ctx.r3.u64 = 0xC0000001u;
}

extern "C" void PPCUnknownIndirectTrap(uint32_t address, PPCContext& ctx, uint8_t*)
{
    ++gPpcServiceCalls;
    if (gPpcServiceCalls <= 20)
        std::cerr << "unresolved PPC indirect target: 0x" << std::hex << address << std::dec << '\n';
    ctx.r3.u64 = 0xC0000001u;
}
#endif

constexpr std::array<uint8_t, 16> kRetailKey{
    0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
    0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91};
constexpr std::array<uint8_t, 16> kDevkitKey{};

uint32_t readBE32(const std::array<char, 4>& bytes)
{
    return (static_cast<uint32_t>(static_cast<unsigned char>(bytes[0])) << 24) |
        (static_cast<uint32_t>(static_cast<unsigned char>(bytes[1])) << 16) |
        (static_cast<uint32_t>(static_cast<unsigned char>(bytes[2])) << 8) |
        static_cast<uint32_t>(static_cast<unsigned char>(bytes[3]));
}

bool readBE32(std::ifstream& input, uint32_t& value)
{
    std::array<char, 4> bytes{};
    input.read(bytes.data(), bytes.size());
    if (input.gcount() != static_cast<std::streamsize>(bytes.size()))
        return false;
    value = readBE32(bytes);
    return true;
}

bool readBE16(std::ifstream& input, uint16_t& value)
{
    std::array<char, 2> bytes{};
    input.read(bytes.data(), bytes.size());
    if (input.gcount() != static_cast<std::streamsize>(bytes.size()))
        return false;
    value = (static_cast<uint16_t>(static_cast<unsigned char>(bytes[0])) << 8) |
        static_cast<uint16_t>(static_cast<unsigned char>(bytes[1]));
    return true;
}

bool readBE32At(std::ifstream& input, uint64_t offset, uint32_t& value)
{
    input.seekg(static_cast<std::streamoff>(offset));
    return readBE32(input, value);
}

bool readBE16At(std::ifstream& input, uint64_t offset, uint16_t& value)
{
    input.seekg(static_cast<std::streamoff>(offset));
    return readBE16(input, value);
}

bool readBytesAt(std::ifstream& input, uint64_t offset, void* data, size_t size)
{
    input.seekg(static_cast<std::streamoff>(offset));
    input.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
    return input.gcount() == static_cast<std::streamsize>(size);
}

uint16_t readLE16(const uint8_t* data)
{
    return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t readLE32(const uint8_t* data)
{
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
        (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

std::optional<XexInfo> inspectXex(const std::string& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
    {
        std::cerr << "cannot open XEX: " << path << '\n';
        return std::nullopt;
    }

    const auto end = input.tellg();
    if (end < 4)
    {
        std::cerr << "XEX is shorter than its magic: " << path << '\n';
        return std::nullopt;
    }

    const uint64_t fileSize = static_cast<uint64_t>(end);
    input.seekg(0);
    std::array<char, 4> magic{};
    input.read(magic.data(), magic.size());
    if (magic != std::array<char, 4>{'X', 'E', 'X', '2'})
    {
        std::cerr << "invalid XEX magic in: " << path << '\n';
        return std::nullopt;
    }

    XexInfo info{};
    uint32_t reserved = 0;
    info.size = fileSize;
    if (!readBE32(input, info.moduleFlags) || !readBE32(input, info.headerSize) ||
        !readBE32(input, reserved) ||
        !readBE32(input, info.securityOffset) || !readBE32(input, info.headerCount))
    {
        std::cerr << "truncated XEX2 header: " << path << '\n';
        return std::nullopt;
    }

    if (info.headerSize < 0x18 || info.headerSize > fileSize ||
        info.securityOffset > fileSize - 4 || info.securityOffset + 0x114 > fileSize)
    {
        std::cerr << "invalid XEX2 header bounds: " << path << '\n';
        return std::nullopt;
    }

    input.seekg(info.securityOffset);
    if (!readBE32(input, info.securityHeaderSize) || !readBE32(input, info.imageSize))
    {
        std::cerr << "truncated XEX2 security header: " << path << '\n';
        return std::nullopt;
    }
    input.seekg(info.securityOffset + 0x10c);
    if (!readBE32(input, info.imageFlags) || !readBE32(input, info.loadAddress))
    {
        std::cerr << "truncated XEX2 security flags: " << path << '\n';
        return std::nullopt;
    }
    if (!readBytesAt(input, info.securityOffset + 0x150, info.encryptedImageKey.data(),
        info.encryptedImageKey.size()))
    {
        std::cerr << "truncated XEX AES key: " << path << '\n';
        return std::nullopt;
    }

    const uint64_t optionalHeadersEnd = 0x18ull + static_cast<uint64_t>(info.headerCount) * 8;
    if (optionalHeadersEnd > info.headerSize)
    {
        std::cerr << "optional XEX headers exceed header size: " << path << '\n';
        return std::nullopt;
    }

    for (uint32_t i = 0; i < info.headerCount; ++i)
    {
        uint32_t key = 0;
        uint32_t value = 0;
        const uint64_t offset = 0x18ull + static_cast<uint64_t>(i) * 8;
        if (!readBE32At(input, offset, key) || !readBE32(input, value))
            return std::nullopt;

        if (key == 0x00010100)
            info.entryPoint = value;
        else if (key == 0x00010201)
            info.imageBase = value;
        else if (key == 0x000003ff)
        {
            info.fileFormatInfoOffset = value;
            if (value > fileSize - 8)
            {
                std::cerr << "file format info is outside XEX: " << path << '\n';
                return std::nullopt;
            }
            uint32_t infoSize = 0;
            if (!readBE32At(input, value, infoSize) || infoSize < 8 || value + infoSize > fileSize)
            {
                std::cerr << "invalid XEX file format info: " << path << '\n';
                return std::nullopt;
            }
            input.seekg(static_cast<std::streamoff>(value + 4));
            if (!readBE16(input, info.encryptionType) || !readBE16(input, info.compressionType))
                return std::nullopt;
            info.fileFormatInfoSize = infoSize;
        }
        else if (key == 0x000103ff)
        {
            if (value > fileSize - 12)
            {
                std::cerr << "import library header is outside XEX: " << path << '\n';
                return std::nullopt;
            }
            uint32_t importSize = 0;
            uint32_t stringTableSize = 0;
            uint32_t libraryCount = 0;
            if (!readBE32At(input, value, importSize) || !readBE32At(input, value + 4, stringTableSize) ||
                !readBE32At(input, value + 8, libraryCount) || importSize < 12 ||
                value + importSize > fileSize || stringTableSize > importSize - 12)
            {
                std::cerr << "invalid XEX import library header: " << path << '\n';
                return std::nullopt;
            }
            std::vector<char> strings(stringTableSize);
            if (!readBytesAt(input, value + 12, strings.data(), strings.size()))
                return std::nullopt;
            std::vector<std::string> stringNames;
            for (size_t stringOffset = 0; stringOffset < strings.size();)
            {
                const char* string = strings.data() + stringOffset;
                const size_t remaining = strings.size() - stringOffset;
                const void* terminator = std::find(string, string + remaining, '\0');
                if (terminator == string + remaining)
                    return std::nullopt;
                stringNames.emplace_back(string);
                stringOffset += static_cast<const char*>(terminator) - string + 1;
            }
            uint64_t libraryOffset = value + 12 + stringTableSize;
            for (uint32_t libraryIndex = 0; libraryIndex < libraryCount; ++libraryIndex)
            {
                uint32_t librarySize = 0;
                uint16_t nameOffset = 0;
                uint16_t importCount = 0;
                if (libraryOffset + 40 > value + importSize ||
                    !readBE32At(input, libraryOffset, librarySize) || librarySize < 40 ||
                    libraryOffset + librarySize > value + importSize ||
                    !readBE16At(input, libraryOffset + 36, nameOffset) ||
                    !readBE16At(input, libraryOffset + 38, importCount) ||
                    nameOffset >= stringNames.size())
                {
                    std::cerr << "invalid XEX import library entry: " << path << '\n';
                    return std::nullopt;
                }
                if (40ull + static_cast<uint64_t>(importCount) * 4 > librarySize)
                {
                    std::cerr << "XEX import descriptors exceed library entry: " << path << '\n';
                    return std::nullopt;
                }
                XexImportLibrary library{stringNames[nameOffset], importCount};
                library.firstThunks.reserve(importCount);
                for (uint16_t importIndex = 0; importIndex < importCount; ++importIndex)
                {
                    uint32_t firstThunk = 0;
                    if (!readBE32At(input, libraryOffset + 40 + importIndex * 4, firstThunk))
                        return std::nullopt;
                    library.firstThunks.push_back(firstThunk);
                }
                info.importLibraries.push_back(std::move(library));
                libraryOffset += librarySize;
            }
        }
    }

    return info;
}

bool aesCbcDecrypt(const uint8_t* key, const uint8_t* encrypted, size_t size, std::vector<uint8_t>& output)
{
    if (size == 0 || size % 16 != 0)
        return false;
    output.resize(size);
    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    if (context == nullptr)
        return false;
    const bool initialized = EVP_DecryptInit_ex(context, EVP_aes_128_cbc(), nullptr, key, nullptr) == 1 &&
        EVP_CIPHER_CTX_set_padding(context, 0) == 1;
    int written = 0;
    int finalWritten = 0;
    const bool decrypted = initialized &&
        EVP_DecryptUpdate(context, output.data(), &written, encrypted, static_cast<int>(size)) == 1 &&
        EVP_DecryptFinal_ex(context, output.data() + written, &finalWritten) == 1;
    EVP_CIPHER_CTX_free(context);
    if (!decrypted || static_cast<size_t>(written + finalWritten) != size)
        return false;
    return true;
}

bool decodeImage(const std::string& path, const XexInfo& info, std::vector<uint8_t>& image)
{
    if (info.fileFormatInfoOffset == 0 || info.fileFormatInfoSize < 8)
    {
        std::cerr << "XEX has no supported file format metadata\n";
        return false;
    }

    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        return false;
    const uint64_t fileSize = static_cast<uint64_t>(input.tellg());
    if (info.headerSize > fileSize || fileSize - info.headerSize == 0)
    {
        std::cerr << "XEX image data exceeds file bounds\n";
        return false;
    }

    std::vector<uint8_t> source(fileSize - info.headerSize);
    input.seekg(info.headerSize);
    input.read(reinterpret_cast<char*>(source.data()), static_cast<std::streamsize>(source.size()));
    if (input.gcount() != static_cast<std::streamsize>(source.size()))
    {
        std::cerr << "could not read XEX image data\n";
        return false;
    }

    if (info.encryptionType == 1)
    {
        const std::array<const std::array<uint8_t, 16>*, 2> candidateKeys{
            &kRetailKey, &kDevkitKey};
        bool selected = false;
        for (const auto* wrappingKey : candidateKeys)
        {
            std::vector<uint8_t> imageKey;
            std::vector<uint8_t> decrypted;
            if (!aesCbcDecrypt(wrappingKey->data(), info.encryptedImageKey.data(), 16, imageKey) ||
                !aesCbcDecrypt(imageKey.data(), source.data(), source.size(), decrypted))
                continue;

            const bool looksLikeImage = decrypted.size() >= 2 && decrypted[0] == 'M' && decrypted[1] == 'Z';
            if (info.compressionType == 2 || looksLikeImage)
            {
                source = std::move(decrypted);
                selected = true;
                break;
            }
        }
        if (!selected)
        {
            std::cerr << "could not select a valid XEX image key\n";
            return false;
        }
    }

    if (info.compressionType == 0)
    {
        if (source.size() < info.imageSize)
            return false;
        image.assign(source.begin(), source.begin() + info.imageSize);
        return true;
    }
    if (info.compressionType != 1)
    {
        std::cerr << "unsupported XEX compression type: " << info.compressionType << '\n';
        return false;
    }

    const uint64_t blockBytes = info.fileFormatInfoSize - 8;
    if (blockBytes % 8 != 0)
        return false;
    image.clear();
    image.reserve(info.imageSize);
    size_t sourceOffset = 0;
    for (uint64_t offset = 0; offset < blockBytes; offset += 8)
    {
        uint32_t dataSize = 0;
        uint32_t zeroSize = 0;
        if (!readBE32At(input, info.fileFormatInfoOffset + 8 + offset, dataSize) ||
            !readBE32(input, zeroSize) || dataSize > source.size() - sourceOffset)
            return false;
        image.insert(image.end(), source.begin() + sourceOffset, source.begin() + sourceOffset + dataSize);
        image.insert(image.end(), zeroSize, 0);
        sourceOffset += dataSize;
    }
    if (image.size() != info.imageSize)
    {
        std::cerr << "XEX decompressed size mismatch: got " << image.size()
                  << ", expected " << info.imageSize << '\n';
        return false;
    }
    return true;
}

bool extractImage(const std::string& path, const XexInfo& info, const std::string& output)
{
    std::vector<uint8_t> image;
    if (!decodeImage(path, info, image))
        return false;
    std::ofstream destination(output, std::ios::binary);
    if (!destination)
    {
        std::cerr << "cannot create image output: " << output << '\n';
        return false;
    }
    destination.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
    return destination.good();
}

std::optional<MappedImage> mapPeImage(const std::vector<uint8_t>& image, const XexInfo& xex)
{
    if (image.size() < 0x40 || image[0] != 'M' || image[1] != 'Z')
    {
        std::cerr << "decoded image is not a DOS/PE image\n";
        return std::nullopt;
    }
    const uint32_t peOffset = readLE32(image.data() + 0x3c);
    if (peOffset > image.size() - 24 || readLE32(image.data() + peOffset) != 0x00004550)
    {
        std::cerr << "decoded image has an invalid PE header\n";
        return std::nullopt;
    }

    const uint8_t* fileHeader = image.data() + peOffset + 4;
    const uint16_t sectionCount = readLE16(fileHeader + 2);
    const uint16_t optionalSize = readLE16(fileHeader + 16);
    const uint8_t* optionalHeader = fileHeader + 20;
    if (readLE16(optionalHeader) != 0x10b || optionalHeader + optionalSize > image.data() + image.size() ||
        sectionCount == 0 || sectionCount > 96 || optionalSize < 60)
    {
        std::cerr << "unsupported or invalid PE32 optional header\n";
        return std::nullopt;
    }

    const uint32_t entryRva = readLE32(optionalHeader + 16);
    const uint32_t imageBase = readLE32(optionalHeader + 28);
    const uint32_t imageSize = readLE32(optionalHeader + 56);
    const uint32_t headersSize = readLE32(optionalHeader + 60);
    if (imageSize == 0 || entryRva >= imageSize)
    {
        std::cerr << "invalid PE image size or entry point\n";
        return std::nullopt;
    }

    const uint8_t* sectionTable = optionalHeader + optionalSize;
    const uint64_t sectionTableSize = static_cast<uint64_t>(sectionCount) * 40;
    if (sectionTable + sectionTableSize > image.data() + image.size())
    {
        std::cerr << "PE section table exceeds decoded image\n";
        return std::nullopt;
    }

    MappedImage mapped{};
    mapped.base = xex.imageBase != 0 ? xex.imageBase : imageBase;
    mapped.entryPoint = mapped.base + entryRva;
    mapped.size = imageSize;
    mapped.memory.assign(imageSize, 0);
    if (headersSize > image.size() || headersSize > mapped.memory.size())
    {
        std::cerr << "PE headers exceed decoded or mapped image\n";
        return std::nullopt;
    }
    std::copy_n(image.begin(), headersSize, mapped.memory.begin());

    for (uint16_t i = 0; i < sectionCount; ++i)
    {
        const uint8_t* section = sectionTable + static_cast<size_t>(i) * 40;
        const uint32_t virtualSize = readLE32(section + 8);
        const uint32_t virtualAddress = readLE32(section + 12);
        const uint32_t rawSize = readLE32(section + 16);
        const uint32_t rawOffset = readLE32(section + 20);
        const uint32_t characteristics = readLE32(section + 36);
        const uint32_t mappedSize = std::max(virtualSize, rawSize);
        if (virtualAddress > imageSize || mappedSize > imageSize - virtualAddress)
        {
            std::cerr << "PE section exceeds guest image: " << i << '\n';
            return std::nullopt;
        }
        if (rawOffset > image.size() || rawSize > image.size() - rawOffset)
        {
            std::cerr << "PE section raw data exceeds decoded image: " << i << '\n';
            return std::nullopt;
        }
        std::copy_n(image.begin() + rawOffset, rawSize, mapped.memory.begin() + virtualAddress);
        size_t nameLength = 0;
        while (nameLength < 8 && section[nameLength] != '\0')
            ++nameLength;
        mapped.sections.push_back({std::string(reinterpret_cast<const char*>(section), nameLength),
            virtualAddress, virtualSize, rawSize, characteristics});
    }
    return mapped;
}

bool mapAndPrintImage(const std::string& path, const XexInfo& info)
{
    std::vector<uint8_t> image;
    if (!decodeImage(path, info, image))
        return false;
    const auto mapped = mapPeImage(image, info);
    if (!mapped)
        return false;
    std::cout << "mapped PE image: base=0x" << std::hex << mapped->base
              << " entry=0x" << mapped->entryPoint << " size=0x" << mapped->size
              << std::dec << " sections=" << mapped->sections.size() << '\n';
    for (const auto& section : mapped->sections)
        std::cout << "  " << section.name << " RVA=0x" << std::hex << section.virtualAddress
                  << " virtual=0x" << section.virtualSize << " raw=0x" << section.rawSize
                  << " flags=0x" << section.characteristics << std::dec << '\n';
    return true;
}

void printXexInfo(const XexInfo& info)
{
    std::cout << "XEX2 image: " << info.size << " bytes\n"
              << "  module flags: 0x" << std::hex << info.moduleFlags << '\n'
              << "  header size: 0x" << info.headerSize << '\n'
              << "  security offset: 0x" << info.securityOffset << '\n'
              << "  optional headers: " << std::dec << info.headerCount << '\n'
              << "  image size: 0x" << std::hex << info.imageSize << '\n'
              << "  image flags: 0x" << info.imageFlags << '\n'
              << "  load address: 0x" << info.loadAddress << '\n'
              << "  entry point: 0x" << info.entryPoint << '\n'
              << "  image base: 0x" << info.imageBase << '\n'
              << "  encryption type: " << std::dec << info.encryptionType << '\n'
              << "  compression type: " << info.compressionType << '\n';
}

void printImports(const XexInfo& info)
{
    uint32_t total = 0;
    std::cout << "XEX import libraries: " << info.importLibraries.size() << '\n';
    for (const auto& library : info.importLibraries)
    {
        total += library.importCount;
        std::cout << "  " << library.name << ": " << library.importCount << " imports\n";
    }
    std::cout << "total imports: " << total << '\n';
}

struct ServiceBinding;
using ServiceTrap = void (*)(const ServiceBinding&);

struct ServiceBinding
{
    std::string library;
    uint32_t importIndex = 0;
    uint32_t firstThunk = 0;
    ServiceTrap trap = nullptr;
};

void unimplementedServiceTrap(const ServiceBinding& binding)
{
    std::cerr << "unimplemented Xbox service: " << binding.library
              << "[" << binding.importIndex << "] thunk=0x"
              << std::hex << binding.firstThunk << std::dec << '\n';
}

std::vector<ServiceBinding> buildServiceTable(const XexInfo& info, ServiceTrap trap)
{
    std::vector<ServiceBinding> table;
    for (const auto& library : info.importLibraries)
    {
        for (uint32_t i = 0; i < library.firstThunks.size(); ++i)
        {
            table.push_back({library.name, i, library.firstThunks[i], trap});
        }
    }
    return table;
}

void printServices(const XexInfo& info)
{
    const auto table = buildServiceTable(info, unimplementedServiceTrap);
    std::cout << "runtime service table: " << table.size() << " trap bindings\n";
    for (size_t i = 0; i < std::min<size_t>(table.size(), 8); ++i)
    {
        const auto& binding = table[i];
        std::cout << "  " << binding.library << "[" << binding.importIndex
                  << "] thunk=0x" << std::hex << binding.firstThunk << std::dec << '\n';
    }
    if (table.size() > 8)
        std::cout << "  ... " << table.size() - 8 << " more bindings\n";
}

bool initializeVulkan(VkInstance& instance, VkPhysicalDevice& physicalDevice)
{
    VkApplicationInfo applicationInfo{};
    applicationInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    applicationInfo.pApplicationName = "Xerenge Runtime";
    applicationInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    applicationInfo.pEngineName = "Xerenge";
    applicationInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    applicationInfo.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &applicationInfo;
    if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS)
    {
        std::cerr << "could not create Vulkan instance\n";
        return false;
    }

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    if (deviceCount == 0)
    {
        std::cerr << "no Vulkan physical device found\n";
        vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
        return false;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
    physicalDevice = devices.front();
    return true;
}
}

int main(int argc, char** argv)
{
    if (argc > 1 && (std::string(argv[1]) == "--validate" || std::string(argv[1]) == "--inspect"))
    {
        if (argc != 3)
        {
            std::cerr << "usage: xerenge-runtime --validate <file.xex>\n";
            return 2;
        }

        const auto info = inspectXex(argv[2]);
        if (!info)
            return 1;

        if (std::string(argv[1]) == "--inspect")
            printXexInfo(*info);
        else
            std::cout << "valid XEX2 image: " << info->size << " bytes\n";
        return 0;
    }

    if (argc > 1 && std::string(argv[1]) == "--extract-image")
    {
        if (argc != 4)
        {
            std::cerr << "usage: xerenge-runtime --extract-image <file.xex> <output.bin>\n";
            return 2;
        }
        const auto info = inspectXex(argv[2]);
        return info && extractImage(argv[2], *info, argv[3]) ? 0 : 1;
    }

    if (argc > 1 && std::string(argv[1]) == "--map-image")
    {
        if (argc != 3)
        {
            std::cerr << "usage: xerenge-runtime --map-image <file.xex>\n";
            return 2;
        }
        const auto info = inspectXex(argv[2]);
        return info && mapAndPrintImage(argv[2], *info) ? 0 : 1;
    }

    if (argc > 1 && (std::string(argv[1]) == "--ppc-prepare" || std::string(argv[1]) == "--ppc-entry"))
    {
        if (argc != 3)
        {
            std::cerr << "usage: xerenge-runtime --ppc-prepare|--ppc-entry <file.xex>\n";
            return 2;
        }
#ifdef XERENGE_HAS_PPC
        const auto info = inspectXex(argv[2]);
        std::vector<uint8_t> image;
        if (!info || !decodeImage(argv[2], *info, image))
            return 1;
        const auto mapped = mapPeImage(image, *info);
        if (!mapped)
            return 1;
        PpcGuestMemory guest;
        if (!guest.initialize(*mapped))
            return 1;
        std::cout << "prepared PPC guest memory: 4 GiB reservation, "
                  << guest.functionCount() << " function mappings, entry point resolved\n";
        if (std::string(argv[1]) == "--ppc-entry")
        {
            gPpcServiceCalls = 0;
            guest.invokeEntryPoint();
            std::cout << "PPC entry point returned after " << gPpcServiceCalls
                      << " service calls\n";
        }
        return 0;
#else
        std::cerr << "PPC module was not enabled; configure with -DXERENGE_PPC_DIRECTORY=<generated PPC directory>\n";
        return 1;
#endif
    }

    if (argc > 1 && std::string(argv[1]) == "--imports")
    {
        if (argc != 3)
        {
            std::cerr << "usage: xerenge-runtime --imports <file.xex>\n";
            return 2;
        }
        const auto info = inspectXex(argv[2]);
        if (!info)
            return 1;
        printImports(*info);
        return 0;
    }

    if (argc > 1 && std::string(argv[1]) == "--services")
    {
        if (argc != 3)
        {
            std::cerr << "usage: xerenge-runtime --services <file.xex>\n";
            return 2;
        }
        const auto info = inspectXex(argv[2]);
        if (!info)
            return 1;
        printServices(*info);
        return 0;
    }

    if (argc > 2)
    {
        std::cerr << "usage: xerenge-runtime [file.xex]\n";
        return 2;
    }

    std::string xexPath;
    if (argc == 2)
    {
        xexPath = argv[1];
        if (!inspectXex(xexPath))
            return 1;
    }

    if (!glfwInit())
    {
        std::cerr << "could not initialize GLFW\n";
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "Xerenge Runtime", nullptr, nullptr);
    if (window == nullptr)
    {
        std::cerr << "could not create a window\n";
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    const bool gpuReady = initializeVulkan(instance, physicalDevice);
    if (gpuReady)
    {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physicalDevice, &properties);
        std::cout << "Vulkan device: " << properties.deviceName << '\n';
    }

    std::cout << "Xerenge runtime started";
    if (!xexPath.empty())
        std::cout << " with " << xexPath;
    std::cout << '\n' << std::flush;

    const auto start = std::chrono::steady_clock::now();
    uint32_t frames = 0;
    bool readbackReported = false;
    while (!glfwWindowShouldClose(window))
    {
        const float phase = static_cast<float>(frames % 360) / 360.0f;
        glViewport(0, 0, 1280, 720);
        glClearColor(0.04f + phase * 0.08f, 0.12f, 0.24f + phase * 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glfwSwapBuffers(window);
        glfwPollEvents();
        ++frames;
        if (!readbackReported)
        {
            glFinish();
            std::array<uint8_t, 4> pixel{};
            glReadPixels(640, 360, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
            std::cout << "diagnostic framebuffer readback: rgba="
                      << static_cast<uint32_t>(pixel[0]) << ','
                      << static_cast<uint32_t>(pixel[1]) << ','
                      << static_cast<uint32_t>(pixel[2]) << ','
                      << static_cast<uint32_t>(pixel[3]) << '\n' << std::flush;
            readbackReported = true;
        }
        if ((frames % 60) == 0)
        {
            const auto elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
            const float fps = elapsed > 0.0f ? frames / elapsed : 0.0f;
            glfwSetWindowTitle(window, ("Xerenge Runtime - diagnostic frame " + std::to_string(static_cast<int>(fps)) + " FPS").c_str());
        }
    }

    if (instance != VK_NULL_HANDLE)
        vkDestroyInstance(instance, nullptr);
    glfwDestroyWindow(window);
    glfwTerminate();
    return gpuReady ? 0 : 1;
}
