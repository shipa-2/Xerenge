#include <GLFW/glfw3.h>
#include <GL/gl.h>
#include <openssl/evp.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <unordered_set>
#include <vector>

#ifdef XERENGE_HAS_PPC
#include <sys/mman.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>

#include "ppc_recomp_shared.h"
#include "shader_cache_runtime.h"
#include "xenos_gpu.h"
#include "xaudio_backend.h"
#endif
#include "xbox_media.h"

XboxMedia gXboxMedia;

namespace
{
std::atomic<bool> gHostCloseRequested{false};
std::atomic<bool> gDeviceSelectorCompleted{false};

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
        // Keep guest stacks above the physical allocation arena. Burnout's
        // two early video heaps span 0x60000000..0x78110000.
        context_.r1.u64 = 0x81FF0000u;
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
std::atomic<uint64_t> gPpcServiceCalls = 0;
uint64_t gPpcUnknownIndirectCalls = 0;
std::atomic<uint32_t> gPpcLastFunction = 0;
std::atomic<uint64_t> gPpcFunctionTransitions = 0;
std::atomic<uint64_t> gPpcFunctionCalls = 0;
std::atomic<bool> gPpcTraceEnabled = false;
std::atomic<uint32_t> gPpcEntryFunction = 0;
std::atomic<uint32_t> gPpcEntryCaller = 0;
std::atomic<uint64_t> gPpcEntryFunctionCalls = 0;
std::atomic<uint32_t> gResourceStateWatchAddress = 0;
std::atomic<uint16_t> gInputButtons = 0;
thread_local bool gPpcIsEntryThread = false;
thread_local std::array<uint32_t, 64> gPpcTlsValues{};
thread_local uint32_t gPpcCurrentFunction = 0;
thread_local uint32_t gPpcCurrentCaller = 0;
thread_local std::array<uint32_t, 5> gPpcLastDataRoutineArgs{};
thread_local uint32_t gPpcLastDataRoutine = 0;
XenosGpu gXenosGpu;
std::atomic<uint32_t> gGraphicsInterruptCallback = 0;
std::atomic<uint32_t> gGraphicsInterruptContext = 0;
std::atomic<uint32_t> gGraphicsWaitEvent = 0;
std::atomic<bool> gGraphicsWaitEventSignaled = false;
thread_local bool gInGraphicsInterruptCallback = false;

void dispatchGraphicsInterrupt(uint8_t* base)
{
    const uint32_t waitEvent = gGraphicsWaitEvent.load(std::memory_order_acquire);
    if (waitEvent != 0)
    {
        bool wasSignaled = false;
        if (!gGraphicsWaitEventSignaled.compare_exchange_strong(
                wasSignaled, true, std::memory_order_acq_rel))
            return;
    }
    const uint32_t callback = gGraphicsInterruptCallback.load(std::memory_order_acquire);
    const uint32_t context = gGraphicsInterruptContext.load(std::memory_order_acquire);
    if (callback == 0 || gInGraphicsInterruptCallback)
        return;

    gInGraphicsInterruptCallback = true;
    PPCContext interrupt{};
    interrupt.r1.u32 = 0x81FE0000u;
    interrupt.r3.u32 = 1; // Xenos interrupt notification: command processor.
    interrupt.r4.u32 = context;
    if (std::getenv("XERENGE_PPC_TRACE") != nullptr)
    {
        static std::atomic<uint32_t> traceCount = 0;
        if (traceCount.fetch_add(1, std::memory_order_relaxed) < 16)
            std::cerr << "graphics interrupt callback=0x" << std::hex << callback
                      << " context=0x" << context << std::dec << '\n';
    }
    PPCDispatchIndirect(interrupt, base, callback);
    gInGraphicsInterruptCallback = false;
}

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
    static const bool entryTraceEnabled = std::getenv("XERENGE_ENTRY_TRACE") != nullptr;
    static const bool frontendPrepareTraceEnabled =
        std::getenv("XERENGE_FRONTEND_PREPARE_TRACE") != nullptr;
    if (frontendPrepareTraceEnabled)
    {
        uint32_t encodedPointer = 0;
        std::memcpy(&encodedPointer, base + 0x825EACE8u + 3760u, sizeof(encodedPointer));
        const uint32_t pointer = __builtin_bswap32(encodedPointer);
        static thread_local uint32_t previousPointer = UINT32_MAX;
        if (pointer != previousPointer)
        {
            std::cerr << "Memory block 19 pointer transition 0x" << std::hex
                      << previousPointer << " -> 0x" << pointer << " at function=0x"
                      << address << " caller=0x" << static_cast<uint32_t>(ctx.lr)
                      << std::dec << '\n';
            previousPointer = pointer;
        }
    }
    if (frontendPrepareTraceEnabled && address == 0x82114A48u)
    {
        static std::atomic<uint32_t> previousState{UINT32_MAX};
        static std::atomic<uint32_t> stateThreeSamples{0};
        uint32_t state = 0;
        std::memcpy(&state, base + ctx.r3.u32 + 48, sizeof(state));
        state = __builtin_bswap32(state);
        const uint32_t old = previousState.exchange(state, std::memory_order_relaxed);
        if (old != state)
            std::cerr << "Frontend prepare state=" << state << " object=0x"
                      << std::hex << ctx.r3.u32 << std::dec << '\n';
        if (state == 3 && stateThreeSamples.fetch_add(1, std::memory_order_relaxed) % 10000u == 0)
        {
            auto read32 = [base](uint32_t address) {
                uint32_t value = 0;
                std::memcpy(&value, base + address, sizeof(value));
                return __builtin_bswap32(value);
            };
            constexpr uint32_t flash = 0x82A528B0u;
            constexpr uint32_t loader = 0x82847090u;
            const uint32_t movie = read32(flash + 1012);
            const uint32_t slot = read32(flash + (movie + 150u) * 4u);
            const uint32_t request = read32(loader + 2216) * 92u + loader;
            const uint32_t file = read32(loader + 2208);
            std::cerr << "Flash prepare movie=" << movie << " loaded="
                      << static_cast<uint32_t>(base[flash + 755]) << " slot=0x"
                      << std::hex << slot << " loaderFile=0x" << file
                      << " readIndex=" << read32(loader + 2216)
                      << " writeIndex=" << read32(loader + 2220)
                      << " decompressStarted="
                      << static_cast<uint32_t>(base[0x82825C35u])
                      << " decompressBusy="
                      << static_cast<uint32_t>(base[0x82825C36u])
                      << " requestState=0x" << read32(request + 76)
                      << " requestDone=0x" << read32(request + 80);
            if (file != 0)
                std::cerr << " fileStatus=" << std::dec << read32(file + 32)
                          << " fileCommand=" << read32(file + 320)
                          << " remaining=0x" << std::hex << read32(file + 56);
            std::cerr << std::dec << '\n';
        }
    }
    if (frontendPrepareTraceEnabled && address == 0x8210DDC0u)
    {
        static std::atomic<uint32_t> loaderUpdates{0};
        const uint32_t count = loaderUpdates.fetch_add(1, std::memory_order_relaxed);
        if (count < 8 || count % 100u == 0)
        {
            std::cerr << "Async loader update=" << count << " caller=0x" << std::hex
                      << ctx.lr << " object=0x" << ctx.r3.u32;
            auto read32 = [base](uint32_t address) {
                uint32_t value = 0;
                std::memcpy(&value, base + address, sizeof(value));
                return __builtin_bswap32(value);
            };
            const uint32_t request = read32(ctx.r3.u32 + 2216u) * 92u + ctx.r3.u32;
            std::cerr << " readIndex=" << read32(ctx.r3.u32 + 2216u)
                      << " writeIndex=" << read32(ctx.r3.u32 + 2220u)
                      << " file=0x" << read32(ctx.r3.u32 + 2208u)
                      << " requestState=0x" << read32(request + 76u)
                      << " requestKind=0x" << read32(request + 68u)
                      << " requestBuffer=0x" << read32(request + 72u);
            const uint32_t file = read32(ctx.r3.u32 + 2208u);
            if (file != 0)
            {
                const uint32_t vtable = read32(file);
                std::cerr << " fileVtable=0x" << vtable
                          << " poll=0x" << read32(vtable + 28u)
                          << " start=0x" << read32(vtable + 8u)
                          << " close=0x" << read32(vtable + 4u);
                if (count < 4)
                {
                    std::cerr << " bytes=";
                    for (uint32_t offset = 32; offset < 112; offset += 4)
                        std::cerr << std::hex << read32(file + offset) << ',';
                }
            }
            std::cerr
                      << std::dec << '\n';
        }
    }
    if (frontendPrepareTraceEnabled && address == 0x821017D8u)
    {
        auto read32 = [base](uint32_t guestAddress) {
            uint32_t value = 0;
            std::memcpy(&value, base + guestAddress, sizeof(value));
            return __builtin_bswap32(value);
        };
        std::cerr << "loader media check object=0x" << std::hex << ctx.r3.u32
                  << " field48=0x" << read32(ctx.r3.u32 + 52u)
                  << " caller=0x" << static_cast<uint32_t>(ctx.lr)
                  << std::dec << '\n';
    }
    if (frontendPrepareTraceEnabled &&
        (address == 0x82369920u || address == 0x8235F370u))
    {
        static std::atomic<uint32_t> dvdTraceCount{0};
        if (dvdTraceCount.fetch_add(1, std::memory_order_relaxed) < 12)
        {
            auto read32 = [base](uint32_t guestAddress) {
                uint32_t value = 0;
                std::memcpy(&value, base + guestAddress, sizeof(value));
                return __builtin_bswap32(value);
            };
            std::cerr << "DVD file method=0x" << std::hex << address
                      << " this=0x" << ctx.r3.u32
                      << " r4=0x" << ctx.r4.u32 << " r5=0x" << ctx.r5.u32
                      << " status=0x" << read32(ctx.r3.u32 + 32u)
                      << " mode=0x" << read32(ctx.r3.u32 + 36u)
                      << " handle=0x" << read32(ctx.r3.u32 + 40u)
                      << " offset=0x" << read32(ctx.r3.u32 + 48u)
                      << " bytes=0x" << read32(ctx.r3.u32 + 56u)
                      << " buffer=0x" << read32(ctx.r3.u32 + 60u)
                      << " backend=0x" << read32(ctx.r3.u32 + 328u)
                      << std::dec << '\n';
        }
    }
    if (frontendPrepareTraceEnabled && address == 0x8211F8D0u)
    {
        static std::atomic<uint32_t> previousGame{UINT32_MAX};
        const uint32_t old = previousGame.exchange(ctx.r3.u32, std::memory_order_relaxed);
        if (old != ctx.r3.u32)
            std::cerr << "CB4Game::Update this=0x" << std::hex << ctx.r3.u32
                      << " caller=0x" << static_cast<uint32_t>(ctx.lr) << std::dec << '\n';
    }
    if (frontendPrepareTraceEnabled && address == 0x82200778u)
    {
        static std::atomic<uint32_t> previousFlashState{UINT32_MAX};
        uint32_t state = 0;
        std::memcpy(&state, base + ctx.r3.u32 + 584, sizeof(state));
        state = __builtin_bswap32(state);
        const uint32_t old = previousFlashState.exchange(state, std::memory_order_relaxed);
        if (old != state)
            std::cerr << "Flash manager prepare state=" << state << " loaded="
                      << static_cast<uint32_t>(base[ctx.r3.u32 + 755]) << '\n';
    }
    if (frontendPrepareTraceEnabled && address == 0x8210CEC0u)
    {
        auto read32 = [base](uint32_t guestAddress) {
            uint32_t value = 0;
            std::memcpy(&value, base + guestAddress, sizeof(value));
            return __builtin_bswap32(value);
        };
        static std::atomic<uint32_t> acquireSamples{0};
        if (acquireSamples.fetch_add(1, std::memory_order_relaxed) < 128)
            std::cerr << "Memory block acquire id=" << ctx.r4.u32 << " manager=0x"
                      << std::hex << ctx.r3.u32 << " caller=0x"
                      << static_cast<uint32_t>(ctx.lr) << std::dec << '\n';
        if (ctx.r4.u32 == 19u)
        {
            static std::atomic<uint64_t> previous{UINT64_MAX};
            const uint64_t state = (uint64_t(read32(ctx.r3.u32 + 3748)) << 32) |
                read32(ctx.r3.u32 + 3760);
            if (previous.exchange(state, std::memory_order_relaxed) != state)
                std::cerr << "Memory block 19 manager=0x" << std::hex << ctx.r3.u32
                          << " lock=0x" << (state >> 32) << " pointer=0x"
                          << static_cast<uint32_t>(state) << std::dec << '\n';
        }
    }
    if (frontendPrepareTraceEnabled && address == 0x82350C50u)
    {
        static std::atomic<uint32_t> samples{0};
        const uint32_t sample = samples.fetch_add(1, std::memory_order_relaxed);
        if (sample < 64)
        {
            auto read32 = [base](uint32_t guestAddress) {
                uint32_t value = 0;
                std::memcpy(&value, base + guestAddress, sizeof(value));
                return __builtin_bswap32(value);
            };
            const uint32_t object = ctx.r3.u32;
            const uint32_t vtable = read32(object);
            std::cerr << "Queue float entry caller=0x" << std::hex
                      << static_cast<uint32_t>(ctx.lr) << " object=0x" << object
                      << " vtable=0x" << vtable << " methods={0x" << read32(vtable)
                      << ",0x" << read32(vtable + 4) << ",0x" << read32(vtable + 8)
                      << ",0x" << read32(vtable + 12) << "}" << std::dec << '\n';
        }
    }
    if (frontendPrepareTraceEnabled && address == 0x8210BFD8u)
        std::cerr << "Memory manager initialize object=0x" << std::hex << ctx.r3.u32
                  << " caller=0x" << static_cast<uint32_t>(ctx.lr) << std::dec << '\n';
    if (frontendPrepareTraceEnabled &&
        (address == 0x8210756Cu || address == 0x821075A0u))
    {
        auto read32 = [base](uint32_t guestAddress) {
            uint32_t value = 0;
            std::memcpy(&value, base + guestAddress, sizeof(value));
            return __builtin_bswap32(value);
        };
        const uint32_t pointerOffset = address == 0x8210756Cu ? 3760u : 3792u;
        std::cerr << "Memory block leaf=0x" << std::hex << address << " manager=0x"
                  << ctx.r11.u32 << " pointer=0x" << read32(ctx.r11.u32 + pointerOffset)
                  << " id=" << std::dec << ctx.r4.u32 << '\n';
    }
    if (frontendPrepareTraceEnabled && address == 0x8210E168u &&
        static_cast<uint32_t>(ctx.lr) == 0x821F6708u)
        std::cerr << "Flash async enqueue path=0x" << std::hex << ctx.r4.u32
                  << " done=0x" << ctx.r5.u32 << " buffer=0x" << ctx.r6.u32
                  << " bytes=0x" << ctx.r7.u32 << std::dec << '\n';
    if (frontendPrepareTraceEnabled && address == 0x8259C760u &&
        static_cast<uint32_t>(ctx.lr) == 0x821F66D8u)
    {
        uint32_t encoded = 0;
        std::memcpy(&encoded, base + ctx.r31.u32 + ctx.r27.u32, sizeof(encoded));
        std::cerr << "Flash block slot after acquire address=0x" << std::hex
                  << ctx.r31.u32 + ctx.r27.u32 << " value=0x"
                  << __builtin_bswap32(encoded) << std::dec << '\n';
    }
    static const bool profileEnabled = std::getenv("XERENGE_FUNCTION_PROFILE") != nullptr;
    if (profileEnabled)
    {
        if (address == 0x820A38E8u || address == 0x820A3988u ||
            address == 0x821819B8u || address == 0x821F7118u ||
            address == 0x82201010u || address == 0x82207630u ||
            address == 0x82207720u || address == 0x82207838u ||
            address == 0x822078F8u)
        {
            static std::atomic<uint32_t> bootTraceCount = 0;
            if (bootTraceCount.fetch_add(1, std::memory_order_relaxed) < 256)
                std::cerr << "Boot flow function=0x" << std::hex << address
                          << " caller=0x" << static_cast<uint32_t>(ctx.lr)
                          << " r3=0x" << ctx.r3.u32 << " r4=0x" << ctx.r4.u32
                          << " r5=0x" << ctx.r5.u32 << " r6=0x" << ctx.r6.u32
                          << std::dec << '\n';
        }
        if (address == 0x82104DD0u)
        {
            thread_local uint32_t memoryCardSamples = 0;
            if ((++memoryCardSamples % 1000000u) == 0)
            {
                auto profileWord = [base](uint32_t guestAddress) {
                    uint32_t value = 0;
                    std::memcpy(&value, base + guestAddress, sizeof(value));
                    return __builtin_bswap32(value);
                };
                std::cerr << "Memory card profile thread=" << std::this_thread::get_id()
                          << " object=0x" << std::hex << ctx.r3.u32
                          << " state=" << std::dec << profileWord(ctx.r3.u32 + 48)
                          << " result=" << profileWord(ctx.r3.u32 + 22400)
                          << " submitted=" << profileWord(ctx.r3.u32 + 2336)
                          << " completed=" << profileWord(ctx.r3.u32 + 2348)
                          << " readIndex=" << profileWord(ctx.r3.u32 + 2344)
                          << " readLimit=" << profileWord(ctx.r3.u32 + 2352) << '\n';
            }
        }
        thread_local std::unordered_map<uint32_t, uint32_t> profileCounts;
        thread_local uint32_t profileCalls = 0;
        ++profileCounts[address];
        if (++profileCalls == 250000)
        {
            std::vector<std::pair<uint32_t, uint32_t>> hottest(
                profileCounts.begin(), profileCounts.end());
            std::partial_sort(hottest.begin(),
                hottest.begin() + std::min<size_t>(hottest.size(), 12), hottest.end(),
                [](const auto& lhs, const auto& rhs) { return lhs.second > rhs.second; });
            std::cerr << "PPC profile thread=" << std::this_thread::get_id();
            for (size_t i = 0; i < std::min<size_t>(hottest.size(), 12); ++i)
                std::cerr << " 0x" << std::hex << hottest[i].first << ':'
                          << std::dec << hottest[i].second;
            std::cerr << '\n';
            hottest.erase(std::remove_if(hottest.begin(), hottest.end(),
                [](const auto& entry) { return entry.first >= 0x82300000u; }),
                hottest.end());
            std::partial_sort(hottest.begin(),
                hottest.begin() + std::min<size_t>(hottest.size(), 20), hottest.end(),
                [](const auto& lhs, const auto& rhs) { return lhs.second > rhs.second; });
            std::cerr << "PPC game profile thread=" << std::this_thread::get_id();
            for (size_t i = 0; i < std::min<size_t>(hottest.size(), 20); ++i)
                std::cerr << " 0x" << std::hex << hottest[i].first << ':'
                          << std::dec << hottest[i].second;
            std::cerr << '\n';
            profileCounts.clear();
            profileCalls = 0;
        }
    }
    gPpcCurrentFunction = address;
    gPpcCurrentCaller = static_cast<uint32_t>(ctx.lr);
    if (std::getenv("XERENGE_RING_PATH_TRACE") != nullptr &&
        (address == 0x82380E70u || address == 0x82380F20u || address == 0x82380FE8u))
    {
        static std::atomic<uint32_t> ringTraceCount{0};
        const uint32_t sample = ringTraceCount.fetch_add(1, std::memory_order_relaxed);
        if (sample < 64)
        {
            const auto loadGuest = [base](uint32_t guestAddress) {
                uint32_t value = 0;
                std::memcpy(&value, base + guestAddress, sizeof(value));
                return __builtin_bswap32(value);
            };
            const uint32_t object = ctx.r3.u32;
            std::cerr << "D3D ring path fn=0x" << std::hex << address
                      << " caller=0x" << static_cast<uint32_t>(ctx.lr)
                      << " r3=0x" << ctx.r3.u32 << " r4=0x" << ctx.r4.u32
                      << " r5=0x" << ctx.r5.u32 << " r6=0x" << ctx.r6.u32
                      << " fields={10384:0x" << loadGuest(object + 10384)
                      << " 10444:0x" << loadGuest(object + 10444)
                      << " 13996:0x" << loadGuest(object + 13996)
                      << " 14000:0x" << loadGuest(object + 14000)
                      << " 14028:0x" << loadGuest(object + 14028) << "}";
            const uint32_t vtable = loadGuest(object + 10384);
            if (vtable != 0)
                std::cerr << " vtable+4=0x" << loadGuest(vtable + 4)
                          << " vtable+60=0x" << loadGuest(vtable + 60);
            std::cerr << std::dec << '\n';
        }
    }
    if (gPpcIsEntryThread && entryTraceEnabled)
    {
        gPpcEntryFunction.store(address, std::memory_order_relaxed);
        gPpcEntryCaller.store(static_cast<uint32_t>(ctx.lr), std::memory_order_relaxed);
        gPpcEntryFunctionCalls.fetch_add(1, std::memory_order_relaxed);
    }
    if (address == 0x825847A8u || address == 0x82584E38u)
    {
        gPpcLastDataRoutine = address;
        gPpcLastDataRoutineArgs = {
            ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32};
    }
    if (address == 0x825857A8u && std::getenv("XERENGE_AUDIO_TRACE") != nullptr)
    {
        auto loadGuest = [base](uint32_t address)
        {
            uint32_t value = 0;
            std::memcpy(&value, base + address, sizeof(value));
            return __builtin_bswap32(value);
        };
        std::cerr << "X3DAudioCalculate entry r3=0x" << std::hex << ctx.r3.u32
                  << " r4=0x" << ctx.r4.u32 << " r5=0x" << ctx.r5.u32
                  << " r6=0x" << ctx.r6.u32
                  << " emitter+52=0x" << (ctx.r4.u32 != 0 ? loadGuest(ctx.r4.u32 + 52) : 0)
                  << " emitter+60=0x" << (ctx.r4.u32 != 0 ? loadGuest(ctx.r4.u32 + 60) : 0)
                  << " emitter+64=0x" << (ctx.r4.u32 != 0 ? loadGuest(ctx.r4.u32 + 64) : 0)
                  << std::dec << '\n';
    }
    const bool traceEnabled = gPpcTraceEnabled.load(std::memory_order_relaxed);
    const uint64_t callCount = traceEnabled
        ? gPpcFunctionCalls.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
    if (address == 0x82355500u && ctx.r3.u32 != 0)
    {
        // CGtResourceManager::Update derives a bucket descriptor from the
        // manager object and divides by its configured bucket size.  The
        // title creates this object through an Xbox virtual callback before
        // the callback table is fully materialized in the current bring-up;
        // keep the guest on the resource loading path while that table is
        // being completed instead of executing a native divide-by-zero.
        const auto loadGuest = [base](uint32_t address) {
            uint32_t value = 0;
            std::memcpy(&value, base + address, sizeof(value));
            return __builtin_bswap32(value);
        };
        const uint32_t list = loadGuest(ctx.r3.u32 + 16384);
        const uint32_t slotCount = loadGuest(ctx.r3.u32 + 16388);
        if (std::getenv("XERENGE_RESOURCE_TRACE") != nullptr)
        {
            static std::atomic<uint32_t> resourceTraceCount = 0;
            if (resourceTraceCount.fetch_add(1, std::memory_order_relaxed) < 32)
                std::cerr << "resource update manager=0x" << std::hex << ctx.r3.u32
                          << " list=0x" << list << " slots=" << std::dec << slotCount
                          << " caller=0x" << std::hex << static_cast<uint32_t>(ctx.lr)
                          << std::dec << '\n';
        }
        if (list != ctx.r3.u32 && slotCount != 0)
        {
            const uint32_t descriptor = ctx.r3.u32 + ((slotCount << 5) & 0xFFFFFFE0u) +
                (__builtin_rotateleft32(list - ctx.r3.u32, 1) - 1u);
            const uint32_t bucketSize = loadGuest(descriptor + 16476);
            if (bucketSize == 0)
            {
                const uint32_t encoded = __builtin_bswap32(1u);
                std::memcpy(base + descriptor + 16476, &encoded, sizeof(encoded));
                if (std::getenv("XERENGE_PPC_TRACE") != nullptr)
                    std::cerr << "resource manager repaired zero bucket size at 0x"
                              << std::hex << descriptor + 16476 << std::dec << '\n';
            }
        }
    }
    if (traceEnabled && (callCount % 10000) == 0)
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
    if (!traceEnabled)
        return;

