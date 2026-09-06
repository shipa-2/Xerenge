#include <GLFW/glfw3.h>
#include <GL/gl.h>
#include <openssl/evp.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string_view>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef XERENGE_HAS_PPC
#include <sys/mman.h>
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>

#include "ppc_recomp_shared.h"
#include "xenos_gpu.h"
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
uint64_t gPpcUnknownIndirectCalls = 0;
std::atomic<uint32_t> gPpcLastFunction = 0;
std::atomic<uint64_t> gPpcFunctionTransitions = 0;
std::atomic<uint64_t> gPpcFunctionCalls = 0;
std::atomic<bool> gPpcTraceEnabled = false;
XenosGpu gXenosGpu;

void ppcWatchdogSignal(int)
{
    const char message[] = "PPC watchdog native backtrace:\n";
    ::write(STDERR_FILENO, message, sizeof(message) - 1);
    void* frames[32]{};
    const int count = ::backtrace(frames, 32);
    ::backtrace_symbols_fd(frames, count, STDERR_FILENO);
}

extern "C" void PPCTraceStore(uint32_t address, uint32_t value, uint64_t lr)
{
    if (std::getenv("XERENGE_PPC_TRACE") != nullptr)
        std::cerr << "guest global store address=0x" << std::hex << address
                  << " value=0x" << value << " lr=0x" << lr << std::dec << '\n';
}

