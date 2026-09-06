#include <GLFW/glfw3.h>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace
{
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
};

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
        }
    }

    return info;
}

bool extractUncompressedImage(const std::string& path, const XexInfo& info, const std::string& output)
{
    if (info.encryptionType != 0 || info.compressionType != 0)
    {
        std::cerr << "XEX image requires decryption/decompression before extraction "
                  << "(encryption=" << info.encryptionType
                  << ", compression=" << info.compressionType << ")\n";
        return false;
    }

    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        return false;
    const uint64_t fileSize = static_cast<uint64_t>(input.tellg());
    if (info.headerSize > fileSize || info.imageSize > fileSize - info.headerSize)
    {
        std::cerr << "uncompressed XEX image exceeds file bounds\n";
        return false;
    }

    std::vector<char> image(info.imageSize);
    input.seekg(info.headerSize);
    input.read(image.data(), static_cast<std::streamsize>(image.size()));
    if (input.gcount() != static_cast<std::streamsize>(image.size()))
    {
        std::cerr << "could not read complete XEX image\n";
        return false;
    }

    std::ofstream destination(output, std::ios::binary);
    if (!destination)
    {
        std::cerr << "cannot create image output: " << output << '\n';
        return false;
    }
    destination.write(image.data(), static_cast<std::streamsize>(image.size()));
    return destination.good();
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

bool initializeVulkan(VkInstance& instance, VkPhysicalDevice& physicalDevice)
{
    uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    if (glfwExtensions == nullptr)
    {
        std::cerr << "GLFW did not provide Vulkan instance extensions\n";
        return false;
    }

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
    createInfo.enabledExtensionCount = glfwExtensionCount;
    createInfo.ppEnabledExtensionNames = glfwExtensions;

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
        return info && extractUncompressedImage(argv[2], *info, argv[3]) ? 0 : 1;
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
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "Xerenge Runtime", nullptr, nullptr);
    if (window == nullptr)
    {
        std::cerr << "could not create a window\n";
        glfwTerminate();
        return 1;
    }

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
    std::cout << '\n';

    while (!glfwWindowShouldClose(window))
        glfwPollEvents();

    if (instance != VK_NULL_HANDLE)
        vkDestroyInstance(instance, nullptr);
    glfwDestroyWindow(window);
    glfwTerminate();
    return gpuReady ? 0 : 1;
}
