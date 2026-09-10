#include "xbox_media.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
constexpr uint64_t kSectorSize = 2048;
constexpr uint64_t kHeaderOffset = 0x10000;
constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";

uint16_t readLE16(const uint8_t* bytes)
{
    return static_cast<uint16_t>(bytes[0]) |
        (static_cast<uint16_t>(bytes[1]) << 8);
}

uint32_t readLE32(const uint8_t* bytes)
{
    return static_cast<uint32_t>(bytes[0]) |
        (static_cast<uint32_t>(bytes[1]) << 8) |
        (static_cast<uint32_t>(bytes[2]) << 16) |
        (static_cast<uint32_t>(bytes[3]) << 24);
}
}

bool XboxMedia::open(const std::string& path)
{
    std::lock_guard lock(mutex_);
    image_.close();
    image_.clear();
    entries_.clear();
    openFiles_.clear();
    directoryMode_ = false;
    root_.clear();

    std::error_code ec;
    if (std::filesystem::is_directory(path, ec))
    {
        directoryMode_ = true;
        root_ = path;
        while (!root_.empty() && (root_.back() == '/' || root_.back() == '\\'))
            root_.pop_back();
        return true;
    }

    image_.open(path, std::ios::binary);
    if (!image_)
        return false;

    std::array<char, 32> header{};
    image_.seekg(static_cast<std::streamoff>(kHeaderOffset));
    image_.read(header.data(), header.size());
    if (image_.gcount() != static_cast<std::streamsize>(header.size()) ||
        std::memcmp(header.data(), kMagic, sizeof(kMagic) - 1) != 0)
    {
        image_.close();
        return false;
    }

    const auto* bytes = reinterpret_cast<const uint8_t*>(header.data());
    const uint32_t rootSector = readLE32(bytes + 0x14);
    const uint32_t rootSize = readLE32(bytes + 0x18);
    if (rootSector == 0 || rootSize == 0 || rootSize > 64u * 1024u * 1024u)
    {
        image_.close();
        return false;
    }
    if (!parseDirectory(rootSector, rootSize, ""))
        return false;
    // The prototype issues one optional media open with an empty object name
    // while probing the sound bank. Keep that probe as a valid zero-byte file
    // so the guest async file state machine can complete instead of reading
    // handle 0xffffffff and terminating its resource worker.
    entries_.emplace("", FileEntry{0, 0, false});
    return true;
}

bool XboxMedia::isOpen() const
{
    std::lock_guard lock(mutex_);
    return directoryMode_ || image_.is_open();
}

bool XboxMedia::extractTo(const std::string& outDir) const
{
    std::lock_guard lock(mutex_);
    if (!image_.is_open())
        return false;
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    std::vector<char> buffer;
    size_t written = 0;
    for (const auto& [path, entry] : entries_)
    {
        if (entry.directory || path.empty())
            continue;
        const std::filesystem::path target =
            std::filesystem::path(outDir) / std::filesystem::path(path);
        std::filesystem::create_directories(target.parent_path(), ec);

        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        buffer.assign(entry.size, 0);
        if (entry.size != 0 &&
            !readAt(static_cast<uint64_t>(entry.startSector) * kSectorSize,
                    buffer.data(), entry.size))
            return false;
        out.write(buffer.data(), static_cast<std::streamsize>(entry.size));
        if (!out)
            return false;
        ++written;
    }
    return written != 0;
}

bool XboxMedia::readAt(uint64_t offset, void* destination, size_t size) const
{
    image_.clear();
    image_.seekg(static_cast<std::streamoff>(offset));
    image_.read(static_cast<char*>(destination), static_cast<std::streamsize>(size));
    return image_.gcount() == static_cast<std::streamsize>(size);
}