extern "C" void PPCTraceFunction(uint32_t address, PPCContext& ctx, uint8_t* base)
{
    const uint64_t callCount = gPpcFunctionCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (gPpcTraceEnabled.load(std::memory_order_relaxed) && (callCount % 10000) == 0)
    {
        std::cerr << "guest function calls=" << callCount << " current=0x"
                  << std::hex << address << std::dec << '\n';
        if (address == 0x8259B0FC)
        {
            uint32_t savedLr = 0;
            std::memcpy(&savedLr, base + ctx.r1.u32 - 8, sizeof(savedLr));
            std::cerr << "  restore helper r1=0x" << std::hex << ctx.r1.u32
                      << " saved_lr=0x" << __builtin_bswap32(savedLr)
                      << " ctx_lr=0x" << static_cast<uint32_t>(ctx.lr) << std::dec << '\n';
        }
    }
    if (!gPpcTraceEnabled.load(std::memory_order_relaxed))
        return;

    static std::atomic<uint32_t> waitTraceCount = 0;
    if (address == 0x825AC688 && waitTraceCount.fetch_add(1, std::memory_order_relaxed) < 8)
        std::cerr << "wait wrapper entry r3=0x" << std::hex << ctx.r3.u32
                  << " r7=0x" << ctx.r7.u32 << " r8=0x" << ctx.r8.u32
                  << " r28=0x" << ctx.r28.u32 << std::dec << '\n';
    if (address == 0x82381C60)
    {
        static std::atomic<uint32_t> queueTraceCount = 0;
        if (queueTraceCount.fetch_add(1, std::memory_order_relaxed) < 8)
        {
            uint32_t output = 0;
            if (ctx.r4.u32 != 0)
            {
                std::memcpy(&output, base + ctx.r4.u32, sizeof(output));
                output = __builtin_bswap32(output);
            }
            std::cerr << "queue wait r3=0x" << std::hex << ctx.r3.u32
                      << " r4=0x" << ctx.r4.u32 << " *r4=0x" << output
                      << " r5=0x" << ctx.r5.u32 << std::dec << '\n';
        }
    }
    if (address == 0x82382250)
    {
        static std::atomic<uint32_t> allocatorQueueTraceCount = 0;
        if (allocatorQueueTraceCount.fetch_add(1, std::memory_order_relaxed) < 8)
        {
            auto guestWord = [base](uint32_t guestAddress) {
                uint32_t value = 0;
                std::memcpy(&value, base + guestAddress, sizeof(value));
                return __builtin_bswap32(value);
            };
            std::cerr << "allocator queue r3=0x" << std::hex << ctx.r3.u32
                      << " +0=0x" << guestWord(ctx.r3.u32)
                      << " +8=0x" << guestWord(ctx.r3.u32 + 8)
                      << " +13536=0x" << guestWord(ctx.r3.u32 + 13536)
                      << " +14028=0x" << guestWord(ctx.r3.u32 + 14028)
                      << std::dec << '\n';
        }
    }
    if (address == 0x82382390)
    {
        static std::atomic<uint32_t> allocatorSetupTraceCount = 0;
        if (allocatorSetupTraceCount.fetch_add(1, std::memory_order_relaxed) < 16)
        {
            auto guestWord = [base](uint32_t guestAddress) {
                uint32_t value = 0;
                std::memcpy(&value, base + guestAddress, sizeof(value));
                return __builtin_bswap32(value);
            };
            std::cerr << "allocator setup r3=0x" << std::hex << ctx.r3.u32
                      << " +10396=0x" << guestWord(ctx.r3.u32 + 10396)
                      << " +10432=0x" << guestWord(ctx.r3.u32 + 10432)
                      << " +10433=0x" << guestWord(ctx.r3.u32 + 10433)
                      << " +13536=0x" << guestWord(ctx.r3.u32 + 13536)
                      << " +14028=0x" << guestWord(ctx.r3.u32 + 14028)
                      << " +14032=0x" << guestWord(ctx.r3.u32 + 14032)
                      << " +14036=0x" << guestWord(ctx.r3.u32 + 14036)
                      << std::dec << '\n';
        }
    }
    if (address == 0x82566CE0 || address == 0x8256EC30 || address == 0x8256F540)
    {
        static std::atomic<uint32_t> callbackTraceCount = 0;
        if (callbackTraceCount.fetch_add(1, std::memory_order_relaxed) < 12)
        {
            auto guestWord = [base](uint32_t guestAddress) {
                uint32_t value = 0;
                std::memcpy(&value, base + guestAddress, sizeof(value));
                return __builtin_bswap32(value);
            };
            std::cerr << "callback path 0x" << std::hex << address
                      << " r3=0x" << ctx.r3.u32
                      << " r4=0x" << ctx.r4.u32
                      << " r5=0x" << ctx.r5.u32
                      << " r6=0x" << ctx.r6.u32
                      << " r11=0x" << ctx.r11.u32;
            if (ctx.r3.u32 < 0x100000u)
                std::cerr << " [r3]=0x" << guestWord(ctx.r3.u32);
            std::cerr << std::dec << '\n';
        }
    }
    static std::atomic<uint32_t> initTraceCount = 0;
    if ((address == 0x820A3AF0 || address == 0x8211A958 || address == 0x8211B0D0) &&
        initTraceCount.fetch_add(1, std::memory_order_relaxed) < 12)
        std::cerr << "init function entry 0x" << std::hex << address
                  << " r3=0x" << ctx.r3.u32 << std::dec << '\n';
    static std::atomic<uint32_t> allocatorTraceCount = 0;
    if ((address == 0x820D59E0 || address == 0x820BF280 || address == 0x822D5000 || address == 0x82350708 || address == 0x82101820 || address == 0x82365F30) &&
        allocatorTraceCount.fetch_add(1, std::memory_order_relaxed) < 12)
        std::cerr << "allocator path entry 0x" << std::hex << address
                  << " r3=0x" << ctx.r3.u32 << " r4=0x" << ctx.r4.u32
                  << " r5=0x" << ctx.r5.u32 << " r6=0x" << ctx.r6.u32
                  << " r7=0x" << ctx.r7.u32 << " r8=0x" << ctx.r8.u32
                  << " r9=0x" << ctx.r9.u32 << " r10=0x" << ctx.r10.u32
                  << " manager=0x" << [&] {
                      uint32_t value = 0;
                      std::memcpy(&value, base + 0x82d14ec0u, sizeof(value));
                      return __builtin_bswap32(value);
                  }()
                  << std::dec << '\n';
    static std::atomic<uint32_t> mathInitTraceCount = 0;
    if (address == 0x8226B940 && mathInitTraceCount.fetch_add(1, std::memory_order_relaxed) < 4)
    {
        auto guestWord = [base](uint32_t address) {
            uint32_t value = 0;
            std::memcpy(&value, base + address, sizeof(value));
            return __builtin_bswap32(value);
        };
        std::cerr << "math init entry r3=0x" << std::hex << ctx.r3.u32
                  << " +0=0x" << guestWord(ctx.r3.u32)
                  << " +800=0x" << guestWord(ctx.r3.u32 + 800)
                  << " +808=0x" << guestWord(ctx.r3.u32 + 808)
                  << " +816=0x" << guestWord(ctx.r3.u32 + 816)
                  << std::dec << '\n';
    }
    static std::atomic<uint32_t> slotTraceCount = 0;
    if (address == 0x823500F0 && slotTraceCount.fetch_add(1, std::memory_order_relaxed) < 4)
    {
        auto guestWord = [base](uint32_t address) {
            uint32_t value = 0;
            std::memcpy(&value, base + address, sizeof(value));
            return __builtin_bswap32(value);
        };
        auto guestByte = [base](uint32_t address) { return base[address]; };
        const uint32_t table = guestWord(ctx.r3.u32 + 12);
        const uint32_t index = guestWord(ctx.r3.u32 + 4);
        std::cerr << "slot allocator entry r3=0x" << std::hex << ctx.r3.u32
                  << " index=" << index << " count=" << guestWord(ctx.r3.u32 + 16)
                  << " table=0x" << table << " first30=0x" << static_cast<uint32_t>(guestByte(table + 30))
                  << " second30=0x" << static_cast<uint32_t>(guestByte(table + 62))
                  << " manager=0x" << guestWord(0x82d14ec0)
                  << std::dec << '\n';
    }
    if (address == 0x820DDB88 || address == 0x820B5B28 ||
        address == 0x82355688 || address == 0x82355880)
    {
        auto guestWord = [base](uint32_t guestAddress) {
            uint32_t value = 0;
            std::memcpy(&value, base + guestAddress, sizeof(value));
            return __builtin_bswap32(value);
        };
        std::cerr << "object init entry 0x" << std::hex << address
                  << " r3=0x" << ctx.r3.u32
                  << " global826971c4=0x" << guestWord(0x826971c4)
                  << " global82697264=0x" << guestWord(0x82697264)
                  << std::dec << '\n';
    }
    static std::atomic<uint32_t> renderTraceCount = 0;
    if ((address == 0x8237FCD0 || address == 0x8237FD58 ||
         address == 0x8237FF98 || address == 0x82380FE8 ||
         address == 0x82381688 || address == 0x82382578 ||
         address == 0x82388688) &&
        renderTraceCount.fetch_add(1, std::memory_order_relaxed) < 128)
    {
        std::cerr << "render path entry 0x" << std::hex << address
                  << " r3=0x" << ctx.r3.u32
                  << " r4=0x" << ctx.r4.u32
                  << " r5=0x" << ctx.r5.u32
                  << " r6=0x" << ctx.r6.u32
                  << " r7=0x" << ctx.r7.u32
                  << " r8=0x" << ctx.r8.u32 << std::dec << '\n';
    }
    const uint32_t previous = gPpcLastFunction.exchange(address, std::memory_order_relaxed);
    if (previous != address && gPpcFunctionTransitions.fetch_add(1, std::memory_order_relaxed) < 5000)
    {
        std::cerr << "guest function: 0x" << std::hex << address;
        if (address == 0x825AC688)
            std::cerr << " r3=0x" << ctx.r3.u32 << " r8=0x" << ctx.r8.u32
                      << " r10=0x" << ctx.r10.u32 << " r11=0x" << ctx.r11.u32;
        std::cerr << std::dec << '\n';
    }
}