    if (address == 0x825AE708u)
    {
        static std::atomic<uint32_t> threadCreateTraceCount = 0;
        if (threadCreateTraceCount.fetch_add(1, std::memory_order_relaxed) < 16)
            std::cerr << "thread create wrapper r3=0x" << std::hex << ctx.r3.u32
                      << " r5=0x" << ctx.r5.u32 << " r6=0x" << ctx.r6.u32
                      << " r7=0x" << ctx.r7.u32 << " r8=0x" << ctx.r8.u32
                      << " r9=0x" << ctx.r9.u32
                      << " caller=0x" << static_cast<uint32_t>(ctx.lr)
                      << std::dec << '\n';
    }

    if (address == 0x8238D6B8u || address == 0x8238D488u)
    {
        static std::atomic<uint32_t> renderQueueTraceCount = 0;
        if (renderQueueTraceCount.fetch_add(1, std::memory_order_relaxed) < 32)
        {
            auto guestWord = [base](uint32_t guestAddress) {
                uint32_t value = 0;
                std::memcpy(&value, base + guestAddress, sizeof(value));
                return __builtin_bswap32(value);
            };
            std::cerr << "render queue function=0x" << std::hex << address
                      << " r3=0x" << ctx.r3.u32;
            if (address == 0x8238D6B8u)
            {
                const uint32_t head = guestWord(ctx.r3.u32);
                std::cerr << " head=0x" << head
                          << " producer=0x" << guestWord(ctx.r3.u32 + 4)
                          << " headNext=0x" << guestWord(head)
                          << " consumer=0x" << guestWord(head + 628)
                          << " event=0x" << (ctx.r3.u32 + 32);
            }
            std::cerr << "\n" << std::dec;
        }
    }
    if (address == 0x825AE318u)
    {
        static std::atomic<uint32_t> ioTraceCount = 0;
        if (ioTraceCount.fetch_add(1, std::memory_order_relaxed) < 24)
        {
            uint32_t status = 0;
            if (ctx.r3.u32 != 0)
            {
                std::memcpy(&status, base + ctx.r3.u32, sizeof(status));
                status = __builtin_bswap32(status);
            }
            std::cerr << "resource io check r3=0x" << std::hex << ctx.r3.u32
                      << " status=" << status << " r4=0x" << ctx.r4.u32
                      << " r5=0x" << ctx.r5.u32 << std::dec << '\n';
        }
    }
    if (address == 0x82104DD0u)
    {
        static std::atomic<uint32_t> stateTraceCount = 0;
        uint32_t state = 0;
        std::memcpy(&state, base + ctx.r3.u32 + 48, sizeof(state));
        state = __builtin_bswap32(state);
        if (stateTraceCount.fetch_add(1, std::memory_order_relaxed) < 32)
        {
            uint32_t ioStatus = 0;
            std::memcpy(&ioStatus, base + ctx.r3.u32 + 22488, sizeof(ioStatus));
            ioStatus = __builtin_bswap32(ioStatus);
            auto guestWord = [base](uint32_t address) {
                uint32_t value = 0;
                std::memcpy(&value, base + address, sizeof(value));
                return __builtin_bswap32(value);
            };
            uint32_t submitted = guestWord(ctx.r3.u32 + 2336);
            uint32_t completed = guestWord(ctx.r3.u32 + 2348);
            uint32_t readIndex = guestWord(ctx.r3.u32 + 2344);
            uint32_t readLimit = guestWord(ctx.r3.u32 + 2352);
            std::cerr << "resource state object=0x" << std::hex << ctx.r3.u32
                      << " state=" << std::dec << state << " ioStatus=" << ioStatus
                      << " submitted=" << submitted << " completed=" << completed
                      << " readIndex=" << readIndex << " readLimit=" << readLimit << '\n';
        }
    }

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
    if ((address == 0x82150418 || address == 0x8234CBE0 ||
         address == 0x8238C960 || address == 0x82387D00 ||
         address == 0x82346708 || address == 0x82349C80 ||
         address == 0x82349B38 || address == 0x8237FCD0 ||
         address == 0x8237FD58 ||
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
    static std::atomic<uint32_t> timerTraceCount = 0;
    if ((address == 0x82423640 || address == 0x824236F8 ||
         address == 0x824237D8 || address == 0x824238F0 ||
         address == 0x8235F998 || address == 0x8235F828 ||
         address == 0x8235F658 || address == 0x8235F6F0 ||
         address == 0x8235F798 || address == 0x8235F1D0) &&
        timerTraceCount.fetch_add(1, std::memory_order_relaxed) < 32)
    {
        std::cerr << "timer path entry 0x" << std::hex << address
                  << " r3=0x" << ctx.r3.u32
                  << " r4=0x" << ctx.r4.u32
                  << " r5=0x" << ctx.r5.u32
                  << " r6=0x" << ctx.r6.u32
                  << " r7=0x" << ctx.r7.u32
                  << " r8=0x" << ctx.r8.u32 << std::dec << '\n';
    }
    if (address == 0x82380FE8 && ctx.r4.u32 != 0 && ctx.r5.u32 != 0 &&
        ctx.r5.u32 < 0x10000u)
    {
        // AddCommandsToPrimaryBuffer copies r5 guest dwords from r4 into the
        // primary ring. Inspect the source at the call boundary as well, so
        // PM4 draw packets are visible before the generated copy advances the
        // guest ring cursor.
        gXenosGpu.processSubmittedBuffer(base, ctx.r4.u32, ctx.r5.u32);
        if (std::getenv("XERENGE_PPC_TRACE") != nullptr)
        {
            static std::atomic<uint32_t> commandTraceCount = 0;
            if (commandTraceCount.fetch_add(1, std::memory_order_relaxed) < 8)
            {
                std::cerr << "primary commands count=" << std::dec << ctx.r5.u32
                          << " at=0x" << std::hex << ctx.r4.u32 << ":";
                auto guestWord = [base](uint32_t address) {
                    uint32_t value = 0;
                    std::memcpy(&value, base + address, sizeof(value));
                    return __builtin_bswap32(value);
                };
                for (uint32_t i = 0; i < std::min<uint32_t>(ctx.r5.u32, 24); ++i)
                    std::cerr << " " << guestWord(ctx.r4.u32 + i * 4);
                std::cerr << std::dec << '\n';
            }
        }
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

extern "C" void PPCGuestClockMidAsmHook(PPCRegister& r3)
{
    // 0x825AEF98 is the title's GetCurrentThreadId import.  NetCrit uses
    // this value as the owner token, so it must remain stable for the life of
    // a translated guest thread; using the clock here makes every lock
    // acquisition look like it came from a different owner and spins in
    // NetCritEnter forever.
    static std::atomic<uint32_t> nextGuestThreadId{1};
    thread_local const uint32_t guestThreadId =
        nextGuestThreadId.fetch_add(1, std::memory_order_relaxed);
    r3.u32 = guestThreadId;
}

extern "C" void PPCStubZeroMidAsmHook(PPCRegister& r3)
{
    r3.u32 = 0;
}

extern "C" void PPCStubZeroClearOutputMidAsmHook(PPCRegister& r3, PPCRegister& r4, uint8_t* base)
{
    if (base != nullptr && r3.u32 >= 0x60000000u && r3.u32 < 0x80000000u && r4.u32 <= 0x1000000u)
        std::memset(base + r3.u32, 0, r4.u32);
    r3.u32 = 0;
}

uint64_t hostTimeBaseFrequency()
{
    static const uint64_t frequency = []
    {
        const auto startTime = std::chrono::steady_clock::now();
        const uint64_t startTicks = __rdtsc();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const uint64_t elapsedTicks = __rdtsc() - startTicks;
        const auto elapsedNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - startTime).count();
        return elapsedNanoseconds > 0
            ? elapsedTicks * 1000000000ull / static_cast<uint64_t>(elapsedNanoseconds)
            : 50000000ull;
    }();
    return frequency;
}

extern "C" void PPCGuestStoreU32(uint8_t* base, uint32_t address, uint32_t value)
{
    if (address == 0x825EACE8u + 3760u &&
        std::getenv("XERENGE_FRONTEND_PREPARE_TRACE") != nullptr)
        std::cerr << "Memory block 19 pointer store value=0x" << std::hex << value
                  << " function=0x" << gPpcCurrentFunction << " caller=0x"
                  << gPpcCurrentCaller << std::dec << '\n';
    const uint32_t watchedResourceState =
        gResourceStateWatchAddress.load(std::memory_order_relaxed);
    if (watchedResourceState != 0 && address == watchedResourceState &&
        std::getenv("XERENGE_PPC_TRACE") != nullptr)
    {
        static std::atomic<uint32_t> stateStoreTraceCount = 0;
        if (stateStoreTraceCount.fetch_add(1, std::memory_order_relaxed) < 64)
        {
            void* returnAddress = __builtin_return_address(0);
            Dl_info symbol{};
            const bool resolved = dladdr(returnAddress, &symbol) != 0;
            const uintptr_t imageOffset = resolved
                ? reinterpret_cast<uintptr_t>(returnAddress) -
                    reinterpret_cast<uintptr_t>(symbol.dli_fbase)
                : 0;
            std::cerr << "resource state store address=0x" << std::hex << address
                      << " value=" << value
                      << " function=0x" << gPpcCurrentFunction
                      << " caller=0x" << gPpcCurrentCaller
                      << " nativeOffset=0x" << imageOffset
                      << " dataRoutine=0x" << gPpcLastDataRoutine
                      << " args=" << gPpcLastDataRoutineArgs[0] << ','
                      << gPpcLastDataRoutineArgs[1] << ',' << gPpcLastDataRoutineArgs[2]
                      << ',' << gPpcLastDataRoutineArgs[3] << ','
                      << gPpcLastDataRoutineArgs[4] << std::dec << '\n';
        }
    }
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

    // CP_RB_WPTR is only the submission doorbell. Deliver the guest callback
    // after the command processor observed an actual Xenos event writeback;
    // invoking it for every pointer update turns the worker into a PPC spin
    // loop before it can reach the title's swap path.
    if (address == XenosGpu::kMmioBase + XenosGpu::kCpRbWptr * 4 && width == 4)
    {
        if (gXenosGpu.takeInterruptPending())
            dispatchGraphicsInterrupt(base);
    }

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
    ~XboxServiceLayer()
    {
        audioThreadStop_.store(true, std::memory_order_release);
        if (audioThread_.joinable())
            audioThread_.join();
    }

    void invoke(std::string_view service, PPCContext& ctx, uint8_t* base)
    {
        std::unique_lock lock(stateMutex_);
        ++gPpcServiceCalls;
        if (service.compare(0, 7, "__imp__") == 0)
            service.remove_prefix(7);

        if (std::getenv("XERENGE_SERVICE_TRACE_UNIQUE") != nullptr)
        {
            static std::unordered_set<std::string> seenCalls;
            const std::string key = std::string(service) + '@' + std::to_string(ctx.lr);
            if (seenCalls.emplace(key).second)
                std::cerr << "Xbox service first call " << service
                          << " caller=0x" << std::hex << ctx.lr
                          << " r3=0x" << ctx.r3.u32 << " r4=0x" << ctx.r4.u32
                          << " r5=0x" << ctx.r5.u32 << " r6=0x" << ctx.r6.u32
                          << " r7=0x" << ctx.r7.u32 << " r8=0x" << ctx.r8.u32
                          << " r9=0x" << ctx.r9.u32 << std::dec << '\n';
        }

        // Keep service discovery useful without turning a boot trace into an
        // unbounded log. A successful stub return can otherwise hide the
        // first missing asset operation before the render loop.
        if (std::getenv("XERENGE_SERVICE_TRACE") != nullptr)
        {
            static std::atomic<uint32_t> serviceTraceCount = 0;
            const uint32_t traceIndex =
                serviceTraceCount.fetch_add(1, std::memory_order_relaxed);
            if (traceIndex < 256 || std::getenv("XERENGE_SERVICE_TRACE_ALL") != nullptr)
                std::cerr << "Xbox service " << service
                          << " r3=0x" << std::hex << ctx.r3.u32
                          << " r4=0x" << ctx.r4.u32
                          << " r5=0x" << ctx.r5.u32
                          << " r6=0x" << ctx.r6.u32
                          << " r7=0x" << ctx.r7.u32
                          << " r8=0x" << ctx.r8.u32
                          << " r9=0x" << ctx.r9.u32
                          << " caller=0x" << ctx.lr << std::dec << '\n';
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
        if ((service == "NtSetTimerEx" || service == "NtWaitForSingleObjectEx" ||
             service == "KeDelayExecutionThread") &&
            std::getenv("XERENGE_PPC_TRACE") != nullptr)
        {
            static std::atomic<uint32_t> timerServiceTraceCount = 0;
            if (timerServiceTraceCount.fetch_add(1, std::memory_order_relaxed) < 48)
                std::cerr << "timer service " << service
                          << " r3=0x" << std::hex << ctx.r3.u32
                          << " r4=0x" << ctx.r4.u32
                          << " r5=0x" << ctx.r5.u32
                          << " r6=0x" << ctx.r6.u32
                          << " r7=0x" << ctx.r7.u32
                          << " r8=0x" << ctx.r8.u32
                          << " r9=0x" << ctx.r9.u32
                          << " caller=0x" << ctx.lr << std::dec << '\n';
        }
        if ((service == "NtCreateFile" || service == "NtReadFile" ||
             service == "NtWriteFile" || service == "NtQueryInformationFile") &&
            std::getenv("XERENGE_PPC_TRACE") != nullptr)
        {
            static std::atomic<uint32_t> fileServiceTraceCount = 0;
            if (fileServiceTraceCount.fetch_add(1, std::memory_order_relaxed) < 64)
            {
                std::cerr << "file service " << service
                          << " r3=0x" << std::hex << ctx.r3.u32
                          << " r4=0x" << ctx.r4.u32
                          << " r5=0x" << ctx.r5.u32
                          << " r6=0x" << ctx.r6.u32
                          << " r7=0x" << ctx.r7.u32
                          << " r8=0x" << ctx.r8.u32
                          << " r9=0x" << ctx.r9.u32 << std::dec << '\n';
                if (service == "NtCreateFile" && ctx.r5.u32 != 0)
                {
                    const uint32_t ansi = loadU32(base, ctx.r5.u32 + 4);
                    const uint32_t chars = ansi != 0
                        ? std::min<uint32_t>(loadU16(base, ansi), 0x200u) : 0;
                    const uint32_t text = ansi != 0 ? loadU32(base, ansi + 4) : 0;
                    std::string path;
                    for (uint32_t i = 0; i < chars && text != 0; ++i)
                        path.push_back(static_cast<char>(base[text + i]));
                    std::cerr << "file path '" << path << "'\n";
                }
            }
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
                          << " flags=0x" << ctx.r9.u32
                          << " caller=0x" << static_cast<uint32_t>(ctx.lr)
                          << std::dec << '\n';
            const uint32_t handle = createObject(base);
            if (handleAddress != 0)
                storeU32(base, handleAddress, handle);
            const uint32_t threadId = nextThreadId_.fetch_add(1, std::memory_order_relaxed);
            if (threadIdAddress != 0)
                storeU32(base, threadIdAddress, threadId);
            if (handle != 0 && startAddress != 0)
            {
                // The title's resource-worker context is allocated from the
                // zeroed guest heap, while the PPC constructor leaves its
                // state field implicit. State 2 is the worker's documented
                // initialization entry; without it 821109F8 repeatedly
                // dispatches the invalid state 0 branch and never reaches
                // the resource queue.
                if (startAddress == 0x821109F8u && loadU32(base, startContext + 48) == 0)
                {
                    gResourceStateWatchAddress.store(startContext + 48, std::memory_order_relaxed);
                    storeU32(base, startContext + 48, 2);
                    std::cerr << "bootstrapped resource thread state object=0x"
                              << std::hex << startContext << " state=2" << std::dec << '\n';
                }
                launchGuestThread(base, ctx.r6.u32, startAddress, startContext, threadId);
            }
            ctx.r3.u32 = handle != 0 ? 0 : 0xC0000017u;
            return;
        }
        if (service == "NtCreateTimer")
        {
            const uint32_t outputHandle = ctx.r3.u32;
            const uint32_t handle = createObject(base);
            if (handle != 0)
            {
                // Timer handles are waitable dispatcher objects.  Keep them
                // in the same event table so NtSetTimerEx can wake the guest
                // waiter instead of making it spin on synthetic timeouts.
                timers_.insert(handle);
                events_[handle] = false;
                manualResetEvents_[handle] = false;
            }
            if (outputHandle != 0)
                storeU32(base, outputHandle, handle);
            ctx.r3.u32 = handle != 0 ? 0u : 0xC0000017u;
            return;
        }
        if (service == "NtCreateEvent")
        {
            // NT ABI: handle out, attributes, event type, initial state.
            const uint32_t outputHandle = ctx.r3.u32;
            const uint32_t handle = createObject(base);
            if (handle != 0)
            {
                events_[handle] = ctx.r6.u32 != 0;
                manualResetEvents_[handle] = ctx.r5.u32 == 0; // NotificationEvent.
            }
            if (outputHandle != 0)
                storeU32(base, outputHandle, handle);
            ctx.r3.u32 = handle != 0 ? 0u : 0xC0000017u;
            return;
        }
        if (service == "NtCreateSemaphore")
        {
            const uint32_t outputHandle = ctx.r3.u32;
            const uint32_t handle = createObject(base);
            if (handle != 0)
            {
                semaphores_[handle] = static_cast<int32_t>(ctx.r5.u32);
                semaphoreLimits_[handle] = std::max<int32_t>(
                    static_cast<int32_t>(ctx.r6.u32), 1);
            }
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
        if (service == "NtResumeThread")
        {
            const uint32_t thread = ctx.r3.u32;
            const auto it = threadSuspendCounts_.find(thread);
            if (it != threadSuspendCounts_.end() && it->second != 0)
                --it->second;
            if (ctx.r4.u32 != 0)
                storeU32(base, ctx.r4.u32,
                    it == threadSuspendCounts_.end() ? 0u : it->second);
            threadCondition_.notify_all();
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "KeWaitForSingleObject" || service == "NtWaitForSingleObjectEx")
        {
            const uint32_t object = ctx.r3.u32;
            const auto ready = [&]
            {
                if (object == gGraphicsWaitEvent.load(std::memory_order_acquire))
                    return gGraphicsWaitEventSignaled.load(std::memory_order_acquire);
                const auto event = events_.find(object);
                if (event != events_.end() && event->second)
                    return true;
                const auto semaphore = semaphores_.find(object);
                return semaphore != semaphores_.end() && semaphore->second > 0;
            };
            if (!ready())
                eventCondition_.wait_for(lock, std::chrono::milliseconds(2), ready);
            if (ready())
            {
                if (object == gGraphicsWaitEvent.load(std::memory_order_acquire))
                    gGraphicsWaitEventSignaled.store(false, std::memory_order_release);
                const auto event = events_.find(object);
                if (event != events_.end() && !manualResetEvents_[object])
                    event->second = false;
                const auto semaphore = semaphores_.find(object);
                if (semaphore != semaphores_.end())
                    --semaphore->second;
                ctx.r3.u32 = 0;
                return;
            }
            if (std::getenv("XERENGE_PPC_TRACE") != nullptr)
            {
                static std::atomic<uint32_t> waitTraceCount = 0;
                if (waitTraceCount.fetch_add(1, std::memory_order_relaxed) < 8)
                    std::cerr << "kernel event wait object=0x" << std::hex << ctx.r3.u32
                              << " caller=0x" << ctx.lr
                              << std::dec << '\n';
            }
            ctx.r3.u32 = 258; // STATUS_TIMEOUT
            return;
        }
        if (service == "KeSetEvent" || service == "KeResetEvent" ||
            service == "NtSetEvent")
        {
            const bool set = service != "KeResetEvent";
            if (ctx.r3.u32 == gGraphicsWaitEvent.load(std::memory_order_acquire))
                gGraphicsWaitEventSignaled.store(set, std::memory_order_release);
            const bool previous = events_[ctx.r3.u32];
            events_[ctx.r3.u32] = set;
            if (service == "NtSetEvent" && ctx.r4.u32 != 0)
                storeU32(base, ctx.r4.u32, previous ? 1u : 0u);
            if (set)
                eventCondition_.notify_all();
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "NtClearEvent")
        {
            // NtClearEvent is the kernel side of the title's ResetEvent
            // wrapper.  Resource IO uses this event as a completion latch;
            // leaving it signalled makes the file state machine repeatedly
            // re-enter UpdateIO instead of waiting for the next request.
            if (ctx.r3.u32 == gGraphicsWaitEvent.load(std::memory_order_acquire))
                gGraphicsWaitEventSignaled.store(false, std::memory_order_release);
            events_[ctx.r3.u32] = false;
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "KeReleaseSemaphore" || service == "NtReleaseSemaphore")
        {
            const uint32_t adjustment = service == "KeReleaseSemaphore"
                ? ctx.r5.u32 : ctx.r4.u32;
            const int32_t previous = semaphores_[ctx.r3.u32];
            const auto limitIt = semaphoreLimits_.find(ctx.r3.u32);
            const int32_t limit = limitIt != semaphoreLimits_.end()
                ? limitIt->second : INT32_MAX;
            semaphores_[ctx.r3.u32] = std::min<int32_t>(limit,
                previous + std::max<int32_t>(static_cast<int32_t>(adjustment), 1));
            if (service == "NtReleaseSemaphore" && ctx.r5.u32 != 0)
                storeU32(base, ctx.r5.u32, static_cast<uint32_t>(previous));
            eventCondition_.notify_all();
            ctx.r3.u32 = static_cast<uint32_t>(previous);
            return;
        }
        if (service == "KeSetBasePriorityThread" ||
            service == "ExRegisterTitleTerminateNotification" ||
            service == "KiApcNormalRoutineNop")
        {
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "NtCreateFile")
        {
            std::string path;
            if (ctx.r5.u32 != 0)
            {
                const uint32_t ansi = loadU32(base, ctx.r5.u32 + 4);
                const uint32_t chars = ansi != 0
                    ? std::min<uint32_t>(loadU16(base, ansi), 0x400u) : 0;
                const uint32_t text = ansi != 0 ? loadU32(base, ansi + 4) : 0;
                for (uint32_t i = 0; i < chars && text != 0; ++i)
                    path.push_back(static_cast<char>(base[text + i]));
            }
            uint32_t handle = 0;
            uint64_t size = 0;
            if (!path.empty() && gXboxMedia.openFile(path, handle, size))
            {
                if (ctx.r3.u32 != 0)
                    storeU32(base, ctx.r3.u32, handle);
                if (ctx.r6.u32 != 0)
                {
                    storeU32(base, ctx.r6.u32 + 0, 0);
                    storeU32(base, ctx.r6.u32 + 4, 1);
                }
                if (std::getenv("XERENGE_PPC_TRACE") != nullptr ||
                    std::getenv("XERENGE_MEDIA_TRACE") != nullptr)
                    std::cerr << "opened media file '" << path << "' handle=0x"
                              << std::hex << handle << " size=0x" << size << std::dec << '\n';
                ctx.r3.u32 = 0;
            }
            else
            {
                if (std::getenv("XERENGE_PPC_TRACE") != nullptr ||
                    std::getenv("XERENGE_MEDIA_TRACE") != nullptr)
                    std::cerr << "media file not found: '" << path << "'\n";
                ctx.r3.u32 = 0xC0000034u;
            }
            return;
        }
        if (service == "NtReadFile")
        {
            uint32_t bytesRead = 0;
            uint64_t byteOffset = 0;
            bool hasExplicitOffset = false;
            if (ctx.r10.u32 != 0)
            {
                byteOffset = (static_cast<uint64_t>(loadU32(base, ctx.r10.u32)) << 32) |
                    loadU32(base, ctx.r10.u32 + 4);
                // FILE_USE_FILE_POINTER_POSITION is -1; in that case the
                // handle's current position is used by NtReadFile.
                hasExplicitOffset = byteOffset != UINT64_MAX;
            }
            if (std::getenv("XERENGE_MEDIA_TRACE") != nullptr)
            {
                std::cerr << "media read handle=0x" << std::hex << ctx.r3.u32
                          << " buffer=0x" << ctx.r8.u32 << " bytes=0x" << ctx.r9.u32
                          << " offsetPtr=0x" << ctx.r10.u32 << " offset=0x" << byteOffset
                          << " caller=0x" << ctx.lr << std::dec << '\n';
            }
            if (hasExplicitOffset && byteOffset > INT64_MAX)
            {
                ctx.r3.u32 = 0xC000000Du; // STATUS_INVALID_PARAMETER.
                return;
            }
            if (hasExplicitOffset)
            {
                uint64_t position = 0;
                if (!gXboxMedia.seekFile(ctx.r3.u32, static_cast<int64_t>(byteOffset),
                                         0, position))
                {
                    ctx.r3.u32 = 0xC0000008u; // STATUS_INVALID_HANDLE.
                    return;
                }
            }
            // The title's wrapper passes the destination and byte count in
            // the preserved r8/r9 pair; r7 is its IO request structure.
            const bool ok = gXboxMedia.readFile(ctx.r3.u32, base + ctx.r8.u32,
                ctx.r9.u32, bytesRead);
            if (ctx.r7.u32 != 0)
            {
                storeU32(base, ctx.r7.u32 + 0, ok ? 0u : 0xC0000008u);
                storeU32(base, ctx.r7.u32 + 4, bytesRead);
            }
            // NtReadFile signals the optional event after completing the
            // request.  The resource worker waits on this dispatcher object
            // before advancing from its I/O state; returning the status
            // without setting it leaves a successfully copied resource stuck
            // in the loading screen.
            if (ctx.r4.u32 != 0)
            {
                events_[ctx.r4.u32] = true;
                eventCondition_.notify_all();
            }
            // A successful read at EOF is reported through the byte count,
            // but a subsequent non-empty read must return END_OF_FILE. The
            // title's stream wrapper uses this distinction to leave its
            // loading state; returning STATUS_SUCCESS with zero bytes makes
            // it restart the same scene forever.
            ctx.r3.u32 = ok
                ? (bytesRead == 0 && ctx.r9.u32 != 0 ? 0xC0000011u : 0u)
                : 0xC0000008u;
            return;
        }
        if (service == "NtQueryInformationFile")
        {
            uint64_t size = 0;
            const bool ok = gXboxMedia.fileSize(ctx.r3.u32, size);
            uint64_t position = 0;
            const bool hasPosition = gXboxMedia.position(ctx.r3.u32, position);
            // The XDK wrapper uses class 34 to obtain EOF from the extended
            // information record and class 14 to obtain the current offset.
            if (ok && ctx.r5.u32 != 0 && ctx.r7.u32 == 34 && ctx.r6.u32 >= 48)
            {
                storeU32(base, ctx.r5.u32 + 40, static_cast<uint32_t>(size >> 32));
                storeU32(base, ctx.r5.u32 + 44, static_cast<uint32_t>(size));
            }
            else if (hasPosition && ctx.r5.u32 != 0 && ctx.r7.u32 == 14 && ctx.r6.u32 >= 8)
            {
                storeU32(base, ctx.r5.u32 + 0, static_cast<uint32_t>(position >> 32));
                storeU32(base, ctx.r5.u32 + 4, static_cast<uint32_t>(position));
            }
            if (ctx.r4.u32 != 0)
            {
                storeU32(base, ctx.r4.u32 + 0, ok ? 0u : 0xC0000008u);
                storeU32(base, ctx.r4.u32 + 4, ok ? ctx.r6.u32 : 0u);
            }
            ctx.r3.u32 = ok ? 0u : 0xC0000008u;
            return;
        }
        if (service == "NtSetInformationFile")
        {
            uint64_t offset = 0;
            if (ctx.r5.u32 != 0)
            {
                offset = (static_cast<uint64_t>(loadU32(base, ctx.r5.u32)) << 32) |
                    loadU32(base, ctx.r5.u32 + 4);
            }
            uint64_t position = 0;
            const bool ok = offset <= INT64_MAX &&
                gXboxMedia.seekFile(ctx.r3.u32, static_cast<int64_t>(offset), 0, position);
            if (ctx.r4.u32 != 0)
            {
                storeU32(base, ctx.r4.u32 + 0, ok ? 0u : 0xC0000008u);
                storeU32(base, ctx.r4.u32 + 4, ok ? ctx.r6.u32 : 0u);
            }
            ctx.r3.u32 = ok ? 0u : 0xC0000008u;
            return;
        }
        if (service == "NtClose")
        {
            events_.erase(ctx.r3.u32);
            manualResetEvents_.erase(ctx.r3.u32);
            timers_.erase(ctx.r3.u32);
            semaphores_.erase(ctx.r3.u32);
            semaphoreLimits_.erase(ctx.r3.u32);
            gXboxMedia.closeFile(ctx.r3.u32);
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "NtSetTimerEx" || service == "KeDelayExecutionThread")
        {
            if (service == "NtSetTimerEx" && timers_.find(ctx.r3.u32) != timers_.end())
            {
                events_[ctx.r3.u32] = true;
                eventCondition_.notify_all();
            }
            // These calls are used by the title's timer worker. A short host
            // delay preserves pacing while the signalled timer event lets the
            // matching wait return STATUS_SUCCESS.
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "RtlNtStatusToDosError")
        {
            ctx.r3.u32 = ctx.r3.u32 == 0 ? 0u : 1u;
            return;
        }
        if (service == "XexCheckExecutablePrivilege")
        {
            // The retail image requests the same privilege that its loader
            // already grants to the title. Returning failure sends the XEX
            // bootstrap directly to KeBugCheck before game initialization.
            ctx.r3.u32 = 1;
            return;
        }
        if (service == "KeQueryPerformanceFrequency")
        {
            // The kernel export returns a 64-bit LARGE_INTEGER in r3. The
            // generated PPC currently lowers mftb to the host TSC, so report
            // the matching calibrated frequency as well.
            ctx.r3.u64 = hostTimeBaseFrequency();
            return;
        }
        if (service == "KeQuerySystemTime")
        {
            if (ctx.r3.u32 != 0)
            {
                const uint64_t ticks = PPCGuestClock();
                storeU32(base, ctx.r3.u32 + 0, static_cast<uint32_t>(ticks >> 32));
                storeU32(base, ctx.r3.u32 + 4, static_cast<uint32_t>(ticks));
            }
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "KeEnableFpuExceptions" || service == "NtDuplicateObject")
        {
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "MmAllocatePhysicalMemoryEx")
        {
            const uint32_t size = std::max<uint32_t>(ctx.r4.u32, 0x1000u);
            const uint32_t address = allocatePhysical(size, base);
            if (std::getenv("XERENGE_FRONTEND_PREPARE_TRACE") != nullptr)
                std::cerr << "MmAllocatePhysicalMemoryEx size=0x" << std::hex << size
                          << " result=0x" << address << " next=0x" << heapCursor_
                          << std::dec << '\n';
            ctx.r3.u32 = address;
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
            const uint32_t fallbackWidth = 1280;
            const uint32_t fallbackHeight = 720;
            // The title's wrapper passes ABI-specific stack pointers in these
            // slots. Keep the host presentation mode stable until the display
            // mode service is implemented and validated against a real frame.
            const uint32_t requestedWidth = ctx.r11.u32 != 0 ? loadU32(base, ctx.r11.u32) : 0;
            const uint32_t heightPointer = loadU32(base, ctx.r1.u32 + 84);
            const uint32_t requestedHeight = heightPointer != 0 ? loadU32(base, heightPointer) : 0;
            const uint32_t width = fallbackWidth;
            const uint32_t height = fallbackHeight;

            std::array<uint32_t, 16> sourceCommands{};
            for (uint32_t i = 0; i < sourceCommands.size(); ++i)
                sourceCommands[i] = loadU32(base, buffer + i * 4);

            // The command buffer already contains the title's PM4 stream.
            // Consume it before writing the platform swap packet below;
            // overwriting the first dwords first would erase the actual draw
            // commands and make a real frame indistinguishable from a clear.
            gXenosGpu.processSubmittedBuffer(base, buffer, 64);
            const bool frontbufferRead = gXenosGpu.presentFromGuest(base, frontbuffer, width, height);
            if (!frontbufferRead)
                gXenosGpu.present(width, height);

            // VdSwap reserves 64 dwords in the primary ring and fills it with
            // a fetch update followed by Xenia's observable XE_SWAP packet.
            // The same PM4 layout is understood by the Xenos command parser.
            storeU32(base, buffer + 0, (5u << 16) | 0x4800u);
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
            // The swap packet resolves the just-rendered EDRAM into the
            // frontbuffer. Read it after processing that packet so the host
            // window presents the current frame instead of the prior one.
            if (!gXenosGpu.presentFromGuest(base, frontbuffer, width, height))
                gXenosGpu.present(width, height);
            if (std::getenv("XERENGE_PPC_TRACE") != nullptr)
            {
                static std::atomic<uint32_t> swapTraceCount = 0;
                if (swapTraceCount.fetch_add(1, std::memory_order_relaxed) < 8)
                {
                    uint32_t frontbufferNonzero = 0;
                    for (uint32_t i = 0; i < 4096; ++i)
                        frontbufferNonzero += loadU32(base, frontbuffer + i * 4) != 0;
                    std::cerr << "VdSwap source 0x" << std::hex << buffer << ":";
                    for (const uint32_t value : sourceCommands)
                        std::cerr << " " << value;
                    std::cerr << std::dec << '\n';
                    std::cerr << "VdSwap caller=0x" << std::hex << ctx.lr
                              << " command buffer 0x" << ctx.r3.u32 << ":";
                    for (uint32_t i = 0; i < 8; ++i)
                        std::cerr << " " << loadU32(base, ctx.r3.u32 + i * 4);
                    std::cerr << " fetch=0x" << ctx.r4.u32 << ":";
                    for (uint32_t i = 0; i < 6; ++i)
                        std::cerr << " " << loadU32(base, ctx.r4.u32 + i * 4);
                    std::cerr << " frontbuffer=0x" << frontbuffer
                              << " nonzeroWords4096=" << std::dec << frontbufferNonzero
                              << " requested=" << requestedWidth << 'x' << requestedHeight
                              << std::hex
                              << " r5=0x" << ctx.r5.u32
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
                              << gXenosGpu.lastFrameHeight()
                              << " opcodes={";
                    bool firstOpcode = true;
                    for (uint32_t opcode = 0; opcode < 256; ++opcode)
                    {
                        const uint64_t count = gXenosGpu.opcodeCount(opcode);
                        if (count == 0)
                            continue;
                        if (!firstOpcode)
                            std::cerr << ',';
                        firstOpcode = false;
                        std::cerr << std::hex << opcode << ':' << std::dec << count;
                    }
                    std::cerr << "}\n";
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
        if (service == "VdSetGraphicsInterruptCallback")
        {
            gGraphicsInterruptCallback.store(ctx.r3.u32, std::memory_order_release);
            gGraphicsInterruptContext.store(ctx.r4.u32, std::memory_order_release);
            if (std::getenv("XERENGE_PPC_TRACE") != nullptr)
                std::cerr << "registered graphics interrupt callback=0x" << std::hex
                          << ctx.r3.u32 << " context=0x" << ctx.r4.u32 << std::dec << '\n';
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "VdRetrainEDRAM" || service == "VdRetrainEDRAMWorker" ||
            service == "VdEnableDisableClockGating" || service == "VdShutdownEngines" ||
            service == "VdSetSystemCommandBufferGpuIdentifierAddress" ||
            service == "VdPersistDisplay" || service == "VdCallGraphicsNotificationRoutines")
        {
            ctx.r3.u32 = 0;
            return;
        }

        if (service == "XamAlloc" || service == "ExAllocatePoolWithTag")
        {
            const uint32_t size = std::max<uint32_t>(ctx.r3.u32, 1);
            ctx.r3.u32 = allocate(size, base);
            return;
        }
        if (service == "RtlAllocateHeap")
        {
            // NT ABI: RtlAllocateHeap(heapHandle, flags, size).
            const uint32_t size = std::max<uint32_t>(ctx.r5.u32, 1);
            ctx.r3.u32 = allocate(size, base);
            return;
        }
        if (service == "NtAllocateVirtualMemory")
        {
            // Xenon XAPI wrapper ABI: r3 = *baseAddress, r4 = *regionSize,
            // r5 = allocation type, r6 = protection. The recompiled title
            // passes these four arguments directly; r3 is not a process
            // handle and r6 is not a size pointer.
            const uint32_t baseAddress = ctx.r3.u32;
            const uint32_t sizeAddress = ctx.r4.u32;
            const uint32_t requested = sizeAddress != 0 ? loadU32(base, sizeAddress) : 0;
            const uint32_t size = std::max<uint32_t>(requested, 0x1000u);
            const uint32_t allocation = allocate(size, base);
            if (baseAddress != 0)
                storeU32(base, baseAddress, allocation);
            if (sizeAddress != 0)
                storeU32(base, sizeAddress, size);
            ctx.r3.u32 = allocation != 0 ? 0 : 0xC0000017u; // STATUS_NO_MEMORY
            return;
        }
        if (service == "XamFree" || service == "ExFreePool")
        {
            allocations_.erase(ctx.r3.u32);
            ctx.r3.u32 = 1;
            return;
        }
        if (service == "RtlFreeHeap")
        {
            // NT ABI: RtlFreeHeap(heapHandle, flags, allocation).
            allocations_.erase(ctx.r5.u32);
            ctx.r3.u32 = 1;
            return;
        }
        if (service == "RtlAllocateHeap")
        {
            // The title's XAPI heap object is initialized by the retail
            // kernel.  The generated guest implementation requires that
            // object to be present before its first allocation, while the
            // runtime already owns a bounds-checked guest allocator.  Keep
            // the Xbox ABI (heap, flags, size) and return a real guest
            // pointer so callers can use the allocation immediately.
            const uint32_t size = ctx.r5.u32;
            ctx.r3.u32 = allocate(size, base);
            return;
        }
        if (service == "RtlReAllocateHeap")
        {
            // RtlReAllocateHeap(heap, flags, old, size). Preserve existing
            // contents when possible; callers use this for small metadata
            // buffers during frontend setup.
            const uint32_t oldAddress = ctx.r5.u32;
            const uint32_t oldSize = allocations_.count(oldAddress)
                ? allocations_.at(oldAddress) : 0;
            const uint32_t newAddress = allocate(ctx.r6.u32, base);
            if (newAddress != 0 && oldAddress != 0 && oldSize != 0)
                std::memcpy(base + newAddress, base + oldAddress,
                    std::min(oldSize, ctx.r6.u32));
            allocations_.erase(oldAddress);
            ctx.r3.u32 = newAddress;
            return;
        }
        if (service == "XAudioGetSpeakerConfig")
        {
            if (ctx.r3.u32 != 0)
                storeU32(base, ctx.r3.u32, 0x00000003u);
            if (ctx.r4.u32 != 0)
                storeU32(base, ctx.r4.u32, 0x00000003u);
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "XAudioSetSpeakerConfig")
        {
            // The frontend only needs the platform to accept the selected
            // stereo configuration.  The host sink performs the actual
            // channel conversion when a render-driver frame arrives.
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "XAudioGetVoiceCategoryVolumeChangeMask")
        {
            // XAudio reports changes through an optional output mask.  A
            // stable zero mask means that no external mixer changed the
            // categories while still completing the render-driver setup.
            if (ctx.r4.u32 >= 0x60000000u && ctx.r4.u32 < 0x80000000u)
                storeU32(base, ctx.r4.u32, 0);
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "XAudioGetVoiceCategoryVolume")
        {
            // Category volume is a BE float in the caller's output slot.
            // Use unity gain; XAudioBackend applies the host sink format and
            // leaves the title's PCM frame untouched.
            if (ctx.r5.u32 >= 0x60000000u && ctx.r5.u32 < 0x80000000u)
                storeU32(base, ctx.r5.u32, 0x3F800000u); // 1.0f
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "XAudioSetVoiceCategoryVolume")
        {
            // Accept title mixer updates.  The current host backend receives
            // already mixed render-driver frames, so no additional state is
            // needed here to keep the Xbox contract asynchronous and safe.
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "XAudioRenderDriverLock" ||
            service == "XAudioSuspendRenderDriverClients")
        {
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "XAudioEffectManagerQueryEffectSize" ||
            service == "XAudioRoutedVoiceInitialize")
        {
            // Burnout creates no custom XAudio effects on the boot path. The
            // retail effect manager and routed voice normally call back into
            // their kernel-owned vtables; the host audio path has no need for
            // those optional objects, so report an empty effect chain and a
            // successfully initialized routed voice.
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "XenonDvdFileSync")
        {
            // CGtFileXenonDVD exposes the open operation asynchronously. The
            // host XDVDFS backend is already mounted synchronously, so a
            // pending guest open can be completed at the first Sync call.
            // Preserve all other status values and only advance the open
            // state used by the title's request queue.
            const uint32_t status = loadU32(base, ctx.r3.u32 + 32u);
            if (status == 2u)
                storeU32(base, ctx.r3.u32 + 32u, 1u);
            ctx.r3.u32 = status == 2u ? 1u : status;
            return;
        }
        if (service == "XAudioRegisterRenderDriverClient")
        {
            if (ctx.r4.u32 != 0)
                storeU32(base, ctx.r4.u32, 0x44415544u); // 'DAUD'
            xaudio_.start();
            registerAudioClient(base, ctx.r3.u32);
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "XAudioUnregisterRenderDriverClient")
        {
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "XAudioSubmitRenderDriverFrame")
        {
            if (ctx.r3.u32 == 0x44415544u && ctx.r4.u32 != 0)
            {
                xaudio_.submitGuestFrame(base, ctx.r4.u32);
                if (std::getenv("XERENGE_AUDIO_TRACE") != nullptr)
                {
                    static std::atomic<uint32_t> submitTraceCount = 0;
                    if (submitTraceCount.fetch_add(1, std::memory_order_relaxed) < 8)
                        std::cerr << "XAudio submitted frame buffer=0x" << std::hex
                                  << ctx.r4.u32 << std::dec << '\n';
                }
            }
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "XMsgStartIORequest" || service == "XMsgStartIORequestEx")
        {
            // XAM_OVERLAPPED is passed in r9 by the generated title wrappers:
            //   0x00 result, 0x04 length, 0x08 context, 0x0c event,
            //   0x10 completion routine, 0x14 completion context,
            //   0x18 extended error.
            // XMsgStartIORequest resets the event before starting a new
            // request.  The frontend subsequently performs the actual
            // synchronous work through the host-backed service and signals
            // this event itself, so leaving the old signalled state here can
            // make a worker consume a stale completion and stop progressing.
            const uint32_t overlapped = ctx.r9.u32;
            const bool validOverlapped =
                (overlapped >= 0x60000000u && overlapped < 0x80000000u) ||
                (overlapped >= 0x82000000u && overlapped < 0x83000000u);
            if (validOverlapped)
            {
                const uint32_t event = loadU32(base, overlapped + 0x0c);
                if (event != 0)
                {
                    events_[event] = false;
                    eventCondition_.notify_all();
                }
                storeU32(base, overlapped + 0x00, 0);
                storeU32(base, overlapped + 0x04, 0);
                storeU32(base, overlapped + 0x18, 0);
                if (std::getenv("XERENGE_XMSG_TRACE") != nullptr)
                {
                    static std::atomic<uint32_t> xmsgTraceCount = 0;
                    if (xmsgTraceCount.fetch_add(1, std::memory_order_relaxed) < 32)
                        std::cerr << "XMsg reset " << service << " overlapped=0x"
                                  << std::hex << overlapped << " event=0x" << event
                                  << " message=0x" << ctx.r4.u32 << std::dec << '\n';
                }
            }
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "XMsgInProcessCall")
        {
            // Profile/resource messages use 0x7001B to clear a two-word
            // result record referenced by param1[1] (the third ABI argument).
            if (ctx.r4.u32 == 0x7001Bu && ctx.r5.u32 != 0)
            {
                const uint32_t result = loadU32(base, ctx.r5.u32 + 4);
                if (result != 0)
                {
                    storeU32(base, result, 0);
                    storeU32(base, result + 4, 0);
                }
            }
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "XamUserCreateAchievementEnumerator")
        {
            // The title uses this during frontend profile setup.  The XAM
            // ABI returns the required item buffer size through r9 and the
            // enumerator handle through r10; returning only HRESULT leaves
            // the caller with an uninitialized object that is later treated
            // as a vtable and dispatches through arbitrary guest memory.
            const uint32_t flags = ctx.r6.u32;
            const uint32_t count = ctx.r8.u32;
            const uint32_t entrySize = 36u + ((flags & 7u) != 0 ? 464u : 0u);
            if (ctx.r9.u32 != 0)
                storeU32(base, ctx.r9.u32, entrySize * count);
            if (ctx.r10.u32 != 0)
            {
                const uint32_t enumerator = createObject(base);
                storeU32(base, ctx.r10.u32, enumerator);
                achievementEnumerators_.emplace(enumerator, 0);
            }
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "XamEnumerate")
        {
            // The frontend asks for the first achievement page while building
            // the local profile screen.  XamEnumerate receives the output
            // buffer in r5, its size in r6, and the returned item count in
            // r7 after the title's small argument-shuffling wrapper.
            const uint32_t handle = ctx.r3.u32;
            const auto enumerator = achievementEnumerators_.find(handle);
            if (enumerator != achievementEnumerators_.end() &&
                ctx.r5.u32 >= 0x50000000u && ctx.r5.u32 < 0x90000000u &&
                ctx.r6.u32 <= 0x1000000u)
            {
                if (enumerator->second == 0)
                {
                    if (ctx.r6.u32 != 0)
                        std::memset(base + ctx.r5.u32, 0, ctx.r6.u32);
                    if (ctx.r7.u32 >= 0x50000000u && ctx.r7.u32 < 0x90000000u)
                        storeU32(base, ctx.r7.u32, 1);
                    enumerator->second = 1;
                    ctx.r3.u32 = 0;
                }
                else
                {
                    if (ctx.r7.u32 >= 0x50000000u && ctx.r7.u32 < 0x90000000u)
                        storeU32(base, ctx.r7.u32, 0);
                    ctx.r3.u32 = 259; // ERROR_NO_MORE_FILES.
                }
                return;
            }
            ctx.r3.u32 = 0xC000000Du; // STATUS_INVALID_PARAMETER.
            return;
        }
        if (service == "XamNotifyCreateListener")
        {
            const uint32_t listener = createObject(base);
            notificationQueues_[listener] = {};
            ctx.r3.u32 = listener;
            return;
        }
        if (service == "XamContentCreate" || service == "XamContentCreateEnumerator" ||
            service == "XamSessionCreateHandle" ||
            service == "XamVoiceCreate" || service == "XMACreateContext")
        {
            ctx.r3.u32 = createObject(base);
            return;
        }
        if (service == "XNotifyGetNext")
        {
            // BOOL XNotifyGetNext(listener, filter, id, parameter).  The
            // frontend polls this after the device selector completes.  A
            // permanently empty queue leaves it in the bootstrap screen, so
            // deliver the UI-open and UI-dismissed notifications queued by
            // the host-side XAM dialog stub.
            auto queue = notificationQueues_.find(ctx.r3.u32);
            auto notification = queue == notificationQueues_.end()
                ? std::deque<std::pair<uint32_t, uint32_t>>::iterator{}
                : queue->second.end();
            if (queue != notificationQueues_.end())
            {
                for (auto it = queue->second.begin();
                     it != queue->second.end(); ++it)
                {
                    if (ctx.r4.u32 == 0 || ctx.r4.u32 == it->first)
                    {
                        notification = it;
                        break;
                    }
                }
            }
            if (queue != notificationQueues_.end() &&
                notification != queue->second.end())
            {
                if (ctx.r5.u32 != 0)
                    storeU32(base, ctx.r5.u32, notification->first);
                if (ctx.r6.u32 != 0)
                    storeU32(base, ctx.r6.u32, notification->second);
                queue->second.erase(notification);
                ctx.r3.u32 = 1;
            }
            else
            {
                if (ctx.r5.u32 != 0)
                    storeU32(base, ctx.r5.u32, 0);
                if (ctx.r6.u32 != 0)
                    storeU32(base, ctx.r6.u32, 0);
                ctx.r3.u32 = 0;
            }
            return;
        }
        if (service == "XamInputGetState")
        {
            constexpr uint32_t kErrorDeviceNotConnected = 1167u;
            const uint32_t userIndex = ctx.r3.u32;
            const uint32_t state = ctx.r5.u32;
            if (userIndex != 0 || state == 0)
            {
                ctx.r3.u32 = kErrorDeviceNotConnected;
                return;
            }
            // XINPUT_STATE is a packet number followed by the 12-byte gamepad
            // state. Expose pad 0 as connected and idle until host input is
            // wired into these fields.
            clear(base, state, 16);
            const uint32_t packet = inputPacketNumber_++;
            storeU32(base, state, packet);
            uint16_t buttons = gInputButtons.load(std::memory_order_relaxed);
            const bool holdStart = std::getenv("XERENGE_HOLD_START") != nullptr;
            const bool holdA = std::getenv("XERENGE_HOLD_A") != nullptr;
            const bool lateAutoStart = std::getenv("XERENGE_AUTO_START_LATE") != nullptr;
            const bool autoStartPulse = std::getenv("XERENGE_AUTO_START") != nullptr &&
                packet >= (lateAutoStart ? 300u : 0u) &&
                packet % 300u >= 20u && packet % 300u < 24u;
            const bool autoAPulse = std::getenv("XERENGE_AUTO_A") != nullptr &&
                packet >= (lateAutoStart ? 300u : 0u) &&
                packet % 300u >= 20u && packet % 300u < 24u;
            if (holdStart || autoStartPulse)
                buttons |= 0x0010u;
            if (holdA || autoAPulse)
                buttons |= 0x1000u;
            storeU16(base, state + 4, buttons);
            if (std::getenv("XERENGE_INPUT_TRACE") != nullptr)
            {
                static std::atomic<uint32_t> inputTraceCount = 0;
                if (inputTraceCount.fetch_add(1, std::memory_order_relaxed) < 32)
                    std::cerr << "XamInputGetState packet=" << packet
                              << " buttons=0x" << std::hex << buttons
                              << " state=0x" << state << std::dec << '\n';
            }
            ctx.r3.u32 = 0; // ERROR_SUCCESS.
            return;
        }
        if (service == "XamInputSetState")
        {
            ctx.r3.u32 = ctx.r3.u32 == 0 ? 0u : 1167u;
            return;
        }
        if (service == "XamShowDeviceSelectorUI")
        {
            // The frontend uses this asynchronous XAM dialog while creating
            // its profile/device state. There is no host dialog in the
            // runtime, so complete the selection immediately and return the
            // primary local device through the title's output structure.
            if (ctx.r7.u32 >= 0x50000000u && ctx.r7.u32 < 0x90000000u)
                storeU32(base, ctx.r7.u32, 0);
            if (ctx.r8.u32 >= 0x50000000u && ctx.r8.u32 < 0x90000000u)
            {
                storeU32(base, ctx.r8.u32 + 0x00, 0);
                storeU32(base, ctx.r8.u32 + 0x04, 0);
                const uint32_t event = loadU32(base, ctx.r8.u32 + 0x0c);
                if (event != 0)
                {
                    events_[event] = true;
                    eventCondition_.notify_all();
                }
            }
            queueSystemNotifications();
            gDeviceSelectorCompleted.store(true, std::memory_order_release);
            // The selector is completed synchronously by the host.  Retail
            // XAM resumes the resource worker when its modal UI is dismissed;
            // without that UI there is no later user-driven resume call, so
            // release the thread suspension created by the selector path.
            for (auto& [thread, suspendCount] : threadSuspendCounts_)
            {
                if (suspendCount != 0)
                    suspendCount = 0;
            }
            threadCondition_.notify_all();
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "XamShowSigninUI")
        {
            // There is no host sign-in dialog.  The local profile is already
            // reported as signed in by XamUserGetSigninState.
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "XamUserGetSigninState")
        {
            // XUSER_SIGNIN_STATE_SIGNED_IN_LOCALLY for the primary pad.
            ctx.r3.u32 = ctx.r3.u32 == 0 ? 1u : 0u;
            return;
        }
        if (service == "XamUserGetXUID")
        {
            constexpr uint32_t kErrorNoSuchUser = 1317u;
            const uint32_t userIndex = ctx.r3.u32;
            const uint32_t output = ctx.r5.u32;
            if (userIndex != 0 || output == 0)
            {
                ctx.r3.u32 = kErrorNoSuchUser;
                return;
            }
            const uint64_t xuid = __builtin_bswap64(0xE000000000000001ull);
            std::memcpy(base + output, &xuid, sizeof(xuid));
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "XamLoaderGetLaunchData")
        {
            // This title is started directly and has no launch-data blob.
            // XAM reports the absence explicitly; returning a generic NT
            // failure makes callers take the wrong bootstrap error path.
            ctx.r3.u32 = 1168u; // X_ERROR_NOT_FOUND.
            return;
        }
        if (service == "XamLoaderGetLaunchDataSize")
        {
            // Keep the size query consistent with GetLaunchData for a direct
            // title launch without a handoff payload.
            if (ctx.r3.u32 >= 0x50000000u && ctx.r3.u32 < 0x90000000u)
                storeU32(base, ctx.r3.u32, 0);
            ctx.r3.u32 = 1168u; // X_ERROR_NOT_FOUND.
            return;
        }
        if (service == "NtSuspendThread")
        {
            const uint32_t thread = ctx.r3.u32;
            const uint32_t previous = threadSuspendCounts_[thread];
            ++threadSuspendCounts_[thread];
            if (ctx.r4.u32 != 0)
                storeU32(base, ctx.r4.u32, previous);
            // The resource worker suspends itself through this wrapper after
            // entering its idle state. Block the host thread until the title
            // resumes the same Xbox thread handle; returning immediately
            // leaves the worker spinning through the PPC dispatcher.
            if (ctx.lr == 0x825AE504u)
            {
                if (gDeviceSelectorCompleted.exchange(false, std::memory_order_acq_rel))
                {
                    // The host completed the selector synchronously, so the
                    // worker must not park waiting for a UI resume callback.
                    --threadSuspendCounts_[thread];
                    threadCondition_.notify_all();
                }
                else
                {
                threadCondition_.wait(lock, [&]
                {
                    const auto current = threadSuspendCounts_.find(thread);
                    return current == threadSuspendCounts_.end() || current->second == 0;
                });
                }
            }
            ctx.r3.u32 = 0;
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
            criticalSections_[ctx.r3.u32] = std::make_shared<std::recursive_mutex>();
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "RtlEnterCriticalSection")
        {
            const uint32_t address = ctx.r3.u32;
            auto& entry = criticalSections_[address];
            if (!entry)
                entry = std::make_shared<std::recursive_mutex>();
            const auto criticalSection = entry;
            lock.unlock();
            criticalSection->lock();
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "RtlLeaveCriticalSection")
        {
            const auto it = criticalSections_.find(ctx.r3.u32);
            if (it != criticalSections_.end())
            {
                const auto criticalSection = it->second;
                lock.unlock();
                criticalSection->unlock();
            }
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "KeTlsAlloc")
        {
            for (uint32_t index = 0; index < tlsUsed_.size(); ++index)
            {
                if (!tlsUsed_[index])
                {
                    tlsUsed_[index] = true;
                    gPpcTlsValues[index] = 0;
                    ctx.r3.u32 = index;
                    return;
                }
            }
            ctx.r3.u32 = 0xffffffffu;
            return;
        }
        if (service == "KeTlsFree")
        {
            if (ctx.r3.u32 < tlsUsed_.size())
            {
                tlsUsed_[ctx.r3.u32] = false;
                gPpcTlsValues[ctx.r3.u32] = 0;
            }
            ctx.r3.u32 = 1;
            return;
        }
        if (service == "KeTlsSetValue")
        {
            if (ctx.r3.u32 < gPpcTlsValues.size() && tlsUsed_[ctx.r3.u32])
                gPpcTlsValues[ctx.r3.u32] = ctx.r4.u32;
            ctx.r3.u32 = 1;
            return;
        }
        if (service == "KeTlsGetValue")
        {
            ctx.r3.u32 = ctx.r3.u32 < gPpcTlsValues.size() && tlsUsed_[ctx.r3.u32]
                ? gPpcTlsValues[ctx.r3.u32] : 0;
            return;
        }
        if (service == "KeGetCurrentProcessType")
        {
            ctx.r3.u32 = 1;
            return;
        }
        if (service == "XGetLanguage")
        {
            // The prototype exposes its supported frontend as French and
            // ships MainFr.bin as the active string bank. XDBF language
            // identifiers use 4 for French; returning English here selects
            // a bank that this build does not provide.
            ctx.r3.u32 = 4;
            return;
        }
        if (service == "ExGetXConfigSetting")
        {
            // The graphics bootstrap only needs a successful read of the
            // small console configuration record.  Returning the generic
            // failure status makes InitializeHardwareDevice abort before it
            // can submit any title draw commands.
            if (ctx.r5.u32 != 0 && ctx.r6.u32 != 0)
            {
                const uint32_t bytes = std::min<uint32_t>(ctx.r6.u32, 0x100u);
                std::memset(base + ctx.r5.u32, 0, bytes);
            }
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "XGetGameRegion")
        {
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "XamGetSystemVersion")
        {
            // XMsg uses this during local profile/bootstrap discovery.  The
            // runtime has no Xbox Live service, but the title must still see
            // a valid local system query so the request can complete.
            if (ctx.r3.u32 >= 0x60000000u && ctx.r3.u32 < 0x80000000u)
                storeU32(base, ctx.r3.u32, 0x00000000u);
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "NetDll_XNetStartup" || service == "NetDll_XNetCleanup")
        {
            // Burnout initializes its online capable frontend even when no
            // network is present.  Complete the local XNet lifecycle without
            // creating sockets or advertising a host network interface.
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "NetDll_WSACreateEvent")
        {
            // WSA events are waitable, manual-reset guest dispatcher objects.
            // The title uses them for its optional online worker even when it
            // runs without a network connection.
            const uint32_t event = createObject(base);
            if (event != 0)
            {
                events_[event] = false;
                manualResetEvents_[event] = true;
            }
            ctx.r3.u32 = event;
            return;
        }
        if (service == "NetDll_socket")
        {
            // The frontend creates one UDP socket while probing the local
            // XNet state.  Keep it as a stable synthetic descriptor; the
            // offline path only needs bind/ioctl/close to complete.
            ctx.r3.u32 = 0x100u;
            return;
        }
        if (service == "NetDll_bind" || service == "NetDll_ioctlsocket" ||
            service == "NetDll_closesocket")
        {
            // No network transport is exposed by this runtime yet, but these
            // setup calls must have normal WinSock success results so the
            // title can select its offline frontend path.
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "NetDll_WSAGetLastError")
        {
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "NetDll_WSACloseEvent")
        {
            events_.erase(ctx.r3.u32);
            manualResetEvents_.erase(ctx.r3.u32);
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "NetDll_WSASetEvent" || service == "NetDll_WSAResetEvent")
        {
            const bool set = service == "NetDll_WSASetEvent";
            events_[ctx.r3.u32] = set;
            if (set)
                eventCondition_.notify_all();
            ctx.r3.u32 = 1;
            return;
        }
        if (service == "NetDll_WSAWaitForMultipleEvents")
        {
            // XNet follows the WinSock ABI: count, guest HANDLE array,
            // wait-all, timeout in milliseconds, alertable.  Returning the
            // standard timeout value lets the title keep rendering while the
            // offline network worker polls its state.
            const uint32_t count = std::min<uint32_t>(ctx.r3.u32, 64);
            const uint32_t handles = ctx.r4.u32;
            const bool waitAll = ctx.r5.u32 != 0;
            const uint32_t timeout = ctx.r6.u32;
            std::vector<uint32_t> waitHandles;
            waitHandles.reserve(count);
            for (uint32_t i = 0; i < count; ++i)
                waitHandles.push_back(loadU32(base, handles + i * 4));
            const auto ready = [&]
            {
                uint32_t readyCount = 0;
                for (const uint32_t handle : waitHandles)
                {
                    const auto it = events_.find(handle);
                    if (it != events_.end() && it->second)
                    {
                        ++readyCount;
                        if (!waitAll)
                            return true;
                    }
                }
                return waitAll && readyCount == waitHandles.size();
            };
            if (!ready())
            {
                const auto waitDuration = timeout == 0xFFFFFFFFu
                    ? std::chrono::milliseconds(2)
                    : std::chrono::milliseconds(std::min<uint32_t>(timeout, 2));
                eventCondition_.wait_for(lock, waitDuration, ready);
            }
            if (ready())
            {
                for (size_t i = 0; i < waitHandles.size(); ++i)
                {
                    const auto it = events_.find(waitHandles[i]);
                    if (it != events_.end() && it->second)
                    {
                        if (!manualResetEvents_[waitHandles[i]])
                            it->second = false;
                        ctx.r3.u32 = 0x00000000u + static_cast<uint32_t>(i);
                        return;
                    }
                }
            }
            ctx.r3.u32 = 258; // WSA_WAIT_TIMEOUT
            return;
        }
        if (service == "NetDll_XNetGetTitleXnAddr")
        {
            // Report an initialized Ethernet link without an online address.
            // This is the documented offline state and prevents the title
            // from interpreting a failed service dispatch as a fatal error.
            if (ctx.r4.u32 >= 0x50000000u && ctx.r4.u32 < 0x90000000u)
                std::memset(base + ctx.r4.u32, 0, 36);
            ctx.r3.u32 = 0x2u; // XNET_GET_XNADDR_ETHERNET.
            return;
        }
        if (service == "XGetVideoMode")
        {
            // XVIDEO_MODE is returned through r3.  The refresh rate is a BE
            // float at offset 0x14; treating it as an integer corrupts the
            // display bootstrap's aspect and scaler calculations.
            if (ctx.r3.u32 != 0)
            {
                storeU32(base, ctx.r3.u32 + 0, 1280);
                storeU32(base, ctx.r3.u32 + 4, 720);
                storeU32(base, ctx.r3.u32 + 8, 0); // progressive
                storeU32(base, ctx.r3.u32 + 12, 1); // widescreen
                storeU32(base, ctx.r3.u32 + 16, 1); // high definition
                storeU32(base, ctx.r3.u32 + 20, 0x42700000u); // 60.0f
                storeU32(base, ctx.r3.u32 + 24, 1); // NTSC
                storeU32(base, ctx.r3.u32 + 28, 0x4A);
                storeU32(base, ctx.r3.u32 + 32, 1);
            }
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "VdQueryVideoMode")
        {
            if (ctx.r3.u32 != 0)
            {
                storeU32(base, ctx.r3.u32 + 0, 1280);
                storeU32(base, ctx.r3.u32 + 4, 720);
                storeU32(base, ctx.r3.u32 + 8, 0);
                storeU32(base, ctx.r3.u32 + 12, 1);
                storeU32(base, ctx.r3.u32 + 16, 1);
                storeU32(base, ctx.r3.u32 + 20, 0x42700000u);
                storeU32(base, ctx.r3.u32 + 24, 1);
                storeU32(base, ctx.r3.u32 + 28, 0x4A);
                storeU32(base, ctx.r3.u32 + 32, 1);
                for (uint32_t offset = 36; offset < 48; offset += 4)
                    storeU32(base, ctx.r3.u32 + offset, 0);
            }
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "VdGetCurrentDisplayInformation")
        {
            if (ctx.r3.u32 != 0)
            {
                std::memset(base + ctx.r3.u32, 0, 0x58);
                storeU16(base, ctx.r3.u32 + 0x00, 1280);
                storeU16(base, ctx.r3.u32 + 0x02, 720);
                storeU32(base, ctx.r3.u32 + 0x18, 1);
                storeU32(base, ctx.r3.u32 + 0x14, 720);
                storeU32(base, ctx.r3.u32 + 0x10, 1280);
                storeU16(base, ctx.r3.u32 + 0x48, 1280);
                storeU16(base, ctx.r3.u32 + 0x4A, 720);
                storeU32(base, ctx.r3.u32 + 0x4C, 0x42700000u);
                storeU32(base, ctx.r3.u32 + 0x50, 0);
                storeU16(base, ctx.r3.u32 + 0x56, 1280);
            }
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "VdInitializeScalerCommandBuffer")
        {
            // The destination pointer and dword count are trailing ABI
            // arguments. The title only requires a valid NOP-filled command
            // buffer before it starts submitting frames.
            const uint32_t destination = ctx.r11.u32;
            // The last scalar argument is in the caller's outgoing stack
            // area. r12 is a scratch register here, not an ABI argument.
            const uint32_t count = readGuestU32(base, ctx.r1.u32 + 0x6Cu);
            if (std::getenv("XERENGE_PPC_TRACE") != nullptr)
            {
                std::cerr << "scaler args r1=0x" << std::hex << ctx.r1.u32
                          << " r3=0x" << ctx.r3.u32 << " r4=0x" << ctx.r4.u32
                          << " r5=0x" << ctx.r5.u32 << " r6=0x" << ctx.r6.u32
                          << " r7=0x" << ctx.r7.u32 << " r8=0x" << ctx.r8.u32
                          << " r9=0x" << ctx.r9.u32 << " r10=0x" << ctx.r10.u32
                          << " r11=0x" << ctx.r11.u32 << " r12=0x" << ctx.r12.u32
                          << std::dec << '\n';
                static std::atomic<uint32_t> scalerStackTraceCount = 0;
                if (scalerStackTraceCount.fetch_add(1, std::memory_order_relaxed) == 0)
                {
                    std::cerr << "scaler stack:";
                    for (uint32_t offset = 0; offset <= 0xE0; offset += 4)
                        std::cerr << " +0x" << std::hex << offset << "=0x"
                                  << readGuestU32(base, ctx.r1.u32 + offset);
                    std::cerr << std::dec << '\n';
                }
            }
            if (destination != 0 && count != 0 && count < 0x10000u)
            {
                for (uint32_t i = 0; i < count; ++i)
                    storeU32(base, destination + i * 4, 0x80000000u);
            }
            ctx.r3.u32 = count;
            return;
        }
        if (service == "RtlUnicodeStringToAnsiString")
        {
            // Xbox uses the standard counted-string layout, but all fields
            // live in the big-endian guest image. The prototype's paths are
            // ASCII-compatible UTF-16, so preserve non-ASCII code units as
            // '?' while keeping the exact counted-string ABI.
            const uint32_t destination = ctx.r3.u32;
            const uint32_t source = ctx.r4.u32;
            const bool allocateDestination = ctx.r5.u32 != 0;
            const uint32_t sourceLength = source != 0 ? loadU16(base, source) : 0;
            const uint32_t sourceBuffer = source != 0 ? loadU32(base, source + 4) : 0;
            if (destination == 0 || sourceBuffer == 0 || (sourceLength & 1u) != 0)
            {
                ctx.r3.u32 = 0xC000000Du; // STATUS_INVALID_PARAMETER
                return;
            }

            const uint32_t characterCount = sourceLength / 2;
            const uint32_t required = characterCount + 1;
            uint32_t destinationBuffer = loadU32(base, destination + 4);
            uint32_t capacity = loadU16(base, destination + 2);
            if (allocateDestination)
            {
                destinationBuffer = allocate(required, base);
                capacity = required;
                storeU32(base, destination + 4, destinationBuffer);
                storeU16(base, destination + 2, static_cast<uint16_t>(capacity));
            }
            if (destinationBuffer == 0 || capacity == 0 || capacity < required)
            {
                ctx.r3.u32 = 0xC0000005u; // STATUS_ACCESS_VIOLATION
                return;
            }

            const uint32_t outputLength = std::min(characterCount, capacity - 1);
            for (uint32_t i = 0; i < outputLength; ++i)
            {
                const uint16_t codeUnit = loadU16(base, sourceBuffer + i * 2);
                base[destinationBuffer + i] =
                    codeUnit <= 0x7Fu ? static_cast<uint8_t>(codeUnit) : '?';
            }
            base[destinationBuffer + outputLength] = 0;
            storeU16(base, destination, static_cast<uint16_t>(outputLength));
            ctx.r3.u32 = 0;
            return;
        }
        if (service == "RtlFreeAnsiString")
        {
            const uint32_t string = ctx.r3.u32;
            if (string != 0)
            {
                const uint32_t buffer = loadU32(base, string + 4);
                if (buffer != 0)
                    allocations_.erase(buffer);
                storeU16(base, string, 0);
                storeU16(base, string + 2, 0);
                storeU32(base, string + 4, 0);
            }
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

        if (gPpcServiceCalls.load(std::memory_order_relaxed) <= 40)
            std::cerr << "unimplemented Xbox service: " << service << '\n';
        ctx.r3.u32 = 0xC0000001u; // STATUS_UNSUCCESSFUL
    }

    bool invokeCallback(uint32_t address, PPCContext& ctx)
    {
        std::lock_guard lock(stateMutex_);
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
            if (std::getenv("XERENGE_PPC_TRACE") != nullptr)
            {
                static std::atomic<uint32_t> callbackTraceCount = 0;
                if (callbackTraceCount.fetch_add(1, std::memory_order_relaxed) < 64)
                    std::cerr << "synthetic vtable callback method=" << method
                              << " object=0x" << std::hex << object
                              << " caller=0x" << static_cast<uint32_t>(ctx.lr)
                              << " r4=0x" << ctx.r4.u32 << " r5=0x" << ctx.r5.u32
                              << " r6=0x" << ctx.r6.u32 << " r7=0x" << ctx.r7.u32
                              << std::dec << '\n';
            }
            // Most methods used during early title bring-up are notification
            // and configuration calls.  They return success while preserving
            // the object in r3 for the following guest call.
            ctx.r3.u32 = 0;
        }
        return true;
    }

    bool materializeGuestObject(uint32_t object, uint8_t* base)
    {
        std::lock_guard lock(stateMutex_);
        const bool titleObject = object >= 0x82000000u && object < 0x90000000u;
        const bool guestHeapObject = object >= 0x60000000u && object < 0x80000000u;
        if (!titleObject && !guestHeapObject)
            return false;
        if (objects_.find(object) == objects_.end())
            objects_.emplace(object, 1);
        allocateVtable(base, 0x220);
        storeU32(base, object, kVtableBase);
        return true;
    }

    uint32_t materializeNullObject(PPCContext& ctx, uint8_t* base)
    {
        std::lock_guard lock(stateMutex_);
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
        std::lock_guard lock(stateMutex_);
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
        std::lock_guard lock(stateMutex_);
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
    void registerAudioClient(uint8_t* base, uint32_t callback)
    {
        if (callback == 0)
            return;
        const uint32_t function = loadU32(base, callback);
        const uint32_t parameter = loadU32(base, callback + 4);
        if (std::getenv("XERENGE_AUDIO_TRACE") != nullptr)
            std::cerr << "XAudio client callback=0x" << std::hex << function
                      << " parameter=0x" << parameter << std::dec << '\n';
        if (function < PPC_CODE_BASE || function >= PPC_CODE_BASE + PPC_CODE_SIZE)
            return;
        std::lock_guard lock(audioMutex_);
        if (audioThread_.joinable())
            return;
        audioCallbackBase_ = base;
        audioCallback_ = function;
        audioCallbackParameter_ = parameter;
        audioThreadStop_.store(false, std::memory_order_release);
        audioThread_ = std::thread([this]
        {
            constexpr auto interval = std::chrono::microseconds(5333);
            while (!audioThreadStop_.load(std::memory_order_acquire))
            {
                PPCContext context{};
                context.r1.u32 = 0x81FD0000u;
                context.r3.u32 = audioCallbackParameter_;
                PPCDispatchIndirect(context, audioCallbackBase_, audioCallback_);
                std::this_thread::sleep_for(interval);
            }
        });
    }

    uint32_t allocatePhysical(uint32_t size, uint8_t* base)
    {
        constexpr uint32_t alignment = 0x1000u;
        heapCursor_ = (heapCursor_ + alignment - 1) & ~(alignment - 1);
        return allocate(size, base);
    }

    void launchGuestThread(uint8_t* base, uint32_t startupAddress, uint32_t startAddress,
        uint32_t startContext, uint32_t threadId)
    {
        if (startAddress == 0x8238D6B8u)
        {
            // Vd's notification worker waits on the dispatcher event embedded
            // at +0x20 in its context; it is not an NtCreateEvent handle.
            gGraphicsWaitEvent.store(startContext + 32u, std::memory_order_release);
            gGraphicsWaitEventSignaled.store(false, std::memory_order_release);
        }
        std::thread([base, startupAddress, startAddress, startContext, threadId]
        {
            PPCContext threadContext{};
            threadContext.r1.u32 = 0x81FC0000u - ((threadId & 0xFFu) * 0x10000u);
            if (startupAddress != 0)
            {
                // XAPI startup trampoline: r3/r4 carry the requested entry
                // point and its context.
                threadContext.r3.u32 = startAddress;
                threadContext.r4.u32 = startContext;
                PPCDispatchIndirect(threadContext, base, startupAddress);
            }
            else
            {
                // The startup callback is optional on Xenon.  Kernel and
                // graphics worker threads use the entry point directly and
                // receive their context in r3.
                threadContext.r3.u32 = startContext;
                if (std::getenv("XERENGE_PPC_TRACE") != nullptr)
                    std::cerr << "direct guest thread entry=0x" << std::hex << startAddress
                              << " context=0x" << startContext << std::dec << '\n';
                PPCDispatchIndirect(threadContext, base, startAddress);
            }
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

    void queueSystemNotifications()
    {
        for (auto& [listener, notifications] : notificationQueues_)
        {
            if (!notifications.empty())
                continue;
            // XamShowDeviceSelectorUI broadcasts notification 9 when the
            // modal device UI opens and again when it closes.
            notifications.emplace_back(0x00000009u, 1u);
            notifications.emplace_back(0x00000009u, 0u);
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

    static uint16_t loadU16(const uint8_t* base, uint32_t address)
    {
        uint16_t value = 0;
        std::memcpy(&value, base + address, sizeof(value));
        return __builtin_bswap16(value);
    }

    uint32_t heapCursor_ = 0x60000000u;
    // Early Burnout allocates large physical video heaps (over 200 MiB)
    // before creating the primary command ring.
    static constexpr uint32_t heapLimit_ = 0x80000000u;
    static constexpr uint32_t kVtableBase = 0x81000000u;
    std::unordered_map<uint32_t, uint32_t> allocations_;
    std::unordered_map<uint32_t, uint32_t> objects_;
    std::unordered_map<uint32_t, bool> events_;
    std::unordered_map<uint32_t, bool> manualResetEvents_;
    std::unordered_set<uint32_t> timers_;
    std::unordered_map<uint32_t, int32_t> semaphores_;
    std::unordered_map<uint32_t, int32_t> semaphoreLimits_;
    std::condition_variable_any eventCondition_;
    std::condition_variable_any threadCondition_;
    std::unordered_map<uint32_t, uint32_t> threadSuspendCounts_;
    std::unordered_map<uint32_t, std::shared_ptr<std::recursive_mutex>> criticalSections_;
    std::unordered_map<uint32_t, std::deque<std::pair<uint32_t, uint32_t>>> notificationQueues_;
    std::unordered_map<uint32_t, uint32_t> achievementEnumerators_;
    bool vtableAllocated_ = false;
    std::array<bool, 64> tlsUsed_{};
    std::atomic<uint32_t> nextThreadId_{1};
    uint32_t inputPacketNumber_ = 1;
    XAudioBackend xaudio_;
    std::recursive_mutex stateMutex_;
    std::mutex audioMutex_;
    std::thread audioThread_;
    std::atomic<bool> audioThreadStop_{false};
    uint8_t* audioCallbackBase_ = nullptr;
    uint32_t audioCallback_ = 0;
    uint32_t audioCallbackParameter_ = 0;
};

XboxServiceLayer gXboxServices;

extern "C" void PPCMaterializeIfZeroMidAsmHook(PPCRegister& r3, PPCContext& ctx, uint8_t* base)
{
    if (r3.u32 == 0)
        r3.u32 = gXboxServices.materializeNullObject(ctx, base);
}

extern "C" uint32_t PPCMaterializeObject(PPCContext& ctx, uint8_t* base)
{
    return gXboxServices.materializeNullObject(ctx, base);
}

extern "C" void PPCImportedServiceTrap(const char* service, PPCContext& ctx, uint8_t* base)
{
    gXboxServices.invoke(service, ctx, base);
}

// Calls to these XAM thunks are emitted as direct title calls by XenonRecomp,
// while indirect imports already use ppc_import_stubs.cpp. Strong definitions
// here override the weak translated NOP wrappers and keep both paths on the
// same service implementation.
extern "C" void sub_825C64DC(PPCContext& ctx, uint8_t* base)
{
    gXboxServices.invoke("XamNotifyCreateListener", ctx, base);
}

extern "C" void sub_825C614C(PPCContext& ctx, uint8_t* base)
{
    gXboxServices.invoke("XNotifyGetNext", ctx, base);
}

extern "C" void __real___imp__sub_825857A8(PPCContext& ctx, uint8_t* base);
extern "C" void __wrap___imp__sub_825857A8(PPCContext& ctx, uint8_t* base)
{
    // X3DAudioCalculate is title code, but early Burnout emitters can carry
    // stale optional distance-curve pointers.  The caller initializes its
    // DSP output record to zero before invoking us; skip only this malformed
    // audio source and keep valid emitters on the translated implementation.
    const auto isGuestPointer = [](uint32_t address)
    {
        return address == 0 ||
            (address >= 0x60000000u && address < 0x80000000u) ||
            (address >= 0x82000000u && address < 0x90000000u);
    };
    auto loadGuest = [base](uint32_t address)
    {
        uint32_t value = 0;
        std::memcpy(&value, base + address, sizeof(value));
        return __builtin_bswap32(value);
    };
    const uint32_t emitter = ctx.r4.u32;
    const bool validEmitter = isGuestPointer(emitter) && emitter != 0 &&
        isGuestPointer(loadGuest(emitter + 60)) &&
        isGuestPointer(loadGuest(emitter + 64));
    if (!validEmitter)
    {
        if (std::getenv("XERENGE_AUDIO_TRACE") != nullptr)
            std::cerr << "X3DAudioCalculate skipped malformed emitter=0x"
                      << std::hex << emitter << std::dec << '\n';
        return;
    }
    __real___imp__sub_825857A8(ctx, base);
}

extern "C" void PPCUnknownIndirectTrap(uint32_t address, PPCContext& ctx, uint8_t* base)
{
    if (ctx.lr == 0x8238B834u || ctx.lr == 0x8238BE1Cu)
    {
        // These are optional notification hooks in the frontend state
        // machine.  On this prototype their slots contain locale/profile
        // data (for example 0xffff and 0x1922), rather than PPC entry
        // points.  The caller already treats the hook as optional and the
        // normal return value is ignored, so complete the notification
        // without letting data be dispatched as code.
        ctx.r3.u32 = 0;
        return;
    }
    if (ctx.lr == 0x82095B04u)
    {
        if (std::getenv("XERENGE_PPC_TRACE") != nullptr)
        {
            static std::atomic<uint32_t> resourcePollTraceCount = 0;
            if (resourcePollTraceCount.fetch_add(1, std::memory_order_relaxed) < 8)
                std::cerr << "frontend resource poll target=0x" << std::hex << address
                          << " object=0x" << ctx.r3.u32 << " vtable=0x"
                          << XboxServiceLayer::readGuestU32(base, ctx.r3.u32)
                          << " command=0x" << ctx.r4.u32 << " ready=0x"
                          << static_cast<uint32_t>(base[0x82D40F09u]) << std::dec << '\n';
        }
        // The frontend bootstrap polls its platform resource object with
        // command 5 until the object signals completion in this byte. The
        // synthetic service object has no asynchronous backend, so complete
        // the request when its callback is dispatched.
        base[0x82D40F09u] = 1;
        ctx.r3.u32 = 0;
        return;
    }
    if (gXboxServices.invokeCallback(address, ctx))
        return;
    if (address == 0x80000000u && ctx.lr == 0x82095858u)
    {
        // Optional platform pre-poll callback. The retail BSS uses the Xenon
        // sentinel until a platform implementation is installed.
        ctx.r3.u32 = 0;
        return;
    }
    if (ctx.lr == 0x824236B0u && ctx.r31.u32 >= 0x82D3965Cu &&
        ctx.r31.u32 < 0x82D3967Cu)
    {
        // The timer worker walks eight optional callback slots. Empty slots
        // may retain their own BSS address after early object setup; that is
        // a list sentinel, not executable PPC code.
        XboxServiceLayer::writeGuestU32(base, ctx.r31.u32, 0u);
        ctx.r3.u32 = 0;
        return;
    }
    if (address == 0x80000000u && ctx.lr == 0x82381170u)
    {
        // The title leaves this optional platform handler at the Xenon
        // sentinel value 0x80000000.  It is called only to notify the
        // platform after resource setup; treating the absent handler as a
        // successful no-op keeps the guest on its normal initialization path.
        ctx.r3.u32 = 0;
        return;
    }
    if (ctx.lr == 0x824236B0u &&
        (address < PPC_IMAGE_BASE || address >= PPC_CODE_BASE))
    {
        // TimerThreadProc walks a fixed eight-entry callback array.  During
        // early frontend startup the tail can contain stale words rather
        // than executable PPC addresses.  Return from this slot and advance
        // the cursor to the final entry so the worker can finish its pass;
        // genuine callbacks are dispatched by PPCDispatchIndirect before
        // reaching this path.
        ctx.r31.u32 = ctx.r30.u32 + 28u;
        ctx.r3.u32 = 0;
        return;
    }
    if (ctx.lr == 0x8234796Cu &&
        ctx.r31.u32 >= 0x82000000u && ctx.r31.u32 < 0x90000000u)
    {
        // CGtSoundListenerManagerBase::Update walks the listener array at
        // this point.  Until the audio backend creates listeners, the retail
        // BSS contains an uninitialized count, which otherwise turns into a
        // huge loop of bogus indirect calls and starves video initialization.
        // The current runtime has no audio listener backend, so terminate the
        // list after the first invalid entry and let the graphics path run.
        XboxServiceLayer::writeGuestU32(base, ctx.r31.u32 + 8u, 0u);
        ctx.r3.u32 = 0;
        return;
    }
    if (ctx.lr == 0x82348B20u)
    {
        // CGtSoundGtfsFile::Read reaches an optional platform sound-file
        // backend through the object at +8.  The retail frontend can issue
        // this request before the XAudio service has created that backend;
        // the null object's first words then look like an indirect target
        // (currently 0x630).  Complete the read as an empty audio block so
        // the sound manager can advance its state and the title can keep
        // loading graphics.
        if (ctx.r4.u32 >= 0x60000000u && ctx.r4.u32 < 0x80000000u &&
            ctx.r5.u32 <= 0x1000000u)
        {
            std::memset(base + ctx.r4.u32, 0, ctx.r5.u32);
        }
        ctx.r3.u32 = 0;
        return;
    }
    if (ctx.lr == 0x8256424Cu)
    {
        // This wrapper invokes the platform input object's GetState method.
        // Several early FE owners leave their optional input-device field
        // uninitialized, so the virtual call resolves through random data.
        // Report an idle device and initialize the byte consumed by both
        // callers instead of allowing stack garbage to steer the FE state.
        if (ctx.r4.u32 >= 0x60000000u && ctx.r4.u32 < 0x80000000u)
            base[ctx.r4.u32] = 0;
        ctx.r3.u32 = 0;
        return;
    }
    if (ctx.lr == 0x825B3AA0u && ctx.r30.u32 == 0x826AFCD4u)
    {
        // Stop a malformed notification-list walk at its sentinel. The
        // callback slot can contain stale BSS/image data even when the list
        // next pointer is already null; treating this as an empty list keeps
        // the worker progressing without invoking arbitrary guest data.
        ctx.r31.u32 = ctx.r30.u32;
        ctx.r3.u32 = 0;
        return;
    }
    if (address != 0 && ctx.r3.u32 >= 0x82000000u && ctx.r3.u32 < 0x90000000u)
    {
        // Some title-owned objects are constructed before their platform
        // vtable is supplied. Their first word then contains a heap/data
        // address, which would otherwise be called as PPC code. Materialize
        // the object in the same callback window used by Xbox service objects
        // and let the next virtual call resolve through invokeCallback.
        if (gXboxServices.materializeGuestObject(ctx.r3.u32, base))
        {
            ctx.r3.u32 = 0;
            return;
        }
    }
    if (address == 0x630u && ctx.r30.u32 == 0x826AFCD4u)
    {
        // The loader clears this callback-list head while zeroing BSS after
        // the early host-side initialization.  The list walk in
        // sub_825B3A58 expects an empty circular sentinel here; otherwise the
        // zeroed node is interpreted as a callback at address 0x630.
        XboxServiceLayer::writeGuestU32(base, 0x826AFCD4u, 0x826AFCD4u);
        ctx.r3.u32 = 0;
        return;
    }
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
    if (ctx.lr == 0x82564D20u)
    {
        // XAudio's DPC fan-out contains optional title callbacks. During
        // frontend startup some slots contain stack/data addresses rather
        // than PPC entry points. The caller decrements r26 after returning;
        // setting it to one completes this eight-slot pass instead of
        // repeatedly dispatching the same invalid callback and starving the
        // guest render thread. The registered render callback remains active.
        ctx.r26.u32 = 1;
        ctx.r3.u32 = 0;
        return;
    }
    if (ctx.lr == 0x825651CCu || ctx.lr == 0x82565388u ||
        ctx.lr == 0x8256544Cu)
    {
        // XAudio's completion pass invokes optional client notifications
        // through voice-object vtables at three fan-out sites. The
        // render-driver client is real and already submits PCM frames, but
        // the retail title can leave these secondary notification objects
        // without host-backed methods. Complete only these invalid
        // notifications so they cannot turn the guest scheduler into calls
        // to data addresses.
        ctx.r3.u32 = 0;
        return;
    }
    if (ctx.lr == 0x82381888u || ctx.lr == 0x82381924u)
    {
        // The display queue has optional platform notification hooks at
        // these two call sites.  On the retail boot path their storage can
        // still contain a floating point value (0x3f800000), which is data
        // rather than a PPC entry point.  The queue itself is valid, so
        // complete only the absent notification and continue submission.
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
        if (argc < 3 || argc > 4)
        {
            std::cerr << "usage: xerenge-runtime --ppc-prepare|--ppc-entry <file.xex> [media.iso]\n";
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
        auto guest = std::make_unique<PpcGuestMemory>();
        if (!guest->initialize(*mapped))
            return 1;
        std::cout << "prepared PPC guest memory: 4 GiB reservation, "
                  << guest->functionCount() << " function mappings, entry point resolved\n";
        if (std::string(argv[1]) == "--ppc-entry")
        {
            const char* mediaPath = argc == 4 ? argv[3] : std::getenv("XERENGE_MEDIA");
            if (mediaPath != nullptr)
            {
                if (!gXboxMedia.open(mediaPath))
                    std::cerr << "warning: could not open XDVDFS media: " << mediaPath << '\n';
                else
                    std::cout << "XDVDFS media mounted: " << mediaPath << '\n';
            }
            gPpcServiceCalls = 0;
            const auto shaderCache = xerengeShaderCache();
            std::cout << "Xenos shader cache: "
                      << (shaderCache.available() ? std::to_string(shaderCache.entryCount) : "none")
                      << " entries\n" << std::flush;
            if (!gXenosGpu.initializeVulkan())
                std::cerr << "warning: Xenos Vulkan backend initialization failed\n";

            std::atomic<bool> guestReturned = false;
            std::thread guestThread([guestPtr = guest.get(), &guestReturned]
            {
                gPpcIsEntryThread = true;
                guestPtr->invokeEntryPoint();
                guestReturned.store(true, std::memory_order_release);
            });

            if (!glfwInit())
            {
                guestThread.join();
                std::cout << "PPC entry point returned after "
                          << gPpcServiceCalls.load(std::memory_order_relaxed)
                          << " service calls\n";
                return 0;
            }
            glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
            GLFWwindow* window = glfwCreateWindow(1280, 720, "Xerenge Burnout video", nullptr, nullptr);
            if (window == nullptr)
            {
                glfwTerminate();
                guestThread.join();
                return 1;
            }
            glfwMakeContextCurrent(window);
            glfwSwapInterval(1);
            gHostCloseRequested.store(false, std::memory_order_release);
            // Keep the close request explicit: after the event callback sets
            // this flag, the loop performs window cleanup and returns from
            // main, which terminates the process together with detached
            // guest-owned workers.
            glfwSetWindowCloseCallback(window, [](GLFWwindow*)
            {
                gHostCloseRequested.store(true, std::memory_order_release);
            });
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            bool readbackReported = false;
            bool guestReturnReported = false;
            uint64_t lastEntryTraceCalls = 0;
            auto nextEntryTrace = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (!glfwWindowShouldClose(window) &&
                   !gHostCloseRequested.load(std::memory_order_acquire))
            {
                if (!guestReturnReported && guestReturned.load(std::memory_order_acquire))
                {
                    std::cerr << "PPC entry thread returned after "
                              << gPpcServiceCalls.load(std::memory_order_relaxed)
                              << " service calls\n";
                    guestReturnReported = true;
                }
                if (std::getenv("XERENGE_ENTRY_TRACE") != nullptr &&
                    std::chrono::steady_clock::now() >= nextEntryTrace)
                {
                    const uint64_t calls = gPpcEntryFunctionCalls.load(std::memory_order_relaxed);
                    std::cerr << "PPC entry sample function=0x" << std::hex
                              << gPpcEntryFunction.load(std::memory_order_relaxed)
                              << " caller=0x" << gPpcEntryCaller.load(std::memory_order_relaxed)
                              << std::dec << " calls=" << calls
                              << " delta=" << (calls - lastEntryTraceCalls) << '\n';
                    lastEntryTraceCalls = calls;
                    nextEntryTrace += std::chrono::seconds(1);
                }
                const auto pixels = gXenosGpu.framebufferCopy();
                glViewport(0, 0, 1280, 720);
                glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                if (!pixels.empty())
                {
                    // Xenos readback stores row zero at the top of the
                    // display surface, while OpenGL's pixel raster position
                    // starts at the lower-left. Draw from the upper-left
                    // with a negative Y zoom so scanout preserves guest
                    // orientation.
                    glRasterPos2f(-1.0f, 1.0f);
                    glPixelZoom(1.0f, -1.0f);
                    glDrawPixels(static_cast<GLsizei>(gXenosGpu.lastFrameWidth()),
                        static_cast<GLsizei>(gXenosGpu.lastFrameHeight()),
                        GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
                    if (!readbackReported)
                    {
                        std::cout << "Xenos framebuffer readback: "
                                  << gXenosGpu.lastFrameWidth() << 'x'
                                  << gXenosGpu.lastFrameHeight() << " checksum=0x"
                                  << std::hex << gXenosGpu.framebufferChecksum() << std::dec
                                  << "\n" << std::flush;
                        readbackReported = true;
                    }
                }
                glfwSwapBuffers(window);
                glfwPollEvents();
                uint16_t buttons = 0;
                buttons |= glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS ? 0x0001u : 0u;
                buttons |= glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS ? 0x0002u : 0u;
                buttons |= glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS ? 0x0004u : 0u;
                buttons |= glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS ? 0x0008u : 0u;
                buttons |= glfwGetKey(window, GLFW_KEY_ENTER) == GLFW_PRESS ? 0x0010u : 0u;
                buttons |= glfwGetKey(window, GLFW_KEY_BACKSPACE) == GLFW_PRESS ? 0x0020u : 0u;
                buttons |= glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS ? 0x1000u : 0u;
                buttons |= glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS ? 0x2000u : 0u;
                gInputButtons.store(buttons, std::memory_order_relaxed);
            }
            glfwDestroyWindow(window);
            glfwTerminate();
            // PPC worker threads are guest-owned and have no cancellation ABI
            // yet. Keep their guest backing alive until process exit.
            guest.release();
            guestThread.detach();
            return 0;
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