std::string XboxMedia::normalize(std::string path)
{
    for (char& c : path)
    {
        if (c == '\\')
            c = '/';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    while (!path.empty() && path.front() == '/')
        path.erase(path.begin());
    if (path.size() >= 2 && path[1] == ':')
        path.erase(0, 2);
    while (!path.empty() && path.front() == '/')
        path.erase(path.begin());
    return path;
}

bool XboxMedia::parseDirectory(uint32_t startSector, uint32_t size, const std::string& prefix)
{
    const uint64_t tableOffset = static_cast<uint64_t>(startSector) * kSectorSize;
    return parseNode(tableOffset, size, 0, prefix, prefix.empty());
}

bool XboxMedia::parseNode(uint64_t tableOffset, uint32_t tableSize, uint64_t nodeOffset,
    const std::string& prefix, bool /*rootNode*/)
{
    if (nodeOffset + 14 > tableSize || (nodeOffset & 3) != 0)
        return false;

    std::array<uint8_t, 14> raw{};
    if (!readAt(tableOffset + nodeOffset, raw.data(), raw.size()))
        return false;
    const uint16_t left = readLE16(raw.data());
    const uint16_t right = readLE16(raw.data() + 2);
    const FileEntry entry{
        readLE32(raw.data() + 4), readLE32(raw.data() + 8), (raw[12] & 0x10u) != 0};
    const uint32_t nameLength = raw[13];
    if (nodeOffset + 14u + nameLength > tableSize)
        return false;

    std::string name(nameLength, '\0');
    if (!readAt(tableOffset + nodeOffset + 14, name.data(), name.size()))
        return false;

    if (left != 0 && !parseNode(tableOffset, tableSize, static_cast<uint64_t>(left) * 4,
        prefix, false))
        return false;

    // Every node in an XDVDFS directory tree - including node 0 of the volume
    // root - is a real entry; there is no synthetic disc-label node to skip.
    const std::string path = prefix + normalize(name);
    if (!path.empty() && path.back() != '/')
        entries_[path] = entry;
    if (entry.directory && entry.size != 0 && !path.empty() && path.back() != '/' &&
        !parseDirectory(entry.startSector, entry.size, path + "/"))
        return false;

    if (right != 0 && !parseNode(tableOffset, tableSize, static_cast<uint64_t>(right) * 4,
        prefix, false))
        return false;
    return true;
}

bool XboxMedia::openFile(const std::string& xboxPath, uint32_t& handle, uint64_t& size)
{
    std::lock_guard lock(mutex_);
    const std::string key = normalize(xboxPath);

    OpenFile open{};
    if (directoryMode_)
    {
        if (key.empty())
        {
            // Keep the sound-bank probe (empty object name) valid, matching
            // the disc path: a zero-byte file the async state machine can
            // complete instead of reading the invalid-handle sentinel.
            open.hostFile = std::make_shared<std::ifstream>();
            open.hostSize = 0;
        }
        else
        {
            auto stream = std::make_shared<std::ifstream>(
                root_ + "/" + key, std::ios::binary);
            if (!*stream)
                return false;
            stream->seekg(0, std::ios::end);
            open.hostSize = static_cast<uint64_t>(stream->tellg());
            stream->seekg(0, std::ios::beg);
            open.hostFile = std::move(stream);
        }
        size = open.hostSize;
    }
    else
    {
        const auto it = entries_.find(key);
        if (it == entries_.end() || it->second.directory)
            return false;
        open.entry = it->second;
        size = it->second.size;
    }

    handle = nextHandle_++;
    if (handle == 0)
        handle = nextHandle_++;
    openFiles_[handle] = std::move(open);
    return true;
}

bool XboxMedia::readFile(uint32_t handle, void* destination, uint32_t size, uint32_t& bytesRead)
{
    std::lock_guard lock(mutex_);
    const auto it = openFiles_.find(handle);
    if (it == openFiles_.end())
        return false;
    OpenFile& file = it->second;
    const uint64_t total = file.hostFile ? file.hostSize : file.entry.size;
    const uint64_t remaining = total > file.position ? total - file.position : 0;
    bytesRead = static_cast<uint32_t>(std::min<uint64_t>(size, remaining));
    if (bytesRead == 0)
        return true;

    if (file.hostFile)
    {
        file.hostFile->clear();
        file.hostFile->seekg(static_cast<std::streamoff>(file.position));
        file.hostFile->read(static_cast<char*>(destination), bytesRead);
        if (file.hostFile->gcount() != static_cast<std::streamsize>(bytesRead))
            return false;
    }
    else
    {
        const uint64_t offset =
            static_cast<uint64_t>(file.entry.startSector) * kSectorSize + file.position;
        if (!readAt(offset, destination, bytesRead))
            return false;
    }
    file.position += bytesRead;
    return true;
}

bool XboxMedia::seekFile(uint32_t handle, int64_t distance, uint32_t method, uint64_t& position)
{
    std::lock_guard lock(mutex_);
    const auto it = openFiles_.find(handle);
    if (it == openFiles_.end())
        return false;
    const OpenFile& file = it->second;
    const uint64_t total = file.hostFile ? file.hostSize : file.entry.size;
    const int64_t base = method == 0 ? 0 : method == 1
        ? static_cast<int64_t>(file.position)
        : static_cast<int64_t>(total);
    const int64_t next = base + distance;
    if (next < 0)
        return false;
    it->second.position = static_cast<uint64_t>(next);
    position = it->second.position;
    return true;
}

bool XboxMedia::position(uint32_t handle, uint64_t& position) const
{
    std::lock_guard lock(mutex_);
    const auto it = openFiles_.find(handle);
    if (it == openFiles_.end())
        return false;
    position = it->second.position;
    return true;
}

bool XboxMedia::fileSize(uint32_t handle, uint64_t& size) const
{
    std::lock_guard lock(mutex_);
    const auto it = openFiles_.find(handle);
    if (it == openFiles_.end())
        return false;
    size = it->second.hostFile ? it->second.hostSize : it->second.entry.size;
    return true;
}

void XboxMedia::closeFile(uint32_t handle)
{
    std::lock_guard lock(mutex_);
    openFiles_.erase(handle);
}