extern "C" uint32_t PPCGuestClock()
{
    static std::atomic<uint32_t> guestClock = 0;
    return guestClock.fetch_add(1000000, std::memory_order_relaxed) + 1000000;
}

extern "C" void PPCGuestStoreU32(uint8_t* base, uint32_t address, uint32_t value)
{
    if (address >= XenosGpu::kMmioBase && address < XenosGpu::kMmioBase + XenosGpu::kMmioSize)
    {
        PPCGuestMmioStore(base, address, value, 4);
        return;
    }
    const uint32_t encoded = __builtin_bswap32(value);
    std::memcpy(base + address, &encoded, sizeof(encoded));
}

extern "C" void PPCGuestMmioStore(uint8_t* base, uint32_t address, uint64_t value, uint32_t width)
{
    static std::atomic<uint32_t> storeCount = 0;
    const uint32_t sequence = storeCount.fetch_add(1, std::memory_order_relaxed);
    if (std::getenv("XERENGE_PPC_MMIO_TRACE") != nullptr &&
        (sequence < 32 || (sequence % 10000) == 0))
    {
        std::cerr << "guest MMIO store #" << sequence << " address=0x" << std::hex
                  << address << " value=0x" << value << " width=" << std::dec << width << '\n';
    }

    gXenosGpu.write(base, address, value, width);

    // Keep the guest-visible big-endian backing bytes until the command
    // processor is connected.  The hook makes MMIO traffic observable while
    // preserving the old memory behavior for code that reads the register
    // shadow back immediately.
    switch (width)
    {
    case 1:
        base[address] = static_cast<uint8_t>(value);
        break;
    case 2:
    {
        const uint16_t encoded = __builtin_bswap16(static_cast<uint16_t>(value));
        std::memcpy(base + address, &encoded, sizeof(encoded));
        break;
    }
    case 4:
    {
        const uint32_t encoded = __builtin_bswap32(static_cast<uint32_t>(value));
        std::memcpy(base + address, &encoded, sizeof(encoded));
        break;
    }
    case 8:
    {
        const uint64_t encoded = __builtin_bswap64(value);
        std::memcpy(base + address, &encoded, sizeof(encoded));
        break;
    }
    default:
        break;
    }
}

class XboxServiceLayer
{
public:
    void invoke(std::string_view service, PPCContext& ctx, uint8_t* base)
    {
        ++gPpcServiceCalls;
        if (service.compare(0, 7, "__imp__") == 0)
            service.remove_prefix(7);

        // Keep service discovery useful without turning a boot trace into an
        // unbounded log. A successful stub return can otherwise hide the
        // first missing asset operation before the render loop.
        if (std::getenv("XERENGE_SERVICE_TRACE") != nullptr)
        {
            static std::atomic<uint32_t> serviceTraceCount = 0;
            if (serviceTraceCount.fetch_add(1, std::memory_order_relaxed) < 256)
                std::cerr << "Xbox service " << service
                          << " r3=0x" << std::hex << ctx.r3.u32
                          << " r4=0x" << ctx.r4.u32
                          << " r5=0x" << ctx.r5.u32
                          << " r6=0x" << ctx.r6.u32
                          << " r7=0x" << ctx.r7.u32
                          << " r8=0x" << ctx.r8.u32 << std::dec << '\n';
        }

        if (service.size() >= 2 && service[0] == 'V' && service[1] == 'd')
        {
            static std::atomic<uint32_t> vdTraceCount = 0;
            if (std::getenv("XERENGE_PPC_TRACE") != nullptr &&
                vdTraceCount.fetch_add(1, std::memory_order_relaxed) < 64)
                std::cerr << "Xbox video service " << service
                          << " r3=0x" << std::hex << ctx.r3.u32
                          << " r4=0x" << ctx.r4.u32 << std::dec << '\n';
        }

        if (service == "VdInitializeRingBuffer")
        {
            gXenosGpu.initializeRingBuffer(ctx.r3.u32, ctx.r4.u32);
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "ExCreateThread")
        {
            // Xbox ABI: r3 is the output handle, r5 is the optional thread
            // id, r7 is the guest start address and r8 its context.  The
            // The xapi startup trampoline in r6 is itself a recompiled guest
            // function.  Entering it preserves the title's TLS and callback
            // setup before PPCDispatchIndirect invokes the requested start
            // address.
            const uint32_t handleAddress = ctx.r3.u32;
            const uint32_t threadIdAddress = ctx.r5.u32;
            const uint32_t startAddress = ctx.r7.u32;
            const uint32_t startContext = ctx.r8.u32;
            if (std::getenv("XERENGE_PPC_TRACE") != nullptr)
                std::cerr << "ExCreateThread handle=0x" << std::hex << handleAddress
                          << " startup=0x" << ctx.r6.u32
                          << " start=0x" << startAddress
                          << " context=0x" << startContext
                          << " flags=0x" << ctx.r9.u32 << std::dec << '\n';
            const uint32_t handle = createObject(base);
            if (handleAddress != 0)
                storeU32(base, handleAddress, handle);
            const uint32_t threadId = nextThreadId_.fetch_add(1, std::memory_order_relaxed);
            if (threadIdAddress != 0)
                storeU32(base, threadIdAddress, threadId);
            if (handle != 0 && startAddress != 0)
                launchGuestThread(base, ctx.r6.u32, startAddress, startContext, threadId);
            ctx.r3.u32 = handle != 0 ? 0 : 0xC0000017u;
            return;
        }
        if (service == "NtCreateTimer")
        {
            const uint32_t outputHandle = ctx.r3.u32;
            const uint32_t handle = createObject(base);
            if (outputHandle != 0)
                storeU32(base, outputHandle, handle);
            ctx.r3.u32 = handle != 0 ? 0u : 0xC0000017u;
            return;
        }
        if (service == "ObReferenceObjectByHandle")
        {
            const uint32_t outputObject = ctx.r6.u32;
            if (outputObject != 0)
                storeU32(base, outputObject, ctx.r3.u32);
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "NtResumeThread" || service == "NtSetTimerEx" ||
            service == "NtWaitForSingleObjectEx" || service == "KeDelayExecutionThread")
        {
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "RtlNtStatusToDosError")
        {
            ctx.r3.u32 = ctx.r3.u32 == 0 ? 0u : 1u;
            return;
        }
        if (service == "MmAllocatePhysicalMemoryEx")
        {
            ctx.r3.u32 = allocatePhysical(std::max<uint32_t>(ctx.r4.u32, 0x1000u), base);
            return;
        }
        if (service == "MmGetPhysicalAddress")
        {
            // The first guest mapping is identity-addressed by the runtime;
            // preserve the Xenon virtual address as its physical token.
            return;
        }
        if (service == "MmFreePhysicalMemory")
        {
            // r4 carries the physical allocation in the Xbox ABI.
            if (ctx.r4.u32 != 0)
                allocations_.erase(ctx.r4.u32);
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "MmQueryAllocationSize")
        {
            const auto it = allocations_.find(ctx.r3.u32);
            ctx.r3.u32 = it == allocations_.end() ? 0 : it->second;
            return;
        }
        if (service == "VdGetSystemCommandBuffer")
        {
            // Xenia exposes these as stable guest tokens.  The title passes
            // them back to VdSwap and uses the first value as the command
            // buffer identity, so keep the documented ABI values here.
            if (ctx.r3.u32 != 0)
                storeU32(base, ctx.r3.u32, 0xBEEF0000u);
            if (ctx.r4.u32 != 0)
                storeU32(base, ctx.r4.u32, 0xBEEF0001u);
            gXenosGpu.initializeRingBuffer(0xBEEF0000u, 16);
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "VdSwap")
        {
            const uint32_t buffer = ctx.r3.u32;
            const uint32_t fetch = ctx.r4.u32;
            const uint32_t frontbuffer = ctx.r8.u32 != 0 ? loadU32(base, ctx.r8.u32) : 0;
            const uint32_t fetch0 = loadU32(base, fetch + 0);
            const uint32_t fetch1 = loadU32(base, fetch + 4);
            const uint32_t fetch2 = loadU32(base, fetch + 8);
            const uint32_t fetch3 = loadU32(base, fetch + 12);
            const uint32_t fetch4 = loadU32(base, fetch + 16);
            const uint32_t fetch5 = loadU32(base, fetch + 20);
            const uint32_t fallbackWidth = (fetch2 & 0x1FFFu) + 1;
            const uint32_t fallbackHeight = ((fetch2 >> 13) & 0x1FFFu) + 1;
            const uint32_t width = ctx.r11.u32 != 0 ? loadU32(base, ctx.r11.u32) : fallbackWidth;
            const uint32_t heightPointer = loadU32(base, ctx.r1.u32 + 84);
            const uint32_t height = heightPointer != 0 ? loadU32(base, heightPointer) : fallbackHeight;

            // VdSwap reserves 64 dwords in the primary ring and fills it with
            // a fetch update followed by Xenia's observable XE_SWAP packet.
            // The same PM4 layout is understood by the Xenos command parser.
            storeU32(base, buffer + 0, (5u << 16) | 0x4000u);
            storeU32(base, buffer + 4, fetch0);
            storeU32(base, buffer + 8, fetch1);
            storeU32(base, buffer + 12, fetch2);
            storeU32(base, buffer + 16, fetch3);
            storeU32(base, buffer + 20, fetch4);
            storeU32(base, buffer + 24, fetch5);
            storeU32(base, buffer + 28, (3u << 30) | (3u << 16) | (0x64u << 8));
            storeU32(base, buffer + 32, 0x53574150u); // 'SWAP'
            storeU32(base, buffer + 36, frontbuffer);
            storeU32(base, buffer + 40, width);
            storeU32(base, buffer + 44, height);
            for (uint32_t i = 12; i < 64; ++i)
                storeU32(base, buffer + i * 4, 0x80000000u);
            gXenosGpu.processSubmittedBuffer(base, buffer, 64);
            if (std::getenv("XERENGE_PPC_TRACE") != nullptr)
            {
                static std::atomic<uint32_t> swapTraceCount = 0;
                if (swapTraceCount.fetch_add(1, std::memory_order_relaxed) < 8)
                {
                    std::cerr << "VdSwap command buffer 0x" << std::hex << ctx.r3.u32 << ":";
                    for (uint32_t i = 0; i < 8; ++i)
                        std::cerr << " " << loadU32(base, ctx.r3.u32 + i * 4);
                    std::cerr << " fetch=0x" << ctx.r4.u32 << ":";
                    for (uint32_t i = 0; i < 6; ++i)
                        std::cerr << " " << loadU32(base, ctx.r4.u32 + i * 4);
                    std::cerr << " r5=0x" << ctx.r5.u32
                              << " r6=0x" << ctx.r6.u32
                              << " r7=0x" << ctx.r7.u32
                              << " r8=0x" << ctx.r8.u32
                              << " r9=0x" << ctx.r9.u32
                              << " r10=0x" << ctx.r10.u32
                              << std::dec << " packets=" << gXenosGpu.packetCount()
                              << " draws=" << gXenosGpu.drawPacketCount()
                              << " swaps=" << gXenosGpu.swapPacketCount()
                              << " frames=" << gXenosGpu.frameCount()
                              << " size=" << gXenosGpu.lastFrameWidth() << 'x'
                              << gXenosGpu.lastFrameHeight() << '\n';
                }
            }
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "VdSetDisplayMode")
        {
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "VdGetCurrentDisplayGamma")
        {
            if (ctx.r3.u32 != 0)
                storeU32(base, ctx.r3.u32, 2);
            if (ctx.r4.u32 != 0)
            {
                const uint32_t gamma = 0x400E38E4u; // 2.22222233f, BE
                std::memcpy(base + ctx.r4.u32, &gamma, sizeof(gamma));
            }
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "VdEnableRingBufferRPtrWriteBack")
        {
            gXenosGpu.enableReadPointerWriteBack(ctx.r3.u32, ctx.r4.u32);
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "VdInitializeEngines")
        {
            ctx.r3.u32 = 1;
            return;
        }
        if (service == "VdGetGraphicsAsicID")
        {
            ctx.r3.u32 = 0x11;
            return;
        }
        if (service == "VdIsHSIOTrainingSucceeded")
        {
            ctx.r3.u32 = 1;
            return;
        }
        if (service == "VdQueryVideoFlags")
        {
            ctx.r3.u32 = 3;
            return;
        }
        if (service == "VdInitializeEDRAM")
        {
            ctx.r3.u32 = 1;
            return;
        }
        if (service == "VdRetrainEDRAM" || service == "VdRetrainEDRAMWorker" ||
            service == "VdEnableDisableClockGating" || service == "VdShutdownEngines" ||
            service == "VdSetGraphicsInterruptCallback" ||
            service == "VdSetSystemCommandBufferGpuIdentifierAddress" ||
            service == "VdPersistDisplay" || service == "VdCallGraphicsNotificationRoutines")
        {
            ctx.r3.u32 = 0;
            return;
        }

        if (service == "XamAlloc" || service == "ExAllocatePoolWithTag" || service == "RtlAllocateHeap")
        {
            const uint32_t size = std::max<uint32_t>(ctx.r3.u32, 1);
            ctx.r3.u32 = allocate(size, base);
            return;
        }
        if (service == "NtAllocateVirtualMemory")
        {
            // NT signature: process, *base, zeroBits, *size, allocationType,
            // protection.  The title uses the current process pseudo-handle;
            // guest pointers in r4/r6 carry the requested range.
            const uint32_t sizeAddress = ctx.r6.u32;
            const uint32_t requested = sizeAddress != 0 ? loadU32(base, sizeAddress) : 0;
            const uint32_t size = std::max<uint32_t>(requested, 0x1000u);
            const uint32_t allocation = allocate(size, base);
            if (ctx.r4.u32 != 0)
                storeU32(base, ctx.r4.u32, allocation);
            if (sizeAddress != 0)
                storeU32(base, sizeAddress, size);
            ctx.r3.u32 = allocation != 0 ? 0 : 0xC0000017u; // STATUS_NO_MEMORY
            return;
        }
        if (service == "XamFree" || service == "ExFreePool" || service == "RtlFreeHeap")
        {
            allocations_.erase(ctx.r3.u32);
            ctx.r3.u32 = 1;
            return;
        }
        if (service == "XamContentCreate" || service == "XamContentCreateEnumerator" ||
            service == "XamNotifyCreateListener" || service == "XamSessionCreateHandle" ||
            service == "XamVoiceCreate" || service == "XMACreateContext")
        {
            ctx.r3.u32 = createObject(base);
            return;
        }
        if (service == "XamContentClose" || service == "XamVoiceClose" || service == "NtClose" ||
            service == "ObDereferenceObject")
        {
            releaseObject(ctx.r3.u32);
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "RtlInitializeCriticalSection")
        {
            clear(base, ctx.r3.u32, 0x20);
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "RtlEnterCriticalSection" || service == "RtlLeaveCriticalSection")
        {
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "KeTlsAlloc")
        {
            for (uint32_t index = 0; index < tlsSlots_.size(); ++index)
            {
                if (!tlsUsed_[index])
                {
                    tlsUsed_[index] = true;
                    tlsSlots_[index] = 0;
                    ctx.r3.u32 = index;
                    return;
                }
            }
            ctx.r3.u32 = 0xffffffffu;
            return;
        }
        if (service == "KeTlsFree")
        {
            if (ctx.r3.u32 < tlsSlots_.size())
            {
                tlsUsed_[ctx.r3.u32] = false;
                tlsSlots_[ctx.r3.u32] = 0;
            }
            ctx.r3.u32 = 1;
            return;
        }
        if (service == "KeTlsSetValue")
        {
            if (ctx.r3.u32 < tlsSlots_.size() && tlsUsed_[ctx.r3.u32])
                tlsSlots_[ctx.r3.u32] = ctx.r4.u32;
            ctx.r3.u32 = 1;
            return;
        }
        if (service == "KeTlsGetValue")
        {
            ctx.r3.u32 = ctx.r3.u32 < tlsSlots_.size() && tlsUsed_[ctx.r3.u32]
                ? tlsSlots_[ctx.r3.u32] : 0;
            return;
        }
        if (service == "KeGetCurrentProcessType")
        {
            ctx.r3.u32 = 1;
            return;
        }
        if (service == "XGetLanguage")
        {
            ctx.r3.u32 = 1; // English, the neutral title default.
            return;
        }
        if (service == "XGetGameRegion" || service == "XGetVideoMode")
        {
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "RtlInitAnsiString")
        {
            const uint32_t destination = ctx.r3.u32;
            const uint32_t source = ctx.r4.u32;
            uint32_t length = 0;
            if (destination != 0 && source != 0)
            {
                while (length < 0x1000u && base[source + length] != 0)
                    ++length;
                storeU16(base, destination, static_cast<uint16_t>(std::min<uint32_t>(length, 0xFFFFu)));
                storeU16(base, destination + 2,
                    static_cast<uint16_t>(std::min<uint32_t>(length + 1, 0xFFFFu)));
                storeU32(base, destination + 4, source);
            }
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "KeBugCheck" || service == "HalReturnToFirmware")
        {
            // Keep the guest alive while the platform bootstrap is emulated.
            ctx.r3.u32 = 0;
            return;
        }

        if (gPpcServiceCalls <= 40)
            std::cerr << "unimplemented Xbox service: " << service << '\n';
        ctx.r3.u32 = 0xC0000001u; // STATUS_UNSUCCESSFUL
    }

    bool invokeCallback(uint32_t address, PPCContext& ctx)
    {
        if (address < kVtableBase || address >= kVtableBase + 0x220)
            return false;

        const uint32_t method = (address - kVtableBase) / 4;
        const uint32_t object = ctx.r3.u32;
        auto it = objects_.find(object);
        if (it == objects_.end())
        {
            ctx.r3.u32 = 0xC000000Du; // STATUS_INVALID_PARAMETER
            return true;
        }
        if (method == 0)
        {
            ++it->second;
            ctx.r3.u32 = 1;
        }
        else if (method == 1)
        {
            releaseObject(object);
            ctx.r3.u32 = 0;
        }
        else
        {
            // Most methods used during early title bring-up are notification
            // and configuration calls.  They return success while preserving
            // the object in r3 for the following guest call.
            ctx.r3.u32 = 0;
        }
        return true;
    }

    uint32_t materializeNullObject(PPCContext& ctx, uint8_t* base)
    {
        const uint32_t slot = ctx.r3.u32;
        if (slot < 0x82000000u || slot >= 0x90000000u)
            return createObject(base);
        const uint32_t existing = loadU32(base, slot);
        if (existing != 0)
            return existing;
        const uint32_t object = createObject(base);
        if (object != 0)
            storeU32(base, slot, object);
        return object;
    }

    uint32_t materializeGlobalObject(uint32_t slot, uint8_t* base)
    {
        const uint32_t existing = loadU32(base, slot);
        if (existing != 0)
            return existing;
        const uint32_t object = createObject(base);
        if (object != 0)
            storeU32(base, slot, object);
        return object;
    }

    static constexpr uint32_t vtableBase() { return kVtableBase; }

    uint32_t allocateGuest(uint32_t size, uint8_t* base)
    {
        return allocate(std::max<uint32_t>(size, 0x1000u), base);
    }

    static void writeGuestU32(uint8_t* base, uint32_t address, uint32_t value)
    {
        storeU32(base, address, value);
    }

    static uint32_t readGuestU32(const uint8_t* base, uint32_t address)
    {
        return loadU32(base, address);
    }

private:
    uint32_t allocatePhysical(uint32_t size, uint8_t* base)
    {
        constexpr uint32_t alignment = 0x1000u;
        heapCursor_ = (heapCursor_ + alignment - 1) & ~(alignment - 1);
        return allocate(size, base);
    }

    void launchGuestThread(uint8_t* base, uint32_t startupAddress, uint32_t startAddress,
        uint32_t startContext, uint32_t threadId)
    {
        std::thread([base, startupAddress, startAddress, startContext, threadId]
        {
            PPCContext threadContext{};
            threadContext.r1.u32 = 0x70000000u - ((threadId & 0xFFu) * 0x10000u);
            threadContext.r3.u32 = startAddress;
            threadContext.r4.u32 = startContext;
            PPCDispatchIndirect(threadContext, base, startupAddress);
        }).detach();
    }

    uint32_t allocate(uint32_t size, uint8_t* base)
    {
        constexpr uint32_t alignment = 16;
        const uint32_t alignedSize = (size + alignment - 1) & ~(alignment - 1);
        if (heapCursor_ > heapLimit_ - alignedSize)
            return 0;
        const uint32_t address = heapCursor_;
        heapCursor_ += alignedSize;
        allocations_.emplace(address, alignedSize);
        std::memset(base + address, 0, alignedSize);
        return address;
    }

    static void clear(uint8_t* base, uint32_t address, uint32_t size)
    {
        if (address != 0)
            std::memset(base + address, 0, size);
    }

    uint32_t createObject(uint8_t* base)
    {
        constexpr uint32_t kVtableSize = 0x220;
        const uint32_t object = allocate(0x20, base);
        if (object == 0)
            return 0;
        storeU32(base, object, kVtableBase);
        allocateVtable(base, kVtableSize);
        for (uint32_t offset = 0; offset < kVtableSize; offset += 4)
            storeU32(base, kVtableBase + offset, kVtableBase + offset);
        objects_.emplace(object, 1);
        return object;
    }

    void allocateVtable(uint8_t* base, uint32_t size)
    {
        // The synthetic vtable lives in the fixed guest callback window.  Its
        // backing bytes are already inside the sparse guest address space;
        // reserve the range from the service heap so object creation cannot
        // overlap it.
        if (!vtableAllocated_)
        {
            std::memset(base + kVtableBase, 0, size);
            vtableAllocated_ = true;
        }
    }

    void releaseObject(uint32_t object)
    {
        auto it = objects_.find(object);
        if (it == objects_.end())
            return;
        if (it->second > 1)
            --it->second;
        else
        {
            objects_.erase(it);
            allocations_.erase(object);
        }
    }

    static void storeU32(uint8_t* base, uint32_t address, uint32_t value)
    {
        const uint32_t bigEndianValue = __builtin_bswap32(value);
        std::memcpy(base + address, &bigEndianValue, sizeof(bigEndianValue));
    }

    static void storeU16(uint8_t* base, uint32_t address, uint16_t value)
    {
        const uint16_t bigEndianValue = __builtin_bswap16(value);
        std::memcpy(base + address, &bigEndianValue, sizeof(bigEndianValue));
    }

    static uint32_t loadU32(const uint8_t* base, uint32_t address)
    {
        uint32_t value = 0;
        std::memcpy(&value, base + address, sizeof(value));
        return __builtin_bswap32(value);
    }

    uint32_t heapCursor_ = 0x60000000u;
    // Early Burnout allocates large physical video heaps (over 200 MiB)
    // before creating the primary command ring.
    static constexpr uint32_t heapLimit_ = 0x78000000u;
    static constexpr uint32_t kVtableBase = 0x81000000u;
    std::unordered_map<uint32_t, uint32_t> allocations_;
    std::unordered_map<uint32_t, uint32_t> objects_;
    bool vtableAllocated_ = false;
    std::array<uint32_t, 64> tlsSlots_{};
    std::array<bool, 64> tlsUsed_{};
    std::atomic<uint32_t> nextThreadId_{1};
};

XboxServiceLayer gXboxServices;

extern "C" uint32_t PPCMaterializeObject(PPCContext& ctx, uint8_t* base)
{
    return gXboxServices.materializeNullObject(ctx, base);
}

extern "C" void PPCImportedServiceTrap(const char* service, PPCContext& ctx, uint8_t* base)
{
    gXboxServices.invoke(service, ctx, base);
}

extern "C" void PPCUnknownIndirectTrap(uint32_t address, PPCContext& ctx, uint8_t* base)
{
    if (gXboxServices.invokeCallback(address, ctx))
        return;
    if (ctx.lr == 0x82382180u)
    {
        // Growable title allocators pass the output byte count in r6 and the
        // minimum element count in r7.  Returning a COM placeholder here
        // leaves the ring capacity unchanged and causes an endless grow loop.
        const uint32_t size = std::max<uint32_t>(ctx.r7.u32 * 0x80u, 0x1000u);
        ctx.r3.u32 = gXboxServices.allocateGuest(size, base);
        if (ctx.r6.u32 != 0)
            XboxServiceLayer::writeGuestU32(base, ctx.r6.u32, size);
        return;
    }
    if (ctx.lr == 0x823816D8u)
    {
        // sub_82381688 dispatches its backing allocator through an object
        // callback at this LR.  r6 points at the stack temporary containing
        // the requested byte count; the callback returns the allocation in
        // r3 and writes the actual size back through r6.
        const uint32_t requested = ctx.r6.u32 != 0
            ? XboxServiceLayer::readGuestU32(base, ctx.r6.u32)
            : 0;
        const uint32_t size = std::max<uint32_t>(requested, 0x1000u);
        ctx.r3.u32 = gXboxServices.allocateGuest(size, base);
        if (ctx.r6.u32 != 0)
            XboxServiceLayer::writeGuestU32(base, ctx.r6.u32, size);
        return;
    }
    if (ctx.lr >= 0x82355700u && ctx.lr <= 0x82355818u)
    {
        // 0x826971c4 is the title's static service object.  Its constructor
        // is reached before the platform object backing it is supplied on the
        // retail boot path.  Materialize the Xbox-compatible object at the
        // first virtual call and let subsequent calls use its synthetic vtable.
        const uint32_t object = gXboxServices.materializeGlobalObject(0x826971c4u, base);
        ctx.r3.u32 = object;
        return;
    }
    if (address == 0)
    {
        const uint32_t object = gXboxServices.materializeNullObject(ctx, base);
        ctx.r3.u32 = object;
        if (ctx.r30.u32 < 0x10000000u)
            ctx.r30.u32 = object;
        return;
    }
    if (address >= PPC_IMAGE_BASE && address < PPC_CODE_BASE)
    {
        if (gPpcUnknownIndirectCalls <= 40)
            std::cerr << "unsupported Xbox callback address below recompiled code: 0x"
                      << std::hex << address << std::dec << '\n';
        ctx.r3.u32 = 0;
        return;
    }
    ++gPpcUnknownIndirectCalls;
    if (gPpcUnknownIndirectCalls <= 40)
    {
        std::cerr << "unresolved PPC indirect target: 0x" << std::hex << address
                  << " from lr=0x" << ctx.lr << " r1=0x" << ctx.r1.u64
                  << " r3=0x" << ctx.r3.u64 << " r4=0x" << ctx.r4.u64
                  << " r11=0x" << ctx.r11.u64 << " r30=0x" << ctx.r30.u64
                  << " r31=0x" << ctx.r31.u64 << std::dec << '\n';
    }
    ctx.r3.u64 = 0;
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
        const uint32_t characteristics = readLE32(section + 36);
        const uint32_t mappedSize = std::max(virtualSize, rawSize);
        if (virtualAddress > imageSize || mappedSize > imageSize - virtualAddress)
        {
            std::cerr << "PE section exceeds guest image: " << i << '\n';
            return std::nullopt;
        }
        // XEX decompression produces an image laid out by virtual address;
        // the PE raw offsets describe the original file packaging and do not
        // identify the source bytes in this decoded buffer.
        const uint32_t sourceOffset = virtualAddress;
        if (sourceOffset >= image.size())
        {
            // XEX may omit PE bookkeeping sections such as relocations from
            // the decoded load image. They are not needed for a preferred-base
            // guest mapping.
            continue;
        }
        const uint32_t availableSize = static_cast<uint32_t>(image.size() - sourceOffset);
        const uint32_t copySize = std::min(rawSize, availableSize);
        if (copySize == 0)
        {
            std::cerr << "PE section data exceeds decoded image: " << i << '\n';
            return std::nullopt;
        }
        std::copy_n(image.begin() + sourceOffset, copySize, mapped.memory.begin() + virtualAddress);
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

// This guest helper registers a cleanup record. The generated implementation
// is weak; keep the loader ABI while the host owns that lifecycle.
#ifdef XERENGE_HAS_PPC
void sub_8259D4B0(PPCContext& ctx, uint8_t*)
{
    // The host owns cleanup records; the guest helper reports success.
    ctx.r3.u32 = 0;
}

// The title's early loader pass walks an Xbox-owned import descriptor. The
// descriptor is not part of the mapped XEX image yet, so keep this pass
// side-effect free until the service layer supplies it.
void sub_82359D48(PPCContext& ctx, uint8_t*)
{
    ctx.r3.u64 = 0;
}
#endif

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
        gPpcTraceEnabled.store(std::getenv("XERENGE_PPC_TRACE") != nullptr, std::memory_order_relaxed);
        if (std::getenv("XERENGE_PPC_BACKTRACE") != nullptr)
        {
            ::signal(SIGALRM, ppcWatchdogSignal);
            ::alarm(3);
        }
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
