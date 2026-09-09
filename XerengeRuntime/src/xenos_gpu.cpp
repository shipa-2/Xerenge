#include "xenos_gpu.h"
#include "shader_cache_runtime.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>

#define XXH_INLINE_ALL
#include <xxhash.h>

XenosGpu::~XenosGpu()
{
    if (vulkanDevice_ != VK_NULL_HANDLE)
    {
        vkDeviceWaitIdle(vulkanDevice_);
        if (vulkanVertexMapped_ != nullptr)
            vkUnmapMemory(vulkanDevice_, vulkanVertexMemory_);
        if (vulkanConstantsMapped_ != nullptr)
            vkUnmapMemory(vulkanDevice_, vulkanConstantsMemory_);
        if (vulkanReadbackMapped_ != nullptr)
            vkUnmapMemory(vulkanDevice_, vulkanReadbackMemory_);
        if (vulkanTextureUploadMapped_ != nullptr)
            vkUnmapMemory(vulkanDevice_, vulkanTextureUploadMemory_);
        if (vulkanFence_ != VK_NULL_HANDLE)
            vkDestroyFence(vulkanDevice_, vulkanFence_, nullptr);
        if (vulkanCommandPool_ != VK_NULL_HANDLE)
            vkDestroyCommandPool(vulkanDevice_, vulkanCommandPool_, nullptr);
        if (vulkanDescriptorPool_ != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(vulkanDevice_, vulkanDescriptorPool_, nullptr);
        if (vulkanSampler_ != VK_NULL_HANDLE)
            vkDestroySampler(vulkanDevice_, vulkanSampler_, nullptr);
        if (vulkanFramebuffer_ != VK_NULL_HANDLE)
            vkDestroyFramebuffer(vulkanDevice_, vulkanFramebuffer_, nullptr);
        if (vulkanColorView_ != VK_NULL_HANDLE)
            vkDestroyImageView(vulkanDevice_, vulkanColorView_, nullptr);
        if (vulkanWhiteView_ != VK_NULL_HANDLE)
            vkDestroyImageView(vulkanDevice_, vulkanWhiteView_, nullptr);
        if (vulkanColorImage_ != VK_NULL_HANDLE)
            vkDestroyImage(vulkanDevice_, vulkanColorImage_, nullptr);
        if (vulkanWhiteImage_ != VK_NULL_HANDLE)
            vkDestroyImage(vulkanDevice_, vulkanWhiteImage_, nullptr);
        if (vulkanVertexBuffer_ != VK_NULL_HANDLE)
            vkDestroyBuffer(vulkanDevice_, vulkanVertexBuffer_, nullptr);
        if (vulkanConstantsBuffer_ != VK_NULL_HANDLE)
            vkDestroyBuffer(vulkanDevice_, vulkanConstantsBuffer_, nullptr);
        if (vulkanReadbackBuffer_ != VK_NULL_HANDLE)
            vkDestroyBuffer(vulkanDevice_, vulkanReadbackBuffer_, nullptr);
        if (vulkanTextureUploadBuffer_ != VK_NULL_HANDLE)
            vkDestroyBuffer(vulkanDevice_, vulkanTextureUploadBuffer_, nullptr);
        if (vulkanColorMemory_ != VK_NULL_HANDLE)
            vkFreeMemory(vulkanDevice_, vulkanColorMemory_, nullptr);
        if (vulkanWhiteMemory_ != VK_NULL_HANDLE)
            vkFreeMemory(vulkanDevice_, vulkanWhiteMemory_, nullptr);
        if (vulkanVertexMemory_ != VK_NULL_HANDLE)
            vkFreeMemory(vulkanDevice_, vulkanVertexMemory_, nullptr);
        if (vulkanConstantsMemory_ != VK_NULL_HANDLE)
            vkFreeMemory(vulkanDevice_, vulkanConstantsMemory_, nullptr);
        if (vulkanReadbackMemory_ != VK_NULL_HANDLE)
            vkFreeMemory(vulkanDevice_, vulkanReadbackMemory_, nullptr);
        if (vulkanTextureUploadMemory_ != VK_NULL_HANDLE)
            vkFreeMemory(vulkanDevice_, vulkanTextureUploadMemory_, nullptr);
        for (const auto& [_, pipeline] : vulkanPipelines_)
            vkDestroyPipeline(vulkanDevice_, pipeline, nullptr);
        if (vulkanRenderPass_ != VK_NULL_HANDLE)
            vkDestroyRenderPass(vulkanDevice_, vulkanRenderPass_, nullptr);
        if (vulkanPipelineLayout_ != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(vulkanDevice_, vulkanPipelineLayout_, nullptr);
        for (VkDescriptorSetLayout layout : vulkanDescriptorSetLayouts_)
            if (layout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(vulkanDevice_, layout, nullptr);
        for (const auto& [_, module] : vulkanShaderModules_)
            vkDestroyShaderModule(vulkanDevice_, module, nullptr);
        vkDestroyDevice(vulkanDevice_, nullptr);
    }
    if (vulkanInstance_ != VK_NULL_HANDLE)
        vkDestroyInstance(vulkanInstance_, nullptr);
}

bool XenosGpu::initializeVulkan()
{
    std::lock_guard lock(mutex_);
    if (vulkanDevice_ != VK_NULL_HANDLE)
        return true;

    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "Xerenge Xenos backend";
    // The backend uses Vulkan 1.2 promoted feature queries and descriptor
    // indexing, so the instance must expose 1.2 before querying the feature
    // chain below.
    application.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &application;
    const VkResult instanceResult = vkCreateInstance(&instanceInfo, nullptr, &vulkanInstance_);
    if (instanceResult != VK_SUCCESS)
    {
        if (std::getenv("XERENGE_XENOS_VULKAN_TRACE") != nullptr)
            std::cerr << "Xenos Vulkan instance creation failed result=" << instanceResult << '\n';
        return false;
    }

    uint32_t physicalCount = 0;
    vkEnumeratePhysicalDevices(vulkanInstance_, &physicalCount, nullptr);
    std::vector<VkPhysicalDevice> physicalDevices(physicalCount);
    vkEnumeratePhysicalDevices(vulkanInstance_, &physicalCount, physicalDevices.data());
    if (std::getenv("XERENGE_XENOS_VULKAN_TRACE") != nullptr)
        std::cerr << "Xenos Vulkan physical devices=" << physicalCount << '\n';
    for (VkPhysicalDevice candidate : physicalDevices)
    {
        VkPhysicalDeviceProperties candidateProperties{};
        vkGetPhysicalDeviceProperties(candidate, &candidateProperties);
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
        uint32_t candidateQueueFamily = UINT32_MAX;
        for (uint32_t i = 0; i < familyCount; ++i)
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0u)
            {
                candidateQueueFamily = i;
                break;
            }
        if (candidateQueueFamily == UINT32_MAX)
            continue;

        VkPhysicalDeviceVulkan12Features available12{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceFeatures2 available{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        available.pNext = &available12;
        vkGetPhysicalDeviceFeatures2(candidate, &available);
        if (std::getenv("XERENGE_XENOS_VULKAN_TRACE") != nullptr)
            std::cerr << "Xenos Vulkan candidate '" << candidateProperties.deviceName
                      << "' clip=" << available.features.shaderClipDistance
                      << " int64=" << available.features.shaderInt64
                      << " bda=" << available12.bufferDeviceAddress
                      << " runtimeArray=" << available12.runtimeDescriptorArray
                      << " partial=" << available12.descriptorBindingPartiallyBound
                      << " variable=" << available12.descriptorBindingVariableDescriptorCount
                      << '\n';
        if (!available.features.shaderClipDistance || !available.features.shaderInt64 ||
            !available12.bufferDeviceAddress ||
            !available12.runtimeDescriptorArray ||
            !available12.descriptorBindingPartiallyBound ||
            !available12.descriptorBindingVariableDescriptorCount)
            continue;

        vulkanPhysicalDevice_ = candidate;
        vulkanQueueFamily_ = candidateQueueFamily;
        break;
    }
    if (vulkanPhysicalDevice_ == VK_NULL_HANDLE)
        return false;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = vulkanQueueFamily_;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan12Features available12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan12Features requested12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    requested12.bufferDeviceAddress = VK_TRUE;
    requested12.runtimeDescriptorArray = VK_TRUE;
    requested12.descriptorBindingPartiallyBound = VK_TRUE;
    requested12.descriptorBindingVariableDescriptorCount = VK_TRUE;
    VkPhysicalDeviceFeatures requested{};
    requested.shaderClipDistance = VK_TRUE;
    requested.shaderInt64 = VK_TRUE;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.pNext = &requested12;
    deviceInfo.pEnabledFeatures = &requested;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    if (vkCreateDevice(vulkanPhysicalDevice_, &deviceInfo, nullptr, &vulkanDevice_) != VK_SUCCESS)
        return false;
    vkGetDeviceQueue(vulkanDevice_, vulkanQueueFamily_, 0, &vulkanQueue_);

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(vulkanPhysicalDevice_, &properties);
    std::cout << "Xenos Vulkan backend: " << properties.deviceName << '\n';
    return true;
}

bool XenosGpu::ensureShaderModule(uint64_t shaderHash)
{
    if (vulkanDevice_ == VK_NULL_HANDLE)
        return false;
    if (vulkanShaderModules_.find(shaderHash) != vulkanShaderModules_.end())
        return true;
    const auto cache = xerengeShaderCache();
    const auto* entry = cache.find(shaderHash);
    if (entry == nullptr)
        return false;
    size_t wordCount = 0;
    const uint32_t* words = cache.spirv(*entry, wordCount);
    if (words == nullptr)
        return false;
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = wordCount * sizeof(uint32_t);
    info.pCode = words;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(vulkanDevice_, &info, nullptr, &module) != VK_SUCCESS)
        return false;
    vulkanShaderModules_.emplace(shaderHash, module);
    return true;
}

uint32_t XenosGpu::findMemoryType(
    uint32_t typeBits, VkMemoryPropertyFlags properties) const
{
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(vulkanPhysicalDevice_, &memoryProperties);
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
        if ((typeBits & (1u << i)) != 0u &&
            (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    return UINT32_MAX;
}

bool XenosGpu::initializeDrawResources()
{
    if (vulkanFramebuffer_ != VK_NULL_HANDLE)
        return true;
    if (vulkanDevice_ == VK_NULL_HANDLE || vulkanRenderPass_ == VK_NULL_HANDLE)
        return false;

    auto createImage = [&](uint32_t width, uint32_t height, VkImageUsageFlags usage,
                           VkImage& image, VkDeviceMemory& memory)
    {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent = {width, height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(vulkanDevice_, &info, nullptr, &image) != VK_SUCCESS)
            return false;
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(vulkanDevice_, image, &requirements);
        const uint32_t memoryType = findMemoryType(requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memoryType == UINT32_MAX)
            return false;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memoryType;
        if (vkAllocateMemory(vulkanDevice_, &allocation, nullptr, &memory) != VK_SUCCESS ||
            vkBindImageMemory(vulkanDevice_, image, memory, 0) != VK_SUCCESS)
            return false;
        return true;
    };
    auto createBuffer = [&](VkDeviceSize size, VkBufferUsageFlags usage,
                            VkBuffer& buffer, VkDeviceMemory& memory, void*& mapped,
                            bool deviceAddress)
    {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = usage;
        if (vkCreateBuffer(vulkanDevice_, &info, nullptr, &buffer) != VK_SUCCESS)
            return false;
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(vulkanDevice_, buffer, &requirements);
        const uint32_t memoryType = findMemoryType(requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (memoryType == UINT32_MAX)
            return false;
        VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        flags.flags = deviceAddress ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT : 0;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.pNext = deviceAddress ? &flags : nullptr;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memoryType;
        if (vkAllocateMemory(vulkanDevice_, &allocation, nullptr, &memory) != VK_SUCCESS ||
            vkBindBufferMemory(vulkanDevice_, buffer, memory, 0) != VK_SUCCESS ||
            vkMapMemory(vulkanDevice_, memory, 0, size, 0, &mapped) != VK_SUCCESS)
            return false;
        return true;
    };
    if (!createImage(1280, 720, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            vulkanColorImage_, vulkanColorMemory_) ||
        !createImage(1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            vulkanWhiteImage_, vulkanWhiteMemory_))
        return false;

    auto createView = [&](VkImage image, VkImageView& view)
    {
        VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        info.image = image;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        info.subresourceRange.levelCount = 1;
        info.subresourceRange.layerCount = 1;
        return vkCreateImageView(vulkanDevice_, &info, nullptr, &view) == VK_SUCCESS;
    };
    if (!createView(vulkanColorImage_, vulkanColorView_) ||
        !createView(vulkanWhiteImage_, vulkanWhiteView_))
        return false;

    VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebufferInfo.renderPass = vulkanRenderPass_;
    framebufferInfo.attachmentCount = 1;
    framebufferInfo.pAttachments = &vulkanColorView_;
    framebufferInfo.width = 1280;
    framebufferInfo.height = 720;
    framebufferInfo.layers = 1;
    if (vkCreateFramebuffer(vulkanDevice_, &framebufferInfo, nullptr,
            &vulkanFramebuffer_) != VK_SUCCESS)
        return false;

    constexpr VkDeviceSize frameBytes = VkDeviceSize(1280) * 720 * 4;
    if (!createBuffer(4u * 1024u * 1024u, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            vulkanVertexBuffer_, vulkanVertexMemory_, vulkanVertexMapped_, false) ||
        !createBuffer(12u * 1024u, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            vulkanConstantsBuffer_, vulkanConstantsMemory_, vulkanConstantsMapped_, true) ||
        !createBuffer(frameBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            vulkanReadbackBuffer_, vulkanReadbackMemory_, vulkanReadbackMapped_, false) ||
        !createBuffer(16u * 1024u * 1024u, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            vulkanTextureUploadBuffer_, vulkanTextureUploadMemory_,
            vulkanTextureUploadMapped_, false))
        return false;
    VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    addressInfo.buffer = vulkanConstantsBuffer_;
    vulkanConstantsAddress_ = vkGetBufferDeviceAddress(vulkanDevice_, &addressInfo);
    if (vulkanConstantsAddress_ == 0)
        return false;

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0;
    if (vkCreateSampler(vulkanDevice_, &samplerInfo, nullptr, &vulkanSampler_) != VK_SUCCESS)
        return false;

    const VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 96},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 96},
    };
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 4;
    poolInfo.poolSizeCount = std::size(poolSizes);
    poolInfo.pPoolSizes = poolSizes;
    if (vkCreateDescriptorPool(vulkanDevice_, &poolInfo, nullptr,
            &vulkanDescriptorPool_) != VK_SUCCESS)
        return false;
    const uint32_t descriptorCounts[] = {96, 0, 0, 96};
    VkDescriptorSetVariableDescriptorCountAllocateInfo countInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO};
    countInfo.descriptorSetCount = vulkanDescriptorSetLayouts_.size();
    countInfo.pDescriptorCounts = descriptorCounts;
    VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setInfo.pNext = &countInfo;
    setInfo.descriptorPool = vulkanDescriptorPool_;
    setInfo.descriptorSetCount = vulkanDescriptorSetLayouts_.size();
    setInfo.pSetLayouts = vulkanDescriptorSetLayouts_.data();
    if (vkAllocateDescriptorSets(vulkanDevice_, &setInfo,
            vulkanDescriptorSets_.data()) != VK_SUCCESS)
        return false;
    std::array<VkDescriptorImageInfo, 96> images{};
    std::array<VkDescriptorImageInfo, 96> samplers{};
    for (uint32_t i = 0; i < images.size(); ++i)
    {
        images[i].imageView = vulkanWhiteView_;
        images[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        samplers[i].sampler = vulkanSampler_;
    }
    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = vulkanDescriptorSets_[0];
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[0].descriptorCount = images.size();
    writes[0].pImageInfo = images.data();
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = vulkanDescriptorSets_[3];
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    writes[1].descriptorCount = samplers.size();
    writes[1].pImageInfo = samplers.data();
    vkUpdateDescriptorSets(vulkanDevice_, std::size(writes), writes, 0, nullptr);

    VkCommandPoolCreateInfo commandPoolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    commandPoolInfo.queueFamilyIndex = vulkanQueueFamily_;
    if (vkCreateCommandPool(vulkanDevice_, &commandPoolInfo, nullptr,
            &vulkanCommandPool_) != VK_SUCCESS)
        return false;
    VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandInfo.commandPool = vulkanCommandPool_;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(vulkanDevice_, &commandInfo,
            &vulkanCommandBuffer_) != VK_SUCCESS)
        return false;
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    return vkCreateFence(vulkanDevice_, &fenceInfo, nullptr, &vulkanFence_) == VK_SUCCESS;
}

bool XenosGpu::ensureGraphicsPipeline(VkPrimitiveTopology topology)
{
    const auto cache = xerengeShaderCache();
    const auto* vertexMicrocode = cache.findMicrocode(activeVertexShaderHash_);
    const auto* pixelMicrocode = cache.findMicrocode(activePixelShaderHash_);
    const bool deviceReady = vulkanDevice_ != VK_NULL_HANDLE;
    const bool vertexFound = vertexMicrocode != nullptr;
    const bool pixelFound = pixelMicrocode != nullptr;
    const bool vertexModule = vertexFound && ensureShaderModule(vertexMicrocode->shaderHash);
    const bool pixelModule = pixelFound && ensureShaderModule(pixelMicrocode->shaderHash);
    if (!deviceReady || !vertexFound || !pixelFound || !vertexModule || !pixelModule)
    {
        if (std::getenv("XERENGE_XENOS_SHADER_TRACE") != nullptr)
        {
            static std::atomic<uint32_t> reasonTraceCount = 0;
            if (reasonTraceCount.fetch_add(1, std::memory_order_relaxed) < 8)
                std::cerr << "Xenos Vulkan pipeline prerequisites device=" << deviceReady
                          << " vsFound=" << vertexFound << " psFound=" << pixelFound
                          << " vsModule=" << vertexModule << " psModule=" << pixelModule
                          << " activeVs=0x" << std::hex << activeVertexShaderHash_
                          << " activePs=0x" << activePixelShaderHash_ << std::dec << '\n';
        }
        return false;
    }

    const uint32_t blendControl = gpuRegisters_[0x2201u];
    const uint32_t colorMask = gpuRegisters_[0x2104u] & 0xFu;
    // The prototype's bootstrap packets leave RB_COLOR_MASK at zero while
    // still issuing visible setup draws. Keep those draws observable until
    // the complete render-target register mapping is implemented.
    const uint32_t effectiveColorMask = colorMask == 0u ? 0xFu : colorMask;
    uint64_t key = activeVertexShaderHash_ ^
        (activePixelShaderHash_ + 0x9E3779B97F4A7C15ull +
            (activeVertexShaderHash_ << 6) + (activeVertexShaderHash_ >> 2));
    key ^= uint64_t(blendControl) * 0xD6E8FEB86659FD93ull;
    key ^= uint64_t(effectiveColorMask) * 0xA0761D6478BD642Full;
    key ^= uint64_t(topology) * 0xE7037ED1A0B428DBull;
    if (vulkanPipelines_.find(key) != vulkanPipelines_.end())
        return true;

    if (vulkanPipelineLayout_ == VK_NULL_HANDLE)
    {
        for (uint32_t set = 0; set < vulkanDescriptorSetLayouts_.size(); ++set)
        {
            VkDescriptorSetLayoutCreateInfo layoutInfo{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            VkDescriptorSetLayoutBinding binding{};
            VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
            VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
            if (set == 0 || set == 3)
            {
                binding.binding = 0;
                binding.descriptorType = set == 0 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                                  : VK_DESCRIPTOR_TYPE_SAMPLER;
                binding.descriptorCount = 96;
                binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
                flagsInfo.bindingCount = 1;
                flagsInfo.pBindingFlags = &flags;
                layoutInfo.pNext = &flagsInfo;
                layoutInfo.bindingCount = 1;
                layoutInfo.pBindings = &binding;
            }
            if (vkCreateDescriptorSetLayout(vulkanDevice_, &layoutInfo, nullptr,
                    &vulkanDescriptorSetLayouts_[set]) != VK_SUCCESS)
                return false;
        }
        VkPushConstantRange pushConstants{};
        pushConstants.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushConstants.size = 3 * sizeof(uint64_t);
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayoutInfo.setLayoutCount = vulkanDescriptorSetLayouts_.size();
        pipelineLayoutInfo.pSetLayouts = vulkanDescriptorSetLayouts_.data();
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushConstants;
        if (vkCreatePipelineLayout(vulkanDevice_, &pipelineLayoutInfo, nullptr,
                &vulkanPipelineLayout_) != VK_SUCCESS)
            return false;

        VkAttachmentDescription attachment{};
        attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference colorReference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorReference;
        // Each draw uses a separate render pass with LOAD and blending.
        // Make previous attachment writes visible to both operations.
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo renderPassInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        renderPassInfo.attachmentCount = 1;
        renderPassInfo.pAttachments = &attachment;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = 1;
        renderPassInfo.pDependencies = &dependency;
        if (vkCreateRenderPass(vulkanDevice_, &renderPassInfo, nullptr,
                &vulkanRenderPass_) != VK_SUCCESS)
            return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vulkanShaderModules_[vertexMicrocode->shaderHash];
    stages[0].pName = "shaderMain";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = vulkanShaderModules_[pixelMicrocode->shaderHash];
    stages[1].pName = "shaderMain";
    const uint32_t specializationValue = 0;
    VkSpecializationMapEntry specializationEntry{0, 0, sizeof(uint32_t)};
    VkSpecializationInfo specialization{1, &specializationEntry,
        sizeof(specializationValue), &specializationValue};
    stages[1].pSpecializationInfo = &specialization;

    VkVertexInputBindingDescription vertexBinding{0, 12 * sizeof(float),
        VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[] = {
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
        {13, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 4 * sizeof(float)},
        {17, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 8 * sizeof(float)},
    };
    VkPipelineVertexInputStateCreateInfo vertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &vertexBinding;
    vertexInput.vertexAttributeDescriptionCount = std::size(attributes);
    vertexInput.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = topology;
    // XenosRecomp passes -fvk-invert-y to DXC for vertex shaders, so their
    // SPIR-V output already accounts for the Direct3D/Vulkan Y convention.
    // A second inversion in the viewport would mirror native draws again.
    VkViewport viewport{0, 0, 1280, 720, 0, 1};
    VkRect2D scissor{{0, 0}, {1280, 720}};
    VkPipelineViewportStateCreateInfo viewportState{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo rasterization{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blendAttachment{};
    if (effectiveColorMask & 0x1u) blendAttachment.colorWriteMask |= VK_COLOR_COMPONENT_R_BIT;
    if (effectiveColorMask & 0x2u) blendAttachment.colorWriteMask |= VK_COLOR_COMPONENT_G_BIT;
    if (effectiveColorMask & 0x4u) blendAttachment.colorWriteMask |= VK_COLOR_COMPONENT_B_BIT;
    if (effectiveColorMask & 0x8u) blendAttachment.colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;
    auto blendFactor = [](uint32_t factor)
    {
        switch (factor)
        {
        case 0: return VK_BLEND_FACTOR_ZERO;
        case 1: return VK_BLEND_FACTOR_ONE;
        case 4: return VK_BLEND_FACTOR_SRC_COLOR;
        case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 6: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 8: return VK_BLEND_FACTOR_DST_COLOR;
        case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 10: return VK_BLEND_FACTOR_DST_ALPHA;
        case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        default: return VK_BLEND_FACTOR_ONE;
        }
    };
    auto blendOp = [](uint32_t operation)
    {
        switch (operation)
        {
        case 1: return VK_BLEND_OP_SUBTRACT;
        case 2: return VK_BLEND_OP_MIN;
        case 3: return VK_BLEND_OP_MAX;
        case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
        default: return VK_BLEND_OP_ADD;
        }
    };
    const uint32_t colorSource = blendControl & 0x1Fu;
    const uint32_t colorOperation = (blendControl >> 5) & 0x7u;
    const uint32_t colorDestination = (blendControl >> 8) & 0x1Fu;
    const uint32_t alphaSource = (blendControl >> 16) & 0x1Fu;
    const uint32_t alphaOperation = (blendControl >> 21) & 0x7u;
    const uint32_t alphaDestination = (blendControl >> 24) & 0x1Fu;
    blendAttachment.blendEnable =
        colorSource != 1u || colorDestination != 0u || colorOperation != 0u ||
        alphaSource != 1u || alphaDestination != 0u || alphaOperation != 0u;
    blendAttachment.srcColorBlendFactor = blendFactor(colorSource);
    blendAttachment.dstColorBlendFactor = blendFactor(colorDestination);
    blendAttachment.colorBlendOp = blendOp(colorOperation);
    blendAttachment.srcAlphaBlendFactor = blendFactor(alphaSource);
    blendAttachment.dstAlphaBlendFactor = blendFactor(alphaDestination);
    blendAttachment.alphaBlendOp = blendOp(alphaOperation);
    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;
    VkGraphicsPipelineCreateInfo pipelineInfo{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &assembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterization;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.layout = vulkanPipelineLayout_;
    pipelineInfo.renderPass = vulkanRenderPass_;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = vkCreateGraphicsPipelines(vulkanDevice_, VK_NULL_HANDLE, 1,
        &pipelineInfo, nullptr, &pipeline);
    if (result != VK_SUCCESS)
    {
        if (std::getenv("XERENGE_XENOS_SHADER_TRACE") != nullptr)
            std::cerr << "Xenos Vulkan pipeline failed: " << result << '\n';
        return false;
    }
    vulkanPipelines_.emplace(key, pipeline);
    if (std::getenv("XERENGE_XENOS_SHADER_TRACE") != nullptr)
        std::cerr << "Xenos Vulkan pipeline ready vs=0x" << std::hex
                  << activeVertexShaderHash_ << " ps=0x" << activePixelShaderHash_
                  << std::dec << '\n';
    return true;
}

bool XenosGpu::drawVulkanGeometry(const float* vertices, uint32_t vertexCount,
    VkPrimitiveTopology topology,
    const uint8_t* texture, uint32_t textureWidth, uint32_t textureHeight,
    uint64_t textureKey)
{
    constexpr uint32_t vertexCapacity = (4u * 1024u * 1024u) /
        (12u * sizeof(float));
    const bool validCount = topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST
        ? vertexCount != 0
        : vertexCount >= 3 && vertexCount % 3 == 0;
    if (!validCount || vertexCount > vertexCapacity ||
        !ensureGraphicsPipeline(topology) || !initializeDrawResources())
        return false;
    const uint32_t blendControl = gpuRegisters_[0x2201u];
    const uint32_t colorMask = gpuRegisters_[0x2104u] & 0xFu;
    const uint32_t effectiveColorMask = colorMask == 0u ? 0xFu : colorMask;
    uint64_t key = activeVertexShaderHash_ ^
        (activePixelShaderHash_ + 0x9E3779B97F4A7C15ull +
            (activeVertexShaderHash_ << 6) + (activeVertexShaderHash_ >> 2));
    key ^= uint64_t(blendControl) * 0xD6E8FEB86659FD93ull;
    key ^= uint64_t(effectiveColorMask) * 0xA0761D6478BD642Full;
    key ^= uint64_t(topology) * 0xE7037ED1A0B428DBull;
    const auto pipeline = vulkanPipelines_.find(key);
    if (pipeline == vulkanPipelines_.end())
        return false;

    constexpr size_t textureUploadCapacity = 16u * 1024u * 1024u;
    const size_t textureBytes = size_t(textureWidth) * textureHeight * 4;
    if (texture != nullptr && textureWidth != 0 && textureHeight != 0 &&
        textureBytes <= textureUploadCapacity && textureKey != vulkanTextureKey_)
    {
        const bool textureSizeChanged = textureWidth != vulkanTextureWidth_ ||
            textureHeight != vulkanTextureHeight_;
        if (textureSizeChanged)
        {
            // The old image may still be referenced by the previous submit.
            // The draw path waits on this fence before returning, so wait for
            // that submit only instead of stalling the whole queue.
            if (vkWaitForFences(vulkanDevice_, 1, &vulkanFence_, VK_TRUE,
                    UINT64_MAX) != VK_SUCCESS)
                return false;
            if (vulkanWhiteView_ != VK_NULL_HANDLE)
                vkDestroyImageView(vulkanDevice_, vulkanWhiteView_, nullptr);
            if (vulkanWhiteImage_ != VK_NULL_HANDLE)
                vkDestroyImage(vulkanDevice_, vulkanWhiteImage_, nullptr);
            if (vulkanWhiteMemory_ != VK_NULL_HANDLE)
                vkFreeMemory(vulkanDevice_, vulkanWhiteMemory_, nullptr);
            vulkanWhiteView_ = VK_NULL_HANDLE;
            vulkanWhiteImage_ = VK_NULL_HANDLE;
            vulkanWhiteMemory_ = VK_NULL_HANDLE;
            VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
            imageInfo.extent = {textureWidth, textureHeight, 1};
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(vulkanDevice_, &imageInfo, nullptr, &vulkanWhiteImage_) != VK_SUCCESS)
                return false;
            VkMemoryRequirements requirements{};
            vkGetImageMemoryRequirements(vulkanDevice_, vulkanWhiteImage_, &requirements);
            const uint32_t memoryType = findMemoryType(requirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (memoryType == UINT32_MAX)
                return false;
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = memoryType;
            if (vkAllocateMemory(vulkanDevice_, &allocation, nullptr,
                    &vulkanWhiteMemory_) != VK_SUCCESS ||
                vkBindImageMemory(vulkanDevice_, vulkanWhiteImage_,
                    vulkanWhiteMemory_, 0) != VK_SUCCESS)
                return false;
            VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image = vulkanWhiteImage_;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.layerCount = 1;
            if (vkCreateImageView(vulkanDevice_, &viewInfo, nullptr,
                    &vulkanWhiteView_) != VK_SUCCESS)
                return false;
            vulkanTextureImageInitialized_ = false;
            std::array<VkDescriptorImageInfo, 96> images{};
            for (auto& image : images)
            {
                image.imageView = vulkanWhiteView_;
                image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = vulkanDescriptorSets_[0];
            write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            write.descriptorCount = images.size();
            write.pImageInfo = images.data();
            vkUpdateDescriptorSets(vulkanDevice_, 1, &write, 0, nullptr);
        }
        else
        {
            // The command buffer is submitted synchronously below, but wait
            // here before replacing the host upload buffer used by that submit.
            if (vkWaitForFences(vulkanDevice_, 1, &vulkanFence_, VK_TRUE,
                    UINT64_MAX) != VK_SUCCESS)
                return false;
        }
        std::memcpy(vulkanTextureUploadMapped_, texture, textureBytes);
        vulkanTextureWidth_ = textureWidth;
        vulkanTextureHeight_ = textureHeight;
        vulkanTextureKey_ = textureKey;
        vulkanTextureInitialized_ = false;
        if (std::getenv("XERENGE_XENOS_VULKAN_TRACE") != nullptr)
            std::cerr << "Xenos Vulkan texture upload=" << textureWidth << 'x'
                      << textureHeight << " key=0x" << std::hex << textureKey
                      << std::dec << '\n';
    }

    std::memcpy(vulkanVertexMapped_, vertices,
        size_t(vertexCount) * 12 * sizeof(float));
    std::memset(vulkanConstantsMapped_, 0, 12u * 1024u);
    auto* constants = static_cast<uint32_t*>(vulkanConstantsMapped_);
    // Xenos has 256 float4 constants per stage in adjacent register banks.
    // Register writes were already byte-swapped while parsing PM4, so copying
    // their bit patterns produces native IEEE floats for the generated SPIR-V.
    std::copy_n(gpuRegisters_.data() + 0x4000u, 1024u, constants);
    std::copy_n(gpuRegisters_.data() + 0x4400u, 1024u, constants + 1024u);
    struct PushConstants
    {
        uint64_t vertex;
        uint64_t pixel;
        uint64_t shared;
    } push{
        vulkanConstantsAddress_,
        vulkanConstantsAddress_ + 4096,
        vulkanConstantsAddress_ + 8192,
    };

    vkWaitForFences(vulkanDevice_, 1, &vulkanFence_, VK_TRUE, UINT64_MAX);
    vkResetFences(vulkanDevice_, 1, &vulkanFence_);
    vkResetCommandBuffer(vulkanCommandBuffer_, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(vulkanCommandBuffer_, &begin) != VK_SUCCESS)
        return false;
    auto imageBarrier = [&](VkImage image, VkImageLayout oldLayout,
                            VkImageLayout newLayout, VkAccessFlags sourceAccess,
                            VkAccessFlags destinationAccess,
                            VkPipelineStageFlags sourceStage,
                            VkPipelineStageFlags destinationStage)
    {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = sourceAccess;
        barrier.dstAccessMask = destinationAccess;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(vulkanCommandBuffer_, sourceStage, destinationStage, 0,
            0, nullptr, 0, nullptr, 1, &barrier);
    };
    if (!vulkanImagesInitialized_)
    {
        imageBarrier(vulkanColorImage_, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        const VkClearColorValue black{{0, 0, 0, 0}};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(vulkanCommandBuffer_, vulkanColorImage_,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        imageBarrier(vulkanColorImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    }
    if (!vulkanTextureInitialized_)
    {
        const VkImageLayout oldTextureLayout = vulkanTextureImageInitialized_
            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
        const VkAccessFlags oldTextureAccess = vulkanTextureImageInitialized_
            ? VK_ACCESS_SHADER_READ_BIT : 0;
        const VkPipelineStageFlags oldTextureStage = vulkanTextureImageInitialized_
            ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        imageBarrier(vulkanWhiteImage_, oldTextureLayout,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, oldTextureAccess,
            VK_ACCESS_TRANSFER_WRITE_BIT,
            oldTextureStage, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (texture != nullptr && textureKey == vulkanTextureKey_)
        {
            VkBufferImageCopy textureCopy{};
            textureCopy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            textureCopy.imageSubresource.layerCount = 1;
            textureCopy.imageExtent = {vulkanTextureWidth_, vulkanTextureHeight_, 1};
            vkCmdCopyBufferToImage(vulkanCommandBuffer_, vulkanTextureUploadBuffer_,
                vulkanWhiteImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &textureCopy);
        }
        else
        {
            const VkClearColorValue white{{1, 1, 1, 1}};
            vkCmdClearColorImage(vulkanCommandBuffer_, vulkanWhiteImage_,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &range);
        }
        imageBarrier(vulkanWhiteImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }

    VkRenderPassBeginInfo renderBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    renderBegin.renderPass = vulkanRenderPass_;
    renderBegin.framebuffer = vulkanFramebuffer_;
    renderBegin.renderArea.extent = {1280, 720};
    vkCmdBeginRenderPass(vulkanCommandBuffer_, &renderBegin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(vulkanCommandBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipeline->second);
    vkCmdBindDescriptorSets(vulkanCommandBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS,
        vulkanPipelineLayout_, 0, vulkanDescriptorSets_.size(),
        vulkanDescriptorSets_.data(), 0, nullptr);
    const VkDeviceSize vertexOffset = 0;
    vkCmdBindVertexBuffers(vulkanCommandBuffer_, 0, 1, &vulkanVertexBuffer_,
        &vertexOffset);
    vkCmdPushConstants(vulkanCommandBuffer_, vulkanPipelineLayout_,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
        sizeof(push), &push);
    vkCmdDraw(vulkanCommandBuffer_, vertexCount, 1, 0, 0);
    vkCmdEndRenderPass(vulkanCommandBuffer_);
    if (vkEndCommandBuffer(vulkanCommandBuffer_) != VK_SUCCESS)
        return false;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &vulkanCommandBuffer_;
    if (vkQueueSubmit(vulkanQueue_, 1, &submit, vulkanFence_) != VK_SUCCESS ||
        vkWaitForFences(vulkanDevice_, 1, &vulkanFence_, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
        return false;
    vulkanImagesInitialized_ = true;
    vulkanTextureInitialized_ = true;
    vulkanTextureImageInitialized_ = true;
    ++vulkanDrawCount_;
    const bool largestDraw = vertexCount > vulkanLargestDrawVertexCount_;
    vulkanLargestDrawVertexCount_ = std::max(vulkanLargestDrawVertexCount_, vertexCount);
    if (std::getenv("XERENGE_XENOS_VULKAN_TRACE") != nullptr &&
        (vulkanDrawCount_ <= 8 || largestDraw || (vulkanDrawCount_ % 256) == 0))
        std::cerr << "Xenos Vulkan draw=" << vulkanDrawCount_
                  << " vertices=" << vertexCount
                  << " readback=0x" << std::hex
                  << XXH3_64bits(edram_.data(), edram_.size()) << std::dec << '\n';
    return true;
}

void XenosGpu::readbackVulkanFrame()
{
    std::lock_guard lock(mutex_);
    if (vulkanDevice_ == VK_NULL_HANDLE || !vulkanImagesInitialized_ ||
        vulkanCommandBuffer_ == VK_NULL_HANDLE || vulkanFence_ == VK_NULL_HANDLE)
        return;
    if (vkWaitForFences(vulkanDevice_, 1, &vulkanFence_, VK_TRUE,
            UINT64_MAX) != VK_SUCCESS)
        return;
    vkResetFences(vulkanDevice_, 1, &vulkanFence_);
    vkResetCommandBuffer(vulkanCommandBuffer_, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(vulkanCommandBuffer_, &begin) != VK_SUCCESS)
        return;
    VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toTransfer.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = vulkanColorImage_;
    toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toTransfer.subresourceRange.levelCount = 1;
    toTransfer.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(vulkanCommandBuffer_,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toTransfer);
    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {1280, 720, 1};
    vkCmdCopyImageToBuffer(vulkanCommandBuffer_, vulkanColorImage_,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vulkanReadbackBuffer_, 1, &copy);
    VkImageMemoryBarrier toColor = toTransfer;
    toColor.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toColor.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(vulkanCommandBuffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1,
        &toColor);
    if (vkEndCommandBuffer(vulkanCommandBuffer_) != VK_SUCCESS)
        return;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &vulkanCommandBuffer_;
    if (vkQueueSubmit(vulkanQueue_, 1, &submit, vulkanFence_) != VK_SUCCESS ||
        vkWaitForFences(vulkanDevice_, 1, &vulkanFence_, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
        return;
    constexpr size_t frameBytes = size_t(1280) * 720 * 4;
    edram_.resize(frameBytes);
    std::memcpy(edram_.data(), vulkanReadbackMapped_, frameBytes);
}

namespace
{
constexpr uint32_t kEdramSize = 10u * 1024u * 1024u;

uint32_t gpuPhysicalToGuest(uint32_t physicalAddress)
{
    return 0x60000000u | (physicalAddress & 0x1FFFFFFFu);
}

uint32_t loadGuestBE(const uint8_t* base, uint32_t address)
{
    uint32_t value = 0;
    std::memcpy(&value, base + address, sizeof(value));
    return __builtin_bswap32(value);
}

void storeGuestBE(uint8_t* base, uint32_t address, uint32_t value)
{
    const uint32_t encoded = __builtin_bswap32(value);
    std::memcpy(base + address, &encoded, sizeof(encoded));
}

uint32_t gpuSwap32(uint32_t value, uint32_t endian)
{
    switch (endian & 3u)
    {
    case 1:
        return ((value & 0x00FF00FFu) << 8) |
            ((value & 0xFF00FF00u) >> 8);
    case 2:
        return __builtin_bswap32(value);
    case 3:
        value = ((value & 0x0000FFFFu) << 16) |
            ((value & 0xFFFF0000u) >> 16);
        return ((value & 0xFF00FF00u) >> 8) |
            ((value & 0x00FF00FFu) << 8);
    default:
        return value;
    }
}

void writeGpuMemory(uint8_t* guestBase, uint32_t encodedAddress, uint32_t value)
{
    const uint32_t destination = gpuPhysicalToGuest(encodedAddress & ~3u);
    if (destination < 0x80000000u)
        storeGuestBE(guestBase, destination, gpuSwap32(value, encodedAddress));
}

void writeEventToGuest(uint8_t* guestBase, uint32_t initiator,
    uint32_t encodedAddress, uint32_t requestedValue, uint64_t frameCount)
{
    const uint32_t physicalAddress = encodedAddress & ~3u;
    const uint32_t destination = gpuPhysicalToGuest(physicalAddress);
    uint32_t value = (initiator & 0x80000000u) != 0
        ? static_cast<uint32_t>(frameCount) : requestedValue;
    if (destination < 0x80000000u)
        storeGuestBE(guestBase, destination, value);
}

}

void XenosGpu::present(uint32_t width, uint32_t height)
{
    std::lock_guard lock(mutex_);
    width = std::clamp(width, 1u, 4096u);
    height = std::clamp(height, 1u, 4096u);
    framebuffer_.assign(static_cast<size_t>(width) * height * 4, 0);
    for (size_t i = 3; i < framebuffer_.size(); i += 4)
        framebuffer_[i] = 255;
    lastFrameWidth_ = width;
    lastFrameHeight_ = height;
}

bool XenosGpu::presentFromGuest(uint8_t* guestBase, uint32_t guestAddress,
    uint32_t width, uint32_t height, uint32_t sourcePitch)
{
    std::lock_guard lock(mutex_);
    width = std::clamp(width, 1u, 4096u);
    height = std::clamp(height, 1u, 4096u);
    // The display fetch width and the temporary resolve pitch can differ.
    // Read rows using the producer pitch so scanout cannot shear diagonally.
    if (sourcePitch == 0)
        sourcePitch = width;
    sourcePitch = std::max(sourcePitch, width);
    const size_t sourceByteCount = static_cast<size_t>(sourcePitch) * height * 4;
    const size_t byteCount = static_cast<size_t>(width) * height * 4;
    // Xenos render targets are exposed through the title's 0x60000000
    // physical-memory alias.  Reject low values passed by a malformed or
    // not-yet-initialized VdSwap call instead of copying arbitrary guest
    // memory into the displayed framebuffer.
    if (guestAddress < 0x60000000u || guestAddress >= 0x80000000u ||
        guestAddress > 0xFFFFFFFFu - sourceByteCount)
        return false;

    // The first bring-up path uses the guest surface as a linear X8R8G8B8
    // readback.  Xenos tiling/swizzle is handled separately once command
    // packets identify the render-target format; keeping the copy here makes
    // a title-provided frontbuffer observable without fabricating pixels.
    // The current Vulkan bring-up renders into a native color image and does
    // not yet implement the complete Xenos tiled resolve into the title's
    // frontbuffer. Some VdSwap calls therefore point at a surface containing
    // only its clear value even though the GPU image has a real frame. Keep
    // the title-provided surface as the primary source, but use the completed
    // Vulkan readback when that surface is demonstrably empty.
    framebuffer_.resize(byteCount);
    size_t guestNonzeroRgbPixels = 0;
    for (uint32_t y = 0; y < height; ++y)
    {
        const uint8_t* row = guestBase + guestAddress +
            static_cast<size_t>(y) * sourcePitch * 4;
        for (uint32_t x = 0; x < width; ++x)
            guestNonzeroRgbPixels += (row[x * 4] | row[x * 4 + 1] |
                row[x * 4 + 2]) != 0;
    }
    const bool useVulkanReadback = guestNonzeroRgbPixels < 8 &&
        vulkanImagesInitialized_ && edram_.size() >= byteCount;
    if (useVulkanReadback)
    {
        const size_t rowBytes = static_cast<size_t>(width) * 4;
        const size_t sourceRowBytes = 1280u * 4;
        for (uint32_t y = 0; y < height; ++y)
        {
            uint8_t* destination = framebuffer_.data() + static_cast<size_t>(y) * rowBytes;
            const uint8_t* source = edram_.data() + static_cast<size_t>(y) * sourceRowBytes;
            // Vulkan uses the fixed 1280x720 bootstrap render target while
            // the Xenos fetch can request 960x720. Resample horizontally so
            // a valid render at x >= 960 is not discarded as padding.
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint32_t sourceX = std::min(1279u,
                    (x * 1280u) / width);
                std::memcpy(destination + static_cast<size_t>(x) * 4,
                    source + static_cast<size_t>(sourceX) * 4, 4);
            }
        }
    }
    else if (sourcePitch == width)
        std::memcpy(framebuffer_.data(), guestBase + guestAddress, byteCount);
    else
    {
        const size_t rowBytes = static_cast<size_t>(width) * 4;
        const size_t sourceRowBytes = static_cast<size_t>(sourcePitch) * 4;
        for (uint32_t y = 0; y < height; ++y)
            std::memcpy(framebuffer_.data() + static_cast<size_t>(y) * rowBytes,
                guestBase + guestAddress + static_cast<size_t>(y) * sourceRowBytes,
                rowBytes);
    }
    if (std::getenv("XERENGE_FRAMEBUFFER_TRACE") != nullptr)
    {
        size_t nonzeroRgbPixels = 0;
        uint64_t checksum = 1469598103934665603ull;
        for (size_t i = 0; i + 3 < framebuffer_.size(); i += 4)
        {
            nonzeroRgbPixels += (framebuffer_[i] | framebuffer_[i + 1] |
                framebuffer_[i + 2]) != 0;
            for (size_t component = 0; component < 4; ++component)
            {
                checksum ^= framebuffer_[i + component];
                checksum *= 1099511628211ull;
            }
        }
        std::cerr << "Xenos frontbuffer guest=0x" << std::hex << guestAddress
                  << " bytes=" << std::dec << byteCount
                  << " sourcePitch=" << sourcePitch
                  << " guestNonzeroRgbPixels=" << guestNonzeroRgbPixels
                  << " displayedNonzeroRgbPixels=" << nonzeroRgbPixels
                  << " source=" << (useVulkanReadback ? "vulkan" : "guest")
                  << " checksum=0x" << std::hex << checksum << std::dec << '\n';
    }
    // The display engine scans this surface out as X8R8G8B8.  Render-target
    // alpha is not desktop-window transparency; Burnout leaves it at zero
    // while writing valid RGB, so make scanout pixels opaque for OpenGL.
    for (size_t i = 0; i < framebuffer_.size(); i += 4)
        framebuffer_[i + 3] = 255;
    lastFrameWidth_ = width;
    lastFrameHeight_ = height;
    return true;
}

std::vector<uint8_t> XenosGpu::framebufferCopy() const
{
    std::lock_guard lock(mutex_);
    return framebuffer_;
}

uint64_t XenosGpu::framebufferChecksum() const
{
    std::lock_guard lock(mutex_);
    uint64_t hash = 1469598103934665603ull;
    for (const uint8_t byte : framebuffer_)
    {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

void XenosGpu::initializeRingBuffer(uint32_t guestAddress, uint32_t sizeLog2)
{
    std::lock_guard lock(mutex_);
    ringBase_ = guestAddress;
    // CP_RB_CNTL.RB_BLKSZ is the log2 of the number of 8-byte blocks.  The
    // command processor therefore exposes 1 << (sizeLog2 + 3) bytes, or
    // 1 << (sizeLog2 + 1) dwords.  Treating the value as a dword exponent
    // truncates the ring by half and drops the first frame after the cursor
    // crosses the artificial limit.
    ringSizeDwords_ = sizeLog2 < 30 ? (1u << (sizeLog2 + 1)) : 0;
    readPointer_ = 0;
    writePointer_ = 0;
}

void XenosGpu::enableReadPointerWriteBack(uint32_t guestAddress, uint32_t)
{
    std::lock_guard lock(mutex_);
    // VdEnableRingBufferRPtrWriteBack receives a GPU physical address.  The
    // guest observes it through the 0x60000000 physical alias, just like
    // EVENT_WRITE_SHD destinations.
    readPointerWriteback_ = gpuPhysicalToGuest(guestAddress & 0x3FFFFFFCu);
}

void XenosGpu::writeGpuRegister(uint32_t index, uint32_t value)
{
    if (index >= 0x4800u && index < 0x4800u + vertexFetchRegisters_.size())
    {
        const uint32_t fetchIndex = index - 0x4800u;

        // A texture fetch constant occupies six consecutive registers.  Words
        // after the descriptor can end in 3 as ordinary data (the width field
        // commonly does), so only classify a vertex fetch outside an active
        // texture group.
        if (pendingTextureFetchWords_ != 0u)
        {
            if (index == pendingTextureFetchRegister_)
            {
                gpuRegisters_[index] = value;
                ++pendingTextureFetchRegister_;
                if (--pendingTextureFetchWords_ == 0u)
                    pendingTextureFetchRegister_ = 0xFFFFFFFFu;
                return;
            }
            pendingTextureFetchRegister_ = 0xFFFFFFFFu;
            pendingTextureFetchWords_ = 0u;
        }

        if ((fetchIndex % 6u) == 0u && (value & 0x3u) == 2u)
        {
            gpuRegisters_[index] = value;
            pendingTextureFetchRegister_ = index + 1u;
            pendingTextureFetchWords_ = 5u;
            pendingVertexFetchRegister_ = 0xFFFFFFFFu;
            return;
        }

        if ((value & 0x3u) == 3u)
        {
            vertexFetchRegisters_[fetchIndex] = value;
            pendingVertexFetchRegister_ = index;
            return;
        }
        if (pendingVertexFetchRegister_ != 0xFFFFFFFFu &&
            index == pendingVertexFetchRegister_ + 1u)
        {
            vertexFetchRegisters_[fetchIndex] = value;
            pendingVertexFetchRegister_ = 0xFFFFFFFFu;
            return;
        }
        pendingVertexFetchRegister_ = 0xFFFFFFFFu;
    }
    if (index < gpuRegisters_.size())
    {
        gpuRegisters_[index] = value;
        if (std::getenv("XERENGE_XENOS_STATE_TRACE") != nullptr &&
            (index == 0x2000u || index == 0x2001u || index == 0x2104u ||
             index == 0x2208u || index == 0x2318u || index == 0x2319u))
        {
            static std::atomic<uint32_t> stateTraceCount{0};
            const uint32_t sample = stateTraceCount.fetch_add(1, std::memory_order_relaxed);
            if (sample < 128)
                std::cerr << "Xenos state register=0x" << std::hex << index
                          << " value=0x" << value << std::dec << '\n';
        }
        if (index == 0x2104u && std::getenv("XERENGE_XENOS_MASK_TRACE") != nullptr)
            std::cerr << "Xenos RB_COLOR_MASK=0x" << std::hex << value << std::dec << '\n';
    }
}

void XenosGpu::rememberVertexFetchStrides(const uint32_t* code, uint32_t dwordCount)
{
    // Immediate Xenos vertex shaders are triples of dwords.  The stride is
    // carried by the third word of a full vfetch instruction, while the
    // fetch constant index is carried by the first word.  Recording this
    // small piece of metadata lets the bootstrap rasterizer use the same
    // layout as the shader without hard-coding a program id.
    for (uint32_t offset = 0; offset + 2 < dwordCount; offset += 3)
    {
        const uint32_t word0 = code[offset];
        if ((word0 & 0x1Fu) != 0u) // FetchOpcode::VertexFetch.
            continue;
        const uint32_t constantIndex =
            (((word0 >> 20) & 0x1Fu) * 3u) + ((word0 >> 25) & 0x3u);
        const uint32_t stride = code[offset + 2] & 0xFFu;
        const bool isMiniFetch = ((code[offset + 1] >> 30) & 1u) != 0u;
        if (constantIndex < vertexFetchStrideWords_.size() && stride != 0u)
        {
            vertexFetchStrideWords_[constantIndex] = stride;
            if (!isMiniFetch)
                activeVertexFetchConstantIndex_ = constantIndex;
        }
    }
}

void XenosGpu::loadPointerShader(uint8_t* guestBase, uint32_t address,
    uint32_t shaderType, uint32_t startSize)
{
    const uint32_t start = startSize >> 16;
    const uint32_t dwordCount = startSize & 0xFFFFu;
    if (start != 0 || dwordCount == 0 || dwordCount > 0x4000u)
        return;
    const uint32_t guestAddress = gpuPhysicalToGuest(address & 0x3FFFFFFCu);
    const size_t byteSize = size_t(dwordCount) * sizeof(uint32_t);
    const uint64_t hash = XXH3_64bits(guestBase + guestAddress, byteSize);
    if (const char* captureDirectory = std::getenv("XERENGE_XENOS_SHADER_CAPTURE_DIR"))
    {
        std::error_code error;
        std::filesystem::create_directories(captureDirectory, error);
        const auto path = std::filesystem::path(captureDirectory) /
            (std::to_string(hash) + (shaderType == 0u ? ".vs.bin" : ".ps.bin"));
        if (!error && !std::filesystem::exists(path))
        {
            std::ofstream stream(path, std::ios::binary);
            stream.write(reinterpret_cast<const char*>(guestBase + guestAddress), byteSize);
        }
    }
    if (std::getenv("XERENGE_XENOS_SHADER_DUMP") != nullptr)
    {
        std::cerr << "Xenos pointer shader code stage=" << shaderType
                  << " dwords=" << dwordCount << ':';
        for (uint32_t i = 0; i < dwordCount; ++i)
            std::cerr << " " << std::hex
                      << loadGuestBE(guestBase, guestAddress + i * 4);
        std::cerr << std::dec << '\n';
    }
    if (shaderType == 0u)
    {
        activeVertexShaderHash_ = hash;
        activeVertexShaderDwords_ = dwordCount;
        std::array<uint32_t, 1024> code{};
        const uint32_t copied = std::min<uint32_t>(dwordCount, code.size());
        for (uint32_t i = 0; i < copied; ++i)
            code[i] = loadGuestBE(guestBase, guestAddress + i * 4);
        rememberVertexFetchStrides(code.data(), copied);
    }
    else if (shaderType == 1u)
    {
        activePixelShaderHash_ = hash;
        activePixelShaderDwords_ = dwordCount;
    }
    else
    {
        return;
    }
    const auto* match = xerengeShaderCache().findMicrocode(hash);
    const bool moduleReady = match != nullptr && ensureShaderModule(match->shaderHash);
    if (std::getenv("XERENGE_XENOS_SHADER_TRACE") != nullptr)
    {
        std::cerr << "Xenos pointer shader stage=" << shaderType
                  << " guest=0x" << std::hex << guestAddress
                  << " dwords=" << std::dec << dwordCount
                  << " cache=" << (match != nullptr ? "hit" : "miss")
                  << " hash=0x" << std::hex << hash;
        if (match != nullptr)
        {
            std::cerr << " compiled=0x" << match->shaderHash;
            std::cerr << " module=" << (moduleReady ? "ready" : "missing");
            if (const auto* compiled = xerengeShaderCache().find(match->shaderHash))
            {
                size_t spirvWords = 0;
                const bool spirvValid =
                    xerengeShaderCache().spirv(*compiled, spirvWords) != nullptr;
                std::cerr << " spirv=" << (spirvValid ? "valid" : "invalid")
                          << '/' << spirvWords;
            }
        }
        std::cerr << std::dec << '\n';
    }
}

void XenosGpu::rasterizeDraw(uint8_t* guestBase, uint32_t initiator)
{
    // EDRAM copy is a draw in RB_MODECONTROL kCopy mode. Cache flush
    // events alone must not overwrite a resolved surface with later clears.
    if ((gpuRegisters_[0x2208] & 7u) == 6u)
    {
        resolveToGuest(guestBase);
        return;
    }

    // Burnout uses auto-indexed point draws during bootstrap and three-vertex
    // primitive-8 draws for the first render-target geometry. Copy packets
    // remain separate from this raster path.
    const uint32_t primitive = initiator & 0x3Fu;
    const uint32_t source = (initiator >> 6) & 0x3u;
    const uint32_t count = initiator >> 16;
    if ((primitive != 1u && primitive != 6u && primitive != 8u) ||
        source != 2u || count == 0 ||
        gpuRegisters_[0x2318] != 0)
        return;

    // Materialize the native pipeline as soon as both bound stages are known.
    // Draw submission remains on the bootstrap path until its resources and
    // render target have been uploaded below.
    ensureGraphicsPipeline(primitive == 1u
        ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
        : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);

    const uint32_t fetchRegister = 0x4800u + activeVertexFetchConstantIndex_ * 2u;
    const uint32_t fetch0 = vertexFetchRegisters_[fetchRegister - 0x4800u];
    const uint32_t fetch1 = vertexFetchRegisters_[fetchRegister - 0x4800u + 1u];
    if (std::getenv("XERENGE_XENOS_VERTEX_TRACE") != nullptr)
        std::cerr << "Xenos vertex-fetch slot=" << activeVertexFetchConstantIndex_
                  << " reg=0x" << std::hex << fetchRegister
                  << " words=" << fetch0 << ' ' << fetch1 << std::dec << '\n';
    if ((fetch0 & 0x3u) != 3u)
        return;

    const uint32_t physicalAddress = (fetch0 >> 2) << 2;
    const uint32_t vertexAddress = gpuPhysicalToGuest(physicalAddress);
    // The fetch constant's second word is the buffer size, not the vertex
    // stride.  Xenos encodes the stride in each vfetch instruction; the
    // bootstrap VS loaded by Burnout uses a seven-dword vertex and its
    // constant therefore remains zero in the register file.  The metadata is
    // populated when an immediate vertex shader is received below.
    uint32_t strideWords = vertexFetchStrideWords_[activeVertexFetchConstantIndex_];
    if (strideWords == 0u)
        strideWords = (fetch1 >> 2) & 0xFFFFFFu;
    const uint32_t minimumStride = primitive == 1u ? 3u : 2u;
    if (strideWords < minimumStride || strideWords > 0x1000)
        return;

    constexpr uint32_t width = 1280;
    constexpr uint32_t height = 720;
    if (edram_.size() != size_t(width) * height * 4)
        edram_.assign(size_t(width) * height * 4, 0);
    const uint32_t colorMask = (gpuRegisters_[0x2104u] & 0xFu) == 0u
        ? 0xFu : gpuRegisters_[0x2104u];
    auto writeColorMasked = [&](size_t pixel, const uint8_t* color)
    {
        for (uint32_t component = 0; component < 4; ++component)
            if ((colorMask & (1u << component)) != 0u)
                edram_[pixel + component] = color[component];
    };

    const uint32_t firstIndex = gpuRegisters_[0x2102] & 0x00FFFFFFu;
    const uint32_t drawVertices = primitive == 8u ? std::min(count, 3u) : count;
    struct RasterVertex
    {
        std::array<float, 3> position{};
        std::array<float, 2> uv{};
        std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
    };
    std::array<RasterVertex, 3> triangle{};
    std::vector<RasterVertex> stripVertices;
    if (primitive == 6u)
        stripVertices.reserve(count);
    std::vector<float> pointVertices;
    if (primitive == 1u)
        pointVertices.reserve(size_t(count) * 12u);
    std::array<uint8_t, 4> drawColor{255, 255, 255, 255};
    std::array<uint8_t, 4> constantColor{};
    bool havePixelConstant = false;
    for (uint32_t component = 0; component < 4; ++component)
    {
        const uint32_t raw = gpuRegisters_[0x4940u + component];
        float value = 0.0f;
        std::memcpy(&value, &raw, sizeof(value));
        if (std::isfinite(value) && std::abs(value) > 0.0001f)
            havePixelConstant = true;
        if (std::isfinite(value))
            constantColor[component] = static_cast<uint8_t>(
                std::clamp(value, 0.0f, 1.0f) * 255.0f);
    }
    if (havePixelConstant)
        drawColor = constantColor;
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t address = vertexAddress +
            (firstIndex + i) * strideWords * sizeof(uint32_t);
        uint32_t bits[2]{};
        for (uint32_t component = 0; component < 2; ++component)
            bits[component] = loadGuestBE(guestBase, address + component * 4);

        if ((primitive == 1u || primitive == 8u || primitive == 6u) &&
            std::getenv("XERENGE_XENOS_VERTEX_TRACE") != nullptr && i < 3)
        {
            std::cerr << "Xenos vertex i=" << i << " guest=0x" << std::hex << address
                      << " words=";
            for (uint32_t word = 0; word < std::min(strideWords, 7u); ++word)
                std::cerr << ' ' << loadGuestBE(guestBase, address + word * 4);
            std::cerr << std::dec << '\n';
        }

        float position[2];
        std::memcpy(&position[0], &bits[0], sizeof(position));
        if (!std::isfinite(position[0]) || !std::isfinite(position[1]))
            continue;

        // The first primitive-8 program writes viewport-space coordinates
        // directly: (-0.5,-0.5) .. (1279.5,719.5). Convert those samples to
        // NDC before applying the software viewport.
        const bool screenSpace = primitive == 8u || std::abs(position[0]) > 2.0f ||
            std::abs(position[1]) > 2.0f;
        const float xNdc = screenSpace
            ? ((position[0] + 0.5f) / width) * 2.0f - 1.0f
            : position[0];
        const float yNdc = screenSpace
            ? 1.0f - ((position[1] + 0.5f) / height) * 2.0f
            : position[1];
        if (primitive != 6u &&
            (xNdc < -1.0f || xNdc > 1.0f || yNdc < -1.0f || yNdc > 1.0f))
            continue;

        std::array<float, 2> uv{};
        // The pointer VS uses vfetch_full at offset 0, then vfetch_mini
        // offset 2 -> r0 (UV) and offset 4 -> r1 (the PS multiplier).
        for (uint32_t component = 0; component < 2; ++component)
        {
            const uint32_t raw = loadGuestBE(guestBase, address + (2 + component) * 4);
            std::memcpy(&uv[component], &raw, sizeof(float));
            if (!std::isfinite(uv[component]))
                uv[component] = 0.0f;
        }
        std::array<float, 4> vertexColor{1.0f, 1.0f, 1.0f, 1.0f};
        for (uint32_t component = 0; component < 4; ++component)
        {
            // The immediate pass-through pair exports its float4 at words
            // 3..6. The pointer texture pair uses words 4..7 for an 8-dword
            // vertex and c2 for the compact 4-dword layout.
            const uint32_t raw = activePixelShaderHash_ == 0x2E372EA28CC404B7ull
                ? loadGuestBE(guestBase, address + (3 + component) * 4)
                : strideWords >= 8u
                    ? loadGuestBE(guestBase, address + (4 + component) * 4)
                    : gpuRegisters_[0x4008u + component];
            std::memcpy(&vertexColor[component], &raw, sizeof(float));
            if (!std::isfinite(vertexColor[component]))
                vertexColor[component] = 1.0f;
        }
        if (primitive == 1u)
        {
            const size_t oldSize = pointVertices.size();
            pointVertices.resize(oldSize + 12u);
            float* destination = pointVertices.data() + oldSize;
            destination[0] = xNdc;
            destination[1] = yNdc;
            destination[2] = 0.0f;
            destination[3] = 1.0f;
            destination[4] = uv[0];
            destination[5] = uv[1];
            destination[6] = 0.0f;
            destination[7] = 0.0f;
            for (uint32_t component = 0; component < 4; ++component)
                destination[8u + component] = vertexColor[component];
            continue;
        }
        if (primitive == 8u && i < triangle.size())
            triangle[i] = {{xNdc, yNdc, 0.0f}, uv, vertexColor};
        if (primitive == 8u)
            continue;
        if (primitive == 6u)
        {
            // The captured pointer shaders fetch float2 position, float2 UV,
            // and optionally float4 colour. Keep position in guest space;
            // the recompiled VS applies the title's actual constant matrices.
            stripVertices.push_back(
                {{position[0], position[1], 0.0f}, uv, vertexColor});
            continue;
        }

        const uint32_t x = std::min(width - 1,
            static_cast<uint32_t>((xNdc * 0.5f + 0.5f) * width));
        const uint32_t y = std::min(height - 1,
            static_cast<uint32_t>((1.0f - (yNdc * 0.5f + 0.5f)) * height));
        const size_t pixel = (size_t(y) * width + x) * 4;
        // The bootstrap vertex program's second fetch is a float4
        // interpolator at words 3..6. The matching cached pixel program
        // forwards that interpolator to color 0, so preserve those actual
        // guest values instead of using a diagnostic white fragment.
        uint8_t color[4]{};
        for (uint32_t component = 0; component < 4; ++component)
        {
            const uint32_t raw = loadGuestBE(guestBase, address + (3 + component) * 4);
            float value = 0.0f;
            std::memcpy(&value, &raw, sizeof(value));
            if (std::isfinite(value))
                color[component] = static_cast<uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f);
        }
        writeColorMasked(pixel, color);
    }

    if (primitive == 1u && !pointVertices.empty())
    {
        // The bootstrap command stream emits one point per glyph/marker. A
        // synchronous Vulkan submit for every single point serializes the
        // guest and display threads and can take minutes before the frontend
        // advances. Keep real batches on the GPU; rasterize singleton point
        // packets locally until the point-sprite batcher is implemented.
        if (pointVertices.size() / 12u >= 32u &&
            drawVulkanGeometry(pointVertices.data(), pointVertices.size() / 12u,
                VK_PRIMITIVE_TOPOLOGY_POINT_LIST, nullptr, 1, 1, 0))
            return;

        for (size_t vertex = 0; vertex < pointVertices.size(); vertex += 12u)
        {
            const float xNdc = pointVertices[vertex + 0];
            const float yNdc = pointVertices[vertex + 1];
            const uint32_t x = std::min(width - 1,
                static_cast<uint32_t>(std::clamp(xNdc * 0.5f + 0.5f, 0.0f, 1.0f) * width));
            const uint32_t y = std::min(height - 1,
                static_cast<uint32_t>(std::clamp(1.0f - (yNdc * 0.5f + 0.5f), 0.0f, 1.0f) * height));
            uint8_t color[4]{};
            for (uint32_t component = 0; component < 4; ++component)
                color[component] = static_cast<uint8_t>(
                    std::clamp(pointVertices[vertex + 8u + component], 0.0f, 1.0f) * 255.0f);
            writeColorMasked((size_t(y) * width + x) * 4, color);
        }
        if (std::getenv("XERENGE_XENOS_DRAW_TRACE") != nullptr)
            std::cerr << "Xenos point draw rasterized count="
                      << pointVertices.size() / 12u << '\n';
        return;
    }

    if ((primitive == 8u && drawVertices == 3u) ||
        (primitive == 6u && stripVertices.size() >= 3u))
    {
        if (primitive == 6u)
            std::copy_n(stripVertices.begin(), 3, triangle.begin());
        // Captured RectangleList draws supply three consecutive corners:
        // top-left, top-right, bottom-right. Complete the parallelogram
        // across the v0-v2 diagonal, including its fourth corner in bounds.
        const float fourthX = triangle[0].position[0] + triangle[2].position[0] -
            triangle[1].position[0];
        const float fourthY = triangle[0].position[1] + triangle[2].position[1] -
            triangle[1].position[1];
        const float minX = std::min({triangle[0].position[0], triangle[1].position[0],
            triangle[2].position[0], fourthX});
        const float maxX = std::max({triangle[0].position[0], triangle[1].position[0],
            triangle[2].position[0], fourthX});
        const float minY = std::min({triangle[0].position[1], triangle[1].position[1],
            triangle[2].position[1], fourthY});
        const float maxY = std::max({triangle[0].position[1], triangle[1].position[1],
            triangle[2].position[1], fourthY});
        const int left = std::max(0, static_cast<int>((minX * 0.5f + 0.5f) * width));
        const int right = std::min(static_cast<int>(width) - 1,
            static_cast<int>((maxX * 0.5f + 0.5f) * width));
        const int top = std::max(0, static_cast<int>((1.0f - (maxY * 0.5f + 0.5f)) * height));
        const int bottom = std::min(static_cast<int>(height) - 1,
            static_cast<int>((1.0f - (minY * 0.5f + 0.5f)) * height));
        const uint32_t texture0 = gpuRegisters_[0x4800u];
        const uint32_t texture1 = gpuRegisters_[0x4801u];
        const uint32_t texture2 = gpuRegisters_[0x4802u];
        const uint32_t textureFormat = texture1 & 0x3Fu;
        const uint32_t textureEndian = (texture1 >> 6) & 0x3u;
        const bool textureIsValid = (texture0 & 0x3u) == 2u;
        const bool hasDxt1Texture = textureIsValid && textureFormat == 13u;
        const bool hasDxt3Texture = textureIsValid && textureFormat == 19u;
        const bool hasRgba8Texture = textureIsValid && textureFormat == 6u;
        if (std::getenv("XERENGE_XENOS_TEXTURE_TRACE_ALL") != nullptr)
            std::cerr << "Xenos tf0 raw=0x" << std::hex << texture0 << ' ' << texture1
                      << ' ' << texture2 << " ps=0x" << activePixelShaderHash_
                      << std::dec << '\n';
        const bool solidVertexColor = !hasDxt1Texture && !hasDxt3Texture && !hasRgba8Texture &&
            !havePixelConstant && activePixelShaderHash_ == 0x2E372EA28CC404B7ull;
        if (solidVertexColor)
        {
            for (uint32_t component = 0; component < 4; ++component)
                drawColor[component] = static_cast<uint8_t>(
                    std::clamp(triangle[0].color[component], 0.0f, 1.0f) * 255.0f);
        }
        if (primitive != 6u && !hasDxt1Texture && !hasDxt3Texture && !hasRgba8Texture &&
            !havePixelConstant && !solidVertexColor)
        {
            if (std::getenv("XERENGE_XENOS_DRAW_TRACE") != nullptr)
                std::cerr << "Xenos rectangle skipped: unsupported pixel resource\n";
            return;
        }
        const uint32_t textureWidth = (texture2 & 0x1FFFu) + 1u;
        const uint32_t textureHeight = ((texture2 >> 13) & 0x1FFFu) + 1u;
        // The fetch constant stores pitch in 32-pixel units.  It is the
        // storage pitch, not the visible width; using width here reads the
        // wrong macro tile for small textures and becomes especially visible
        // with 1x1 UI resources.
        const uint32_t texturePitchPixels = std::max(32u,
            ((texture0 >> 22) & 0x1FFu) << 5);
        if ((hasDxt1Texture || hasDxt3Texture || hasRgba8Texture) &&
            std::getenv("XERENGE_XENOS_TEXTURE_TRACE") != nullptr)
            std::cerr << "Xenos texture tf0 base=0x" << std::hex
                      << ((texture1 >> 12) & 0xFFFFFu)
                      << " format=" << (texture1 & 0x3Fu)
                      << " size=" << std::dec << textureWidth << 'x' << textureHeight
                      << " pitch=" << ((texture0 >> 22) & 0x1FFu) << '\n';
        bool alphaTest = false;
        bool pixelShaderPassesInterpolator = false;
        float alphaThreshold = 0.0f;
        if (const auto* microcode = xerengeShaderCache().findMicrocode(
                activePixelShaderHash_))
        {
            if (const auto* shader = xerengeShaderCache().find(microcode->shaderHash))
            {
                alphaTest = (shader->specConstantsMask & (1u << 1)) != 0u;
                // This bootstrap PS has no texture operation in its generated
                // SPIR-V; it forwards location 0 after the Xenos max opcode.
                // Treating any stale tf0 as an implicit sample replaced clear
                // and solid-colour rectangles with unrelated texture data.
                pixelShaderPassesInterpolator =
                    microcode->shaderHash == 0xBF9D261289F75F76ull;
            }
        }
        if (alphaTest)
        {
            const uint32_t sharedBase = gpuPhysicalToGuest(gpuRegisters_[0x2308u]);
            const uint32_t raw = loadGuestBE(guestBase, sharedBase + 308u);
            std::memcpy(&alphaThreshold, &raw, sizeof(alphaThreshold));
            if (!std::isfinite(alphaThreshold))
                alphaThreshold = 0.0f;
            if (std::getenv("XERENGE_XENOS_TEXTURE_TRACE") != nullptr)
                std::cerr << "Xenos alpha-test threshold=" << alphaThreshold
                          << " shared=0x" << std::hex << sharedBase << std::dec << '\n';
        }
        const uint32_t texturePitchBlocks = std::max(8u,
            ((texturePitchPixels + 3u) / 4u + 7u) & ~7u);
        const uint32_t textureBase = gpuPhysicalToGuest(
            ((texture1 >> 12) & 0xFFFFFu) << 12);
        auto textureAddress = [&](uint32_t x, uint32_t y,
            uint32_t pitchAligned, uint32_t bytesPerBlockLog2)
        {
            const uint32_t outerBlocks =
                ((y >> 5) * (pitchAligned >> 5) + (x >> 5)) << 6;
            const uint32_t innerBlocks = (((y >> 1) & 7u) << 3) | (x & 7u);
            const uint32_t outerInnerBytes = (outerBlocks | innerBlocks) << bytesPerBlockLog2;
            const uint32_t bank = (y >> 4) & 1u;
            const uint32_t pipe = ((x >> 3) & 3u) ^ (((y >> 3) & 1u) << 1);
            return (textureBase + ((y & 1u) << 4) + (pipe << 6) +
                (bank << 11) + (outerInnerBytes & 0xFu) +
                (((outerInnerBytes >> 4) & 1u) << 5) +
                (((outerInnerBytes >> 5) & 7u) << 8) +
                ((outerInnerBytes >> 8) << 12));
        };
        auto sampleTexture = [&](float u, float v)
        {
            std::array<uint8_t, 4> result{255, 255, 255, 255};
            if ((!hasDxt1Texture && !hasDxt3Texture && !hasRgba8Texture) ||
                textureWidth == 0 || textureHeight == 0)
                return result;
            u = std::clamp(u, 0.0f, 1.0f);
            v = std::clamp(v, 0.0f, 1.0f);
            const uint32_t x = std::min(textureWidth - 1,
                static_cast<uint32_t>(u * textureWidth));
            const uint32_t y = std::min(textureHeight - 1,
                static_cast<uint32_t>(v * textureHeight));
            const bool blockCompressed = hasDxt1Texture || hasDxt3Texture;
            const uint32_t blockX = blockCompressed ? x / 4u : x;
            const uint32_t blockY = blockCompressed ? y / 4u : y;
            const uint32_t address = textureAddress(blockX, blockY,
                blockCompressed ? texturePitchBlocks : texturePitchPixels,
                hasDxt1Texture ? 3u : hasDxt3Texture ? 4u : 2u);
            if (hasRgba8Texture)
            {
                uint8_t texel[4] = {guestBase[address + 0], guestBase[address + 1],
                    guestBase[address + 2], guestBase[address + 3]};
                static bool loggedRgba8Sample = false;
                if (!loggedRgba8Sample && std::getenv("XERENGE_XENOS_TEXTURE_BYTES") != nullptr)
                {
                    loggedRgba8Sample = true;
                    std::cerr << "Xenos RGBA8 sample guest=0x" << std::hex << address
                              << " raw=" << unsigned(texel[0]) << ' ' << unsigned(texel[1])
                              << ' ' << unsigned(texel[2]) << ' ' << unsigned(texel[3])
                              << " endian=" << textureEndian << std::dec << '\n';
                }
                if (textureEndian == 1u)
                    std::swap(texel[0], texel[1]);
                else if (textureEndian == 2u)
                    std::swap(texel[0], texel[3]), std::swap(texel[1], texel[2]);
                else if (textureEndian == 3u)
                    std::swap(texel[0], texel[2]), std::swap(texel[1], texel[3]);
                std::copy(std::begin(texel), std::end(texel), result.begin());
                return result;
            }
            uint8_t block[16]{};
            const uint32_t blockBytes = hasDxt1Texture ? 8u : 16u;
            for (uint32_t i = 0; i < blockBytes; ++i)
                block[i] = guestBase[address + i];
            if (textureEndian == 1u)
            {
                for (uint32_t i = 0; i < blockBytes; i += 2)
                    std::swap(block[i], block[i + 1]);
            }
            else if (textureEndian == 2u)
            {
                for (uint32_t i = 0; i < blockBytes; i += 4)
                    std::swap(block[i], block[i + 3]), std::swap(block[i + 1], block[i + 2]);
            }
            else if (textureEndian == 3u)
            {
                for (uint32_t i = 0; i < blockBytes; i += 4)
                    std::swap(block[i], block[i + 2]), std::swap(block[i + 1], block[i + 3]);
            }
            const uint32_t local = (y & 3u) * 4u + (x & 3u);
            if (hasDxt1Texture)
                result[3] = 255u;
            else
            {
                const uint8_t alphaByte = block[local >> 1];
                result[3] = static_cast<uint8_t>(
                    ((local & 1u) ? alphaByte >> 4 : alphaByte & 0xFu) * 17u);
            }
            const uint32_t colorOffset = hasDxt1Texture ? 0u : 8u;
            const uint16_t c0 = uint16_t(block[colorOffset]) |
                (uint16_t(block[colorOffset + 1]) << 8);
            const uint16_t c1 = uint16_t(block[colorOffset + 2]) |
                (uint16_t(block[colorOffset + 3]) << 8);
            auto expand = [](uint16_t c, uint32_t shift, uint32_t bits)
            { return (c >> shift) & ((1u << bits) - 1u); };
            uint8_t colors[4][3]{};
            for (uint32_t c = 0; c < 2; ++c)
            {
                const uint16_t value = c ? c1 : c0;
                colors[c][0] = static_cast<uint8_t>(expand(value, 11, 5) * 255 / 31);
                colors[c][1] = static_cast<uint8_t>(expand(value, 5, 6) * 255 / 63);
                colors[c][2] = static_cast<uint8_t>(expand(value, 0, 5) * 255 / 31);
            }
            if (hasDxt1Texture && c0 <= c1)
            {
                for (uint32_t c = 0; c < 3; ++c)
                    colors[2][c] = static_cast<uint8_t>((colors[0][c] + colors[1][c]) / 2u);
                std::fill(std::begin(colors[3]), std::end(colors[3]), 0u);
                result[3] = 0u;
            }
            else
            {
                for (uint32_t c = 0; c < 3; ++c)
                    colors[2][c] = static_cast<uint8_t>((2u * colors[0][c] + colors[1][c]) / 3u);
                for (uint32_t c = 0; c < 3; ++c)
                    colors[3][c] = static_cast<uint8_t>((colors[0][c] + 2u * colors[1][c]) / 3u);
            }
            const uint32_t indexOffset = hasDxt1Texture ? 4u : 12u;
            const uint32_t colorIndex = (uint32_t(block[indexOffset + (local >> 2)]) >>
                ((local & 3u) * 2u)) & 3u;
            for (uint32_t c = 0; c < 3; ++c)
                result[c] = colors[colorIndex][c];
            return result;
        };
        auto fillTriangle = [&](const RasterVertex& va, const RasterVertex& vb,
            const RasterVertex& vc)
        {
            // The pointer vertex programs receive viewport coordinates for
            // the UI strip, while the software rasterizer evaluates samples
            // in NDC. Keep the original values in RasterVertex for the Vulkan
            // path and normalize only this fallback calculation.
            const auto toNdc = [](float x, float y)
            {
                if (std::abs(x) <= 2.0f && std::abs(y) <= 2.0f)
                    return std::array<float, 2>{x, y};
                return std::array<float, 2>{
                    ((x + 0.5f) / width) * 2.0f - 1.0f,
                    1.0f - ((y + 0.5f) / height) * 2.0f};
            };
            const auto a = toNdc(va.position[0], va.position[1]);
            const auto b = toNdc(vb.position[0], vb.position[1]);
            const auto c = toNdc(vc.position[0], vc.position[1]);
            const float ax = a[0], ay = a[1];
            const float bx = b[0], by = b[1];
            const float cx = c[0], cy = c[1];
            const float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
            if (area == 0.0f)
                return;
            const float localMinX = std::min({ax, bx, cx});
            const float localMaxX = std::max({ax, bx, cx});
            const float localMinY = std::min({ay, by, cy});
            const float localMaxY = std::max({ay, by, cy});
            const int localLeft = std::max(0,
                static_cast<int>((localMinX * 0.5f + 0.5f) * width));
            const int localRight = std::min(static_cast<int>(width) - 1,
                static_cast<int>((localMaxX * 0.5f + 0.5f) * width));
            const int localTop = std::max(0,
                static_cast<int>((1.0f - (localMaxY * 0.5f + 0.5f)) * height));
            const int localBottom = std::min(static_cast<int>(height) - 1,
                static_cast<int>((1.0f - (localMinY * 0.5f + 0.5f)) * height));
            for (int y = localTop; y <= localBottom; ++y)
                for (int x = localLeft; x <= localRight; ++x)
                {
                    const float px = (static_cast<float>(x) + 0.5f) / width * 2.0f - 1.0f;
                    const float py = 1.0f - (static_cast<float>(y) + 0.5f) / height * 2.0f;
                    const float w0 = ((bx - px) * (cy - py) - (by - py) * (cx - px)) / area;
                    const float w1 = ((cx - px) * (ay - py) - (cy - py) * (ax - px)) / area;
                    const float w2 = 1.0f - w0 - w1;
                    if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f)
                    {
                        const float u = w0 * va.uv[0] + w1 * vb.uv[0] + w2 * vc.uv[0];
                        const float v = w0 * va.uv[1] + w1 * vb.uv[1] + w2 * vc.uv[1];
                        const bool hasSampledTexture = !pixelShaderPassesInterpolator &&
                            (hasDxt1Texture || hasDxt3Texture || hasRgba8Texture);
                        std::array<uint8_t, 4> output = hasSampledTexture
                            ? sampleTexture(u, v) : drawColor;
                        if (hasSampledTexture || pixelShaderPassesInterpolator)
                        {
                            for (uint32_t component = 0; component < 4; ++component)
                            {
                                const float factor = w0 * va.color[component] +
                                    w1 * vb.color[component] + w2 * vc.color[component];
                                const float value = hasSampledTexture
                                    ? (output[component] / 255.0f) * factor
                                    : factor;
                                output[component] = static_cast<uint8_t>(
                                    std::clamp(value, 0.0f, 1.0f) * 255.0f);
                            }
                        }
                        if (alphaTest && output[3] / 255.0f < alphaThreshold)
                            continue;
                        writeColorMasked((size_t(y) * width + x) * 4, output.data());
                    }
                }
        };
        const auto& v0 = triangle[0];
        const auto& v1 = triangle[1];
        const auto& v2 = triangle[2];
        // Xenos RectangleList supplies three corners; infer the fourth corner
        // from the parallelogram relation before filling the second triangle.
        RasterVertex v3 = v1;
        v3.position[0] = v0.position[0] + v2.position[0] - v1.position[0];
        v3.position[1] = v0.position[1] + v2.position[1] - v1.position[1];
        v3.uv[0] = v0.uv[0] + v2.uv[0] - v1.uv[0];
        v3.uv[1] = v0.uv[1] + v2.uv[1] - v1.uv[1];
        for (uint32_t component = 0; component < 4; ++component)
            v3.color[component] = v0.color[component] + v2.color[component] -
                v1.color[component];
        std::vector<const RasterVertex*> nativeOrder;
        if (primitive == 8u)
            nativeOrder = {&v0, &v1, &v2, &v0, &v2, &v3};
        else
            for (size_t vertex = 2; vertex < stripVertices.size(); ++vertex)
            {
                const RasterVertex* a = &stripVertices[vertex - 2];
                const RasterVertex* b = &stripVertices[vertex - 1];
                const RasterVertex* c = &stripVertices[vertex];
                if (vertex & 1u)
                    std::swap(a, b);
                nativeOrder.insert(nativeOrder.end(), {a, b, c});
            }
        std::vector<float> nativeVertices(nativeOrder.size() * 12u);
        for (uint32_t vertex = 0; vertex < nativeOrder.size(); ++vertex)
        {
            float* destination = nativeVertices.data() + vertex * 12;
            destination[0] = nativeOrder[vertex]->position[0];
            destination[1] = nativeOrder[vertex]->position[1];
            destination[2] = nativeOrder[vertex]->position[2];
            destination[3] = 1.0f;
            destination[4] = nativeOrder[vertex]->uv[0];
            destination[5] = nativeOrder[vertex]->uv[1];
            for (uint32_t component = 0; component < 4; ++component)
                destination[8 + component] = nativeOrder[vertex]->color[component];
        }
        // Convert the tiled guest resource to tightly packed RGBA8.  Hashing
        // the decoded contents keeps unchanged images resident while still
        // detecting guest writes through an unchanged fetch descriptor.
        constexpr size_t textureUploadCapacity = 16u * 1024u * 1024u;
        std::vector<uint8_t> nativeTexture;
        uint64_t nativeTextureKey = 0;
        const bool hasNativeTexture = hasDxt3Texture || hasRgba8Texture;
        const size_t nativeTextureBytes = size_t(textureWidth) * textureHeight * 4;
        if (hasNativeTexture && textureWidth != 0 && textureHeight != 0 &&
            nativeTextureBytes <= textureUploadCapacity)
        {
            nativeTexture.resize(nativeTextureBytes);
            for (uint32_t y = 0; y < textureHeight; ++y)
                for (uint32_t x = 0; x < textureWidth; ++x)
                {
                    const auto texel = sampleTexture(
                        (static_cast<float>(x) + 0.5f) / textureWidth,
                        (static_cast<float>(y) + 0.5f) / textureHeight);
                    std::copy(texel.begin(), texel.end(),
                        nativeTexture.begin() +
                            (size_t(y) * textureWidth + x) * 4);
                }
            // Guest code may rewrite a texture without changing its fetch
            // descriptor.  Key the upload by decoded contents so video and
            // dynamic UI resources cannot become permanently stale.
            nativeTextureKey = XXH3_64bits(
                nativeTexture.data(), nativeTexture.size());
            nativeTextureKey ^= uint64_t(textureWidth) << 32;
            nativeTextureKey ^= uint64_t(textureHeight);
            if (nativeTextureKey == 0)
                nativeTextureKey = 1;
            if (nativeTextureKey != vulkanTextureKey_ &&
                std::getenv("XERENGE_XENOS_TEXTURE_STATS") != nullptr)
            {
                static std::atomic<uint32_t> textureStatsCount = 0;
                if (textureStatsCount.fetch_add(1, std::memory_order_relaxed) < 32)
                {
                    size_t transparent = 0, opaque = 0, partial = 0;
                    for (size_t texel = 0; texel < nativeTexture.size(); texel += 4)
                    {
                        transparent += nativeTexture[texel + 3] == 0;
                        opaque += nativeTexture[texel + 3] == 255;
                        partial += nativeTexture[texel + 3] != 0 &&
                            nativeTexture[texel + 3] != 255;
                    }
                    float minU = nativeVertices[4], maxU = nativeVertices[4];
                    float minV = nativeVertices[5], maxV = nativeVertices[5];
                    for (size_t vertex = 1; vertex < nativeOrder.size(); ++vertex)
                    {
                        minU = std::min(minU, nativeVertices[vertex * 12 + 4]);
                        maxU = std::max(maxU, nativeVertices[vertex * 12 + 4]);
                        minV = std::min(minV, nativeVertices[vertex * 12 + 5]);
                        maxV = std::max(maxV, nativeVertices[vertex * 12 + 5]);
                    }
                    std::cerr << "Xenos texture stats size=" << textureWidth << 'x'
                              << textureHeight << " format=" << textureFormat
                              << " alpha=" << transparent << '/' << partial << '/'
                              << opaque << " uv=" << minU << ',' << minV << ".."
                              << maxU << ',' << maxV << '\n';
                }
            }
        }
        if (drawVulkanGeometry(nativeVertices.data(), nativeOrder.size(),
                VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                hasNativeTexture && nativeTextureKey != vulkanTextureKey_
                    ? nativeTexture.data() : nullptr,
                textureWidth, textureHeight, nativeTextureKey))
            return;
        if (primitive == 6u)
        {
            for (size_t vertex = 2; vertex < stripVertices.size(); ++vertex)
            {
                const RasterVertex* a = &stripVertices[vertex - 2];
                const RasterVertex* b = &stripVertices[vertex - 1];
                const RasterVertex* c = &stripVertices[vertex];
                if (vertex & 1u)
                    std::swap(a, b);
                fillTriangle(*a, *b, *c);
            }
        }
        else
        {
            fillTriangle(v0, v1, v2);
            fillTriangle(v0, v2, v3);
        }
        if (std::getenv("XERENGE_XENOS_DRAW_TRACE") != nullptr)
            std::cerr << "Xenos triangle rasterized v0=" << triangle[0].position[0] << ','
                      << triangle[0].position[1] << " v1=" << triangle[1].position[0] << ','
                      << triangle[1].position[1] << " v2=" << triangle[2].position[0] << ','
                      << triangle[2].position[1] << " pixelConstant="
                      << (havePixelConstant ? "yes" : "fallback") << '\n';
    }
    else if (primitive == 6u && std::getenv("XERENGE_XENOS_VERTEX_TRACE") != nullptr)
    {
        std::cerr << "Xenos triangle strip rejected vertices="
                  << stripVertices.size() << '/' << count
                  << " stride=" << strideWords
                  << " fetchSlot=" << activeVertexFetchConstantIndex_ << '\n';
    }
}

void XenosGpu::resolveToGuest(uint8_t* guestBase)
{
    readbackVulkanFrame();
    const uint32_t physicalDestination = gpuRegisters_[0x2319];
    if (physicalDestination == 0)
        return;

    if (edram_.empty())
        edram_.resize(kEdramSize, 0);

    // RB_COPY_DEST_BASE is a 29-bit physical address. The runtime maps the
    // title's physical allocations through the 0x60000000 guest alias.
    const uint32_t destination =
        0x60000000u | (physicalDestination & 0x1FFFFFFFu);
    constexpr uint32_t width = 1280;
    constexpr uint32_t height = 720;
    constexpr size_t byteCount = size_t(width) * height * 4;
    if (destination > 0xFFFFFFFFu - byteCount)
        return;

    // The linear path is deliberately limited to the portion of EDRAM that
    // exists in this bring-up. Once the software rasterizer writes tiled
    // samples, this is replaced by the Xenos tile resolve routine.
    const size_t copyCount = std::min(byteCount, edram_.size());
    std::memcpy(guestBase + destination, edram_.data(), copyCount);
    ++resolveCount_;
    if (std::getenv("XERENGE_XENOS_RESOLVE_TRACE") != nullptr)
    {
        size_t nonzeroPixels = 0;
        size_t nonzeroAlphaPixels = 0;
        size_t rgbWithZeroAlphaPixels = 0;
        size_t whiteRgbPixels = 0;
        size_t topHalfRgbPixels = 0;
        size_t bottomHalfRgbPixels = 0;
        uint32_t firstNonzeroPixel = 0;
        uint32_t firstPixel = 0;
        uint32_t differentPixels = 0;
        for (size_t i = 0; i + 3 < copyCount; i += 4)
        {
            nonzeroPixels += (edram_[i] | edram_[i + 1] | edram_[i + 2]) != 0;
            if ((edram_[i] | edram_[i + 1] | edram_[i + 2]) != 0)
            {
                const size_t y = (i / 4) / width;
                (y < height / 2 ? topHalfRgbPixels : bottomHalfRgbPixels)++;
            }
            nonzeroAlphaPixels += edram_[i + 3] != 0;
            rgbWithZeroAlphaPixels +=
                (edram_[i] | edram_[i + 1] | edram_[i + 2]) != 0 && edram_[i + 3] == 0;
            whiteRgbPixels += edram_[i] == 255 && edram_[i + 1] == 255 &&
                edram_[i + 2] == 255;
            const uint32_t pixel = uint32_t(edram_[i]) | (uint32_t(edram_[i + 1]) << 8) |
                (uint32_t(edram_[i + 2]) << 16) | (uint32_t(edram_[i + 3]) << 24);
            if (firstNonzeroPixel == 0 && (pixel & 0x00FFFFFFu) != 0)
                firstNonzeroPixel = pixel;
            if (i == 0)
                firstPixel = pixel;
            differentPixels += pixel != firstPixel;
        }
        std::cerr << "Xenos resolve destination=0x" << std::hex << destination
                  << " bytes=" << std::dec << copyCount
                  << " nonzeroRgbPixels=" << nonzeroPixels
                  << " nonzeroAlphaPixels=" << nonzeroAlphaPixels
                  << " rgbWithZeroAlphaPixels=" << rgbWithZeroAlphaPixels
                  << " whiteRgbPixels=" << whiteRgbPixels
                  << " topHalfRgbPixels=" << topHalfRgbPixels
                  << " bottomHalfRgbPixels=" << bottomHalfRgbPixels
                  << " firstNonzeroPixel=0x" << std::hex << firstNonzeroPixel << std::dec
                  << " differentFromFirst=" << differentPixels
                  << " firstPixel=0x" << std::hex << firstPixel << std::dec << '\n';
    }
}

void XenosGpu::processSubmittedBuffer(uint8_t* guestBase, uint32_t guestAddress, uint32_t dwordCount)
{
    std::lock_guard lock(mutex_);
    processBuffer(guestBase, guestAddress, dwordCount, 0);
}

bool XenosGpu::takeInterruptPending()
{
    std::lock_guard lock(mutex_);
    const bool pending = interruptPending_;
    interruptPending_ = false;
    return pending;
}

void XenosGpu::processBuffer(uint8_t* guestBase, uint32_t guestAddress,
    uint32_t dwordCount, uint32_t recursionDepth)
{
    if (recursionDepth > 8 || dwordCount > (1u << 22))
        return;

    uint32_t offset = 0;
    while (offset < dwordCount)
    {
        const uint32_t packet = loadGuestBE(guestBase, guestAddress + offset * 4);
        if (packet == 0)
        {
            ++offset;
            continue;
        }
        const uint32_t type = packet >> 30;
        uint32_t length = 1;
        if (type == 0)
        {
            ++type0Count_;
            length = ((packet >> 16) & 0x3FFFu) + 2;
            const uint32_t baseRegister = packet & 0x7FFFu;
            const bool writeOne = (packet & 0x8000u) != 0;
            for (uint32_t i = 0; i + 1 < length; ++i)
            {
                const uint32_t index = writeOne ? baseRegister : baseRegister + i;
                if (index < gpuRegisters_.size())
                    writeGpuRegister(index, loadGuestBE(
                        guestBase, guestAddress + (offset + 1 + i) * 4));
            }
        }
        else if (type == 3)
        {
            ++type3Count_;
            length = ((packet >> 16) & 0x3FFFu) + 2;
            const uint32_t opcode = (packet >> 8) & 0xFFu;
            ++opcodeCounts_[opcode];
            if (opcode == 0x2Du || opcode == 0x55u || opcode == 0x56u)
            {
                const uint32_t payloadCount = length - 1;
                if (payloadCount != 0 && offset + payloadCount < dwordCount)
                {
                    const uint32_t offsetType = loadGuestBE(
                        guestBase, guestAddress + (offset + 1) * 4);
                    uint32_t index = offsetType & (opcode == 0x2Du ? 0x7FFu : 0xFFFFu);
                    if (opcode == 0x2Du)
                    {
                        switch ((offsetType >> 16) & 0xFFu)
                        {
                        case 0: index += 0x4000; break;
                        case 1: index += 0x4800; break;
                        case 2: index += 0x4900; break;
                        case 3: index += 0x4908; break;
                        case 4: index += 0x2000; break;
                        default: index = 0xFFFFFFFFu; break;
                        }
                    }
                    if (index != 0xFFFFFFFFu)
                        for (uint32_t i = 1; i < payloadCount; ++i)
                            writeGpuRegister(index + i - 1,
                                loadGuestBE(guestBase, guestAddress + (offset + 1 + i) * 4));
                }
            }
            if (opcode == 0x2Fu && length >= 4 && offset + 3 < dwordCount)
            {
                // PM4_LOAD_ALU_CONSTANT: physical source address, typed
                // destination register, and dword count.
                const uint32_t physicalAddress = loadGuestBE(
                    guestBase, guestAddress + (offset + 1) * 4) & 0x3FFFFFFFu;
                const uint32_t offsetType = loadGuestBE(
                    guestBase, guestAddress + (offset + 2) * 4);
                const uint32_t sizeDwords = loadGuestBE(
                    guestBase, guestAddress + (offset + 3) * 4) & 0xFFFu;
                uint32_t index = offsetType & 0x7FFu;
                switch ((offsetType >> 16) & 0xFFu)
                {
                case 0: index += 0x4000; break;
                case 1: index += 0x4800; break;
                case 2: index += 0x4900; break;
                case 3: index += 0x4908; break;
                case 4: index += 0x2000; break;
                default: index = 0xFFFFFFFFu; break;
                }
                const uint32_t source = gpuPhysicalToGuest(physicalAddress);
                if (index != 0xFFFFFFFFu && sizeDwords <= 0x8000u - index)
                    for (uint32_t i = 0; i < sizeDwords; ++i)
                        writeGpuRegister(index + i,
                        loadGuestBE(guestBase, source + i * 4));
            }

            if (opcode == 0x21u && length >= 4 && offset + 3 < dwordCount)
            {
                const uint32_t info = loadGuestBE(guestBase,
                    guestAddress + (offset + 1) * 4);
                const uint32_t andOperand = loadGuestBE(guestBase,
                    guestAddress + (offset + 2) * 4);
                const uint32_t orOperand = loadGuestBE(guestBase,
                    guestAddress + (offset + 3) * 4);
                const uint32_t index = info & 0x1FFFu;
                if (index < gpuRegisters_.size())
                {
                    uint32_t value = gpuRegisters_[index];
                    value &= (info & 0x80000000u) != 0
                        ? gpuRegisters_[andOperand & 0x1FFFu] : andOperand;
                    value |= (info & 0x40000000u) != 0
                        ? gpuRegisters_[orOperand & 0x1FFFu] : orOperand;
                    writeGpuRegister(index, value);
                }
            }

            if (opcode == 0x3Du && length >= 3 && offset + 2 < dwordCount)
            {
                uint32_t address = loadGuestBE(guestBase,
                    guestAddress + (offset + 1) * 4);
                for (uint32_t i = 0; i + 2 < length; ++i)
                {
                    writeGpuMemory(guestBase, address,
                        loadGuestBE(guestBase,
                            guestAddress + (offset + 2 + i) * 4));
                    address += 4;
                }
            }

            if (opcode == 0x3Cu && length >= 6 && offset + 5 < dwordCount)
            {
                const uint32_t waitInfo = loadGuestBE(guestBase,
                    guestAddress + (offset + 1) * 4);
                const uint32_t pollAddress = loadGuestBE(guestBase,
                    guestAddress + (offset + 2) * 4);
                const uint32_t reference = loadGuestBE(guestBase,
                    guestAddress + (offset + 3) * 4);
                const uint32_t mask = loadGuestBE(guestBase,
                    guestAddress + (offset + 4) * 4);
                const uint32_t value = (waitInfo & 0x10u) != 0
                    ? loadGuestBE(guestBase, gpuPhysicalToGuest(pollAddress & ~3u))
                    : (pollAddress < gpuRegisters_.size() ? gpuRegisters_[pollAddress] : 0);
                const uint32_t relation = waitInfo & 7u;
                const uint32_t masked = value & mask;
                const bool matched = relation == 0u ? false :
                    relation == 1u ? masked < reference :
                    relation == 2u ? masked <= reference :
                    relation == 3u ? masked == reference :
                    relation == 4u ? masked != reference :
                    relation == 5u ? masked >= reference :
                    relation == 6u ? masked > reference : true;
                if (!matched && std::getenv("XERENGE_XENOS_WAIT_TRACE") != nullptr)
                    std::cerr << "Xenos WAIT_REG_MEM unsatisfied address=0x"
                              << std::hex << pollAddress << " value=0x" << value
                              << " ref=0x" << reference << " mask=0x" << mask
                              << std::dec << '\n';
            }
            if (opcode == 0x2Bu && length >= 3 && offset + 2 < dwordCount &&
                std::getenv("XERENGE_XENOS_SHADER_TRACE") != nullptr)
            {
                const uint32_t shaderType = loadGuestBE(
                    guestBase, guestAddress + (offset + 1) * 4);
                const uint32_t startSize = loadGuestBE(
                    guestBase, guestAddress + (offset + 2) * 4);
                std::cerr << "Xenos immediate shader type=" << shaderType
                          << " dwords=" << (startSize & 0xFFFFu) << '\n';
            }
            if (opcode == 0x2Bu && length >= 3 && offset + 2 < dwordCount)
            {
                const uint32_t shaderType = loadGuestBE(
                    guestBase, guestAddress + (offset + 1) * 4);
                const uint32_t codeDwords = loadGuestBE(
                    guestBase, guestAddress + (offset + 2) * 4) & 0xFFFFu;
                if (codeDwords != 0 && codeDwords <= length - 3)
                {
                    if (std::getenv("XERENGE_XENOS_SHADER_DUMP") != nullptr)
                    {
                        std::cerr << "Xenos shader code stage=" << shaderType
                                  << " dwords=" << codeDwords << ':';
                        for (uint32_t i = 0; i < codeDwords; ++i)
                            std::cerr << " " << std::hex << loadGuestBE(
                                guestBase, guestAddress + (offset + 3 + i) * 4);
                        std::cerr << std::dec << '\n';
                    }
                    if (shaderType == 0u)
                    {
                        std::array<uint32_t, 1024> code{};
                        const uint32_t copied = std::min<uint32_t>(codeDwords, code.size());
                        for (uint32_t i = 0; i < copied; ++i)
                            code[i] = loadGuestBE(
                                guestBase, guestAddress + (offset + 3 + i) * 4);
                        rememberVertexFetchStrides(code.data(), copied);
                    }
                    const size_t byteSize = size_t(codeDwords) * sizeof(uint32_t);
                    const uint64_t hash = XXH3_64bits(
                        guestBase + guestAddress + (offset + 3) * 4, byteSize);
                    if (const char* captureDirectory =
                            std::getenv("XERENGE_XENOS_SHADER_CAPTURE_DIR"))
                    {
                        std::error_code error;
                        std::filesystem::create_directories(captureDirectory, error);
                        const auto path = std::filesystem::path(captureDirectory) /
                            (std::to_string(hash) +
                                (shaderType == 0u ? ".vs.bin" : ".ps.bin"));
                        if (!error && !std::filesystem::exists(path))
                        {
                            std::ofstream stream(path, std::ios::binary);
                            stream.write(reinterpret_cast<const char*>(guestBase +
                                guestAddress + (offset + 3) * 4), byteSize);
                        }
                    }
                    if (shaderType == 0u)
                    {
                        activeVertexShaderHash_ = hash;
                        activeVertexShaderDwords_ = codeDwords;
                    }
                    else if (shaderType == 1u)
                    {
                        activePixelShaderHash_ = hash;
                        activePixelShaderDwords_ = codeDwords;
                    }
                    const auto cache = xerengeShaderCache();
                    const auto* match = cache.findMicrocode(hash);
                    const bool moduleReady =
                        match != nullptr && ensureShaderModule(match->shaderHash);
                    if (std::getenv("XERENGE_XENOS_SHADER_TRACE") != nullptr)
                    {
                        std::cerr << "Xenos shader cache "
                                  << (match != nullptr ? "hit" : "miss")
                                  << " hash=0x" << std::hex << hash
                                  << " stage=" << shaderType
                                  << " bytes=" << std::dec << byteSize;
                        if (match != nullptr)
                        {
                            std::cerr << " compiled=0x" << std::hex << match->shaderHash;
                            std::cerr << " module=" << (moduleReady ? "ready" : "missing");
                            if (const auto* compiled = cache.find(match->shaderHash))
                            {
                                size_t spirvWords = 0;
                                const bool spirvValid = cache.spirv(*compiled, spirvWords) != nullptr;
                                std::cerr << " spirv=" << (spirvValid ? "valid" : "invalid")
                                          << '/' << std::dec << spirvWords;
                            }
                        }
                        std::cerr << std::dec << '\n';
                    }
                }
            }
            if (opcode == 0x27u && length >= 3 && offset + 2 < dwordCount)
            {
                const uint32_t address = loadGuestBE(
                    guestBase, guestAddress + (offset + 1) * 4);
                const uint32_t startSize = loadGuestBE(
                    guestBase, guestAddress + (offset + 2) * 4);
                loadPointerShader(guestBase, address, address & 0x3u, startSize);
            }
            if (opcode == 0x46u && length >= 2 && offset + 1 < dwordCount)
            {
                const uint32_t event = loadGuestBE(
                    guestBase, guestAddress + (offset + 1) * 4);
                (void)event; // Cache events do not initiate an EDRAM copy.
                interruptPending_ = true;
            }
            if ((opcode == 0x58u || opcode == 0x59u) && length >= 4 &&
                offset + 3 < dwordCount)
            {
                const uint32_t initiator = loadGuestBE(
                    guestBase, guestAddress + (offset + 1) * 4);
                const uint32_t address = loadGuestBE(
                    guestBase, guestAddress + (offset + 2) * 4);
                const uint32_t value = loadGuestBE(
                    guestBase, guestAddress + (offset + 3) * 4);
                writeEventToGuest(guestBase, initiator, address, value, frameCount_);
                interruptPending_ = true;
                if (std::getenv("XERENGE_XENOS_EVENT_TRACE") != nullptr)
                    std::cerr << "Xenos event opcode=0x" << std::hex << opcode
                              << " initiator=0x" << initiator
                              << " address=0x" << address
                              << " value=0x" << value << std::dec << '\n';
            }
            if (std::getenv("XERENGE_XENOS_PACKET_TRACE") != nullptr)
            {
                static std::atomic<uint32_t> traceCount = 0;
                if (std::getenv("XERENGE_XENOS_PACKET_TRACE_ALL") != nullptr ||
                    traceCount.fetch_add(1, std::memory_order_relaxed) < 256)
                {
                    std::cerr << "Xenos PM4 depth=" << recursionDepth
                              << " address=0x" << std::hex << guestAddress + offset * 4
                              << " opcode=0x" << opcode << std::dec
                              << " dwords=" << length;
                    for (uint32_t i = 1; i < std::min<uint32_t>(length, 9); ++i)
                        std::cerr << " " << std::hex
                                  << loadGuestBE(guestBase, guestAddress + (offset + i) * 4);
                    std::cerr << std::dec << '\n';
                }
            }
            if (opcode == 0x3Fu && length >= 3 && offset + 2 < dwordCount)
            {
                // CP_INDIRECT_BUFFER stores a 29-bit physical address.  The
                // title's physical allocations live in the 0x60000000 guest
                // alias, while PM4 strips those high virtual-address bits.
                const uint32_t physicalAddress =
                    loadGuestBE(guestBase, guestAddress + (offset + 1) * 4);
                const uint32_t indirectCount =
                    loadGuestBE(guestBase, guestAddress + (offset + 2) * 4) & 0xFFFFFu;
                const uint32_t indirectAddress =
                    0x60000000u | (physicalAddress & 0x1FFFFFFFu);
                processBuffer(guestBase, indirectAddress, indirectCount, recursionDepth + 1);
            }
            if (opcode == 0x36u && length >= 2 && offset + 1 < dwordCount)
            {
                // DRAW_INDX_2 carries VGT_DRAW_INITIATOR as its only payload.
                // Keep it in the register file so later rasterization sees the
                // same state as a SET_CONSTANT/Type-0 packet would provide.
                gpuRegisters_[0x21FC] = loadGuestBE(
                    guestBase, guestAddress + (offset + 1) * 4);
                rasterizeDraw(guestBase, gpuRegisters_[0x21FC]);
            }
            if (opcode == 0x22u && length >= 3 && offset + 2 < dwordCount)
            {
                // DRAW_INDX starts with a viz-query token followed by
                // VGT_DRAW_INITIATOR.  The remaining words describe the DMA
                // index buffer when the source isn't auto-indexed.
                gpuRegisters_[0x21FC] = loadGuestBE(
                    guestBase, guestAddress + (offset + 2) * 4);
                rasterizeDraw(guestBase, gpuRegisters_[0x21FC]);
            }
            if (opcode == 0x22u || opcode == 0x36u)
            {
                ++drawPacketCount_;
                if (std::getenv("XERENGE_XENOS_DRAW_TRACE") != nullptr)
                {
                    const uint32_t initiator = gpuRegisters_[0x21FC];
                    std::cerr << "Xenos draw state surface=0x" << std::hex
                              << gpuRegisters_[0x2000]
                              << " color=0x" << gpuRegisters_[0x2001]
                              << " mask=0x" << gpuRegisters_[0x2104]
                              << " mode=0x" << gpuRegisters_[0x2208]
                              << " blend=0x" << gpuRegisters_[0x2201]
                              << " depth=0x" << gpuRegisters_[0x2200]
                              << " initiator=0x" << initiator
                              << " prim=" << (initiator & 0x3Fu)
                              << " source=" << ((initiator >> 6) & 0x3u)
                              << " indices=" << (initiator >> 16)
                              << " program=0x" << gpuRegisters_[0x2180]
                              << " vsShader=0x" << activeVertexShaderHash_
                              << "/" << activeVertexShaderDwords_
                              << " psShader=0x" << activePixelShaderHash_
                              << "/" << activePixelShaderDwords_
                              << " vsConst=0x" << gpuRegisters_[0x2307]
                              << " psConst=0x" << gpuRegisters_[0x2308]
                              << " copyBase=0x" << gpuRegisters_[0x2319]
                              << " copyCtl=0x" << gpuRegisters_[0x2318]
                              << " copyPitch=0x" << gpuRegisters_[0x231A]
                              << " copyInfo=0x" << gpuRegisters_[0x231B]
                              << " clear=0x" << gpuRegisters_[0x231E]
                              << std::dec << '\n';
                    if (std::getenv("XERENGE_XENOS_FETCH_TRACE") != nullptr)
                    {
                        const uint32_t fetchDwords =
                            std::getenv("XERENGE_XENOS_FETCH_FULL") != nullptr ? 96u : 6u;
                        std::cerr << "Xenos fetch0=" << std::hex;
                        for (uint32_t i = 0; i < fetchDwords; ++i)
                            std::cerr << " " << gpuRegisters_[0x4800 + i];
                        std::cerr << std::dec << '\n';
                        if (std::getenv("XERENGE_XENOS_CONSTANT_TRACE") != nullptr)
                        {
                            std::cerr << "Xenos vs constants:" << std::hex;
                            for (uint32_t i = 0; i < 24; ++i)
                                std::cerr << " " << gpuRegisters_[0x4000 + i];
                            std::cerr << std::dec << '\n';
                        }
                        const uint32_t fetch0 = gpuRegisters_[0x4800];
                        const uint32_t fetch1 = gpuRegisters_[0x4801];
                        if ((fetch0 & 0x3u) == 3u)
                        {
                            const uint32_t physicalAddress = (fetch0 >> 2) << 2;
                            const uint32_t address = gpuPhysicalToGuest(physicalAddress);
                            const uint32_t words = (fetch1 >> 2) & 0xFFFFFFu;
                            std::cerr << "Xenos vertex-data physical=0x" << std::hex
                                      << physicalAddress << " guest=0x" << address
                                      << " words=" << std::dec << words << ':';
                            for (uint32_t vertex = 0; vertex < 3; ++vertex)
                            {
                                std::cerr << " [";
                                for (uint32_t word = 0; word < words; ++word)
                                    std::cerr << (word ? " " : "") << std::hex
                                              << loadGuestBE(guestBase,
                                                  address + (vertex * words + word) * 4);
                                std::cerr << "]";
                            }
                            std::cerr << std::dec << '\n';
                        }
                    }
                }
            }
            if (opcode == 0x64u)
            {
                ++swapPacketCount_;
                if (length >= 5 && offset + 4 < dwordCount)
                {
                    const uint32_t width = loadGuestBE(guestBase, guestAddress + (offset + 3) * 4);
                    const uint32_t height = loadGuestBE(guestBase, guestAddress + (offset + 4) * 4);
                    lastFrameWidth_ = width;
                    lastFrameHeight_ = height;
                    ++frameCount_;
                }
            }
        }
        if (length == 0 || length > dwordCount - offset)
            break;
        ++packetCount_;
        offset += length;
    }
}

void XenosGpu::write(uint8_t* guestBase, uint32_t address, uint64_t value, uint32_t width)
{
    std::lock_guard lock(mutex_);
    ++mmioWriteCount_;
    if (address < kMmioBase || address >= kMmioBase + kMmioSize || width != 4)
        return;

    const uint32_t index = (address - kMmioBase) / 4;
    if (index >= registers_.size())
        return;

    const uint32_t registerValue = static_cast<uint32_t>(value);
    registers_[index] = registerValue;
    if (index == kCpRbBase)
    {
        ringBase_ = registerValue;
    }
    else if (index == kCpRbCntl)
    {
        const uint32_t sizeLog2 = registerValue & 0x3Fu;
        ringSizeDwords_ = sizeLog2 < 30 ? (1u << (sizeLog2 + 1)) : 0;
    }
    else if (index == kCpRbRptrAddr)
    {
        readPointerWriteback_ = gpuPhysicalToGuest(registerValue & 0x3FFFFFFCu);
    }
    else if (index == kCpRbWptr)
    {
        writePointer_ = registerValue;
        if (std::getenv("XERENGE_PPC_MMIO_TRACE") != nullptr && writePointer_ <= 0x80)
        {
            std::cerr << "Xenos ring base=0x" << std::hex << ringBase_
                      << " wptr=0x" << writePointer_ << ":";
            for (uint32_t i = 0; i < std::min<uint32_t>(writePointer_, 32); ++i)
                std::cerr << " " << loadGuestBE(guestBase, ringBase_ + i * 4);
            std::cerr << std::dec << '\n';
        }
        processRing(guestBase);
    }
}

void XenosGpu::processRing(uint8_t* guestBase)
{
    if (ringBase_ == 0 || ringSizeDwords_ == 0)
    {
        if (std::getenv("XERENGE_PPC_MMIO_TRACE") != nullptr && mmioWriteCount_ <= 4)
            std::cerr << "Xenos CP_RB_WPTR=" << writePointer_
                      << " without configured ring buffer\n";
        return;
    }

    const uint32_t target = writePointer_ % ringSizeDwords_;
    uint32_t available =
        (target + ringSizeDwords_ - readPointer_) % ringSizeDwords_;
    const auto ringLoad = [&](uint32_t index) {
        return loadGuestBE(guestBase,
            ringBase_ + (index % ringSizeDwords_) * 4);
    };
    while (readPointer_ != target && available != 0)
    {
        const uint32_t packet = ringLoad(readPointer_);
        const uint32_t type = packet >> 30;
        uint32_t length = 1;
        if (type == 0)
        {
            ++type0Count_;
            length = ((packet >> 16) & 0x3FFFu) + 2;
            const uint32_t baseRegister = packet & 0x7FFFu;
            const bool writeOne = (packet & 0x8000u) != 0;
            for (uint32_t i = 0; i + 1 < length; ++i)
            {
                const uint32_t index = writeOne ? baseRegister : baseRegister + i;
                if (index < gpuRegisters_.size())
                    writeGpuRegister(index, ringLoad(readPointer_ + 1 + i));
            }
        }
        else if (type == 3)
        {
            ++type3Count_;
            length = ((packet >> 16) & 0x3FFFu) + 2;
            const uint32_t opcode = (packet >> 8) & 0xFFu;
            ++opcodeCounts_[opcode];
            if (opcode == 0x2Du || opcode == 0x55u || opcode == 0x56u)
            {
                const uint32_t payloadCount = length - 1;
                if (payloadCount != 0 && payloadCount < available)
                {
                    const uint32_t offsetType = ringLoad(readPointer_ + 1);
                    uint32_t index = offsetType & (opcode == 0x2Du ? 0x7FFu : 0xFFFFu);
                    if (opcode == 0x2Du)
                    {
                        switch ((offsetType >> 16) & 0xFFu)
                        {
                        case 0: index += 0x4000; break;
                        case 1: index += 0x4800; break;
                        case 2: index += 0x4900; break;
                        case 3: index += 0x4908; break;
                        case 4: index += 0x2000; break;
                        default: index = 0xFFFFFFFFu; break;
                        }
                    }
                    if (index != 0xFFFFFFFFu)
                        for (uint32_t i = 1; i < payloadCount; ++i)
                            writeGpuRegister(index + i - 1,
                                ringLoad(readPointer_ + 1 + i));
                }
            }
            if (opcode == 0x2Fu && length >= 4 && 3 < available)
            {
                const uint32_t physicalAddress = ringLoad(readPointer_ + 1) & 0x3FFFFFFFu;
                const uint32_t offsetType = ringLoad(readPointer_ + 2);
                const uint32_t sizeDwords = ringLoad(readPointer_ + 3) & 0xFFFu;
                uint32_t index = offsetType & 0x7FFu;
                switch ((offsetType >> 16) & 0xFFu)
                {
                case 0: index += 0x4000; break;
                case 1: index += 0x4800; break;
                case 2: index += 0x4900; break;
                case 3: index += 0x4908; break;
                case 4: index += 0x2000; break;
                default: index = 0xFFFFFFFFu; break;
                }
                const uint32_t source = gpuPhysicalToGuest(physicalAddress);
                if (index != 0xFFFFFFFFu && sizeDwords <= 0x8000u - index)
                    for (uint32_t i = 0; i < sizeDwords; ++i)
                        writeGpuRegister(index + i,
                            loadGuestBE(guestBase, source + i * 4));
            }

            if (opcode == 0x21u && length >= 4 && 3 < available)
            {
                const uint32_t info = ringLoad(readPointer_ + 1);
                const uint32_t andOperand = ringLoad(readPointer_ + 2);
                const uint32_t orOperand = ringLoad(readPointer_ + 3);
                const uint32_t index = info & 0x1FFFu;
                if (index < gpuRegisters_.size())
                {
                    uint32_t value = gpuRegisters_[index];
                    value &= (info & 0x80000000u) != 0
                        ? gpuRegisters_[andOperand & 0x1FFFu] : andOperand;
                    value |= (info & 0x40000000u) != 0
                        ? gpuRegisters_[orOperand & 0x1FFFu] : orOperand;
                    writeGpuRegister(index, value);
                }
            }

            if (opcode == 0x3Du && length >= 3 && 2 < available)
            {
                uint32_t address = ringLoad(readPointer_ + 1);
                for (uint32_t i = 0; i + 2 < length; ++i)
                {
                    writeGpuMemory(guestBase, address,
                        ringLoad(readPointer_ + 2 + i));
                    address += 4;
                }
            }

            if (opcode == 0x3Cu && length >= 6 && 5 < available)
            {
                const uint32_t waitInfo = ringLoad(readPointer_ + 1);
                const uint32_t pollAddress = ringLoad(readPointer_ + 2);
                const uint32_t reference = ringLoad(readPointer_ + 3);
                const uint32_t mask = ringLoad(readPointer_ + 4);
                const uint32_t value = (waitInfo & 0x10u) != 0
                    ? loadGuestBE(guestBase, gpuPhysicalToGuest(pollAddress & ~3u))
                    : (pollAddress < gpuRegisters_.size() ? gpuRegisters_[pollAddress] : 0);
                const uint32_t relation = waitInfo & 7u;
                const uint32_t masked = value & mask;
                const bool matched = relation == 0u ? false :
                    relation == 1u ? masked < reference :
                    relation == 2u ? masked <= reference :
                    relation == 3u ? masked == reference :
                    relation == 4u ? masked != reference :
                    relation == 5u ? masked >= reference :
                    relation == 6u ? masked > reference : true;
                if (!matched && std::getenv("XERENGE_XENOS_WAIT_TRACE") != nullptr)
                    std::cerr << "Xenos WAIT_REG_MEM unsatisfied address=0x"
                              << std::hex << pollAddress << " value=0x" << value
                              << " ref=0x" << reference << " mask=0x" << mask
                              << std::dec << '\n';
            }
            if (opcode == 0x27u && length >= 3 && 2 < available)
            {
                const uint32_t address = ringLoad(readPointer_ + 1);
                const uint32_t startSize = ringLoad(readPointer_ + 2);
                loadPointerShader(guestBase, address, address & 0x3u, startSize);
            }
            if (opcode == 0x46u && length >= 2 && 1 < available)
            {
                const uint32_t event = ringLoad(readPointer_ + 1);
                (void)event; // Cache events do not initiate an EDRAM copy.
                interruptPending_ = true;
            }
            if ((opcode == 0x58u || opcode == 0x59u) && length >= 4 &&
                3 < available)
            {
                const uint32_t initiator = ringLoad(readPointer_ + 1);
                const uint32_t address = ringLoad(readPointer_ + 2);
                const uint32_t value = ringLoad(readPointer_ + 3);
                writeEventToGuest(guestBase, initiator, address, value, frameCount_);
                interruptPending_ = true;
                if (std::getenv("XERENGE_XENOS_EVENT_TRACE") != nullptr)
                    std::cerr << "Xenos event opcode=0x" << std::hex << opcode
                              << " initiator=0x" << initiator
                              << " address=0x" << address
                              << " value=0x" << value << std::dec << '\n';
            }
            if (opcode == 0x3Fu && length >= 3 && 2 < available)
            {
                const uint32_t physicalAddress = ringLoad(readPointer_ + 1);
                const uint32_t indirectCount = ringLoad(readPointer_ + 2) & 0xFFFFFu;
                const uint32_t indirectAddress =
                    0x60000000u | (physicalAddress & 0x1FFFFFFFu);
                processBuffer(guestBase, indirectAddress, indirectCount, 1);
            }
            if (opcode == 0x36u && length >= 2 && 1 < available)
            {
                gpuRegisters_[0x21FC] = ringLoad(readPointer_ + 1);
                rasterizeDraw(guestBase, gpuRegisters_[0x21FC]);
            }
            if (opcode == 0x22u && length >= 3 && 2 < available)
            {
                gpuRegisters_[0x21FC] = ringLoad(readPointer_ + 2);
                rasterizeDraw(guestBase, gpuRegisters_[0x21FC]);
            }
            if (opcode == 0x22u || opcode == 0x36u)
            {
                ++drawPacketCount_;
                if (std::getenv("XERENGE_XENOS_DRAW_TRACE") != nullptr)
                {
                    const uint32_t initiator = gpuRegisters_[0x21FC];
                    std::cerr << "Xenos draw state surface=0x" << std::hex
                              << gpuRegisters_[0x2000]
                              << " color=0x" << gpuRegisters_[0x2001]
                              << " mask=0x" << gpuRegisters_[0x2104]
                              << " mode=0x" << gpuRegisters_[0x2208]
                              << " blend=0x" << gpuRegisters_[0x2201]
                              << " depth=0x" << gpuRegisters_[0x2200]
                              << " initiator=0x" << initiator
                              << " prim=" << (initiator & 0x3Fu)
                              << " source=" << ((initiator >> 6) & 0x3u)
                              << " indices=" << (initiator >> 16)
                              << " program=0x" << gpuRegisters_[0x2180]
                              << " vsShader=0x" << activeVertexShaderHash_
                              << "/" << activeVertexShaderDwords_
                              << " psShader=0x" << activePixelShaderHash_
                              << "/" << activePixelShaderDwords_
                              << " vsConst=0x" << gpuRegisters_[0x2307]
                              << " psConst=0x" << gpuRegisters_[0x2308]
                              << " copyBase=0x" << gpuRegisters_[0x2319]
                              << " copyCtl=0x" << gpuRegisters_[0x2318]
                              << " copyPitch=0x" << gpuRegisters_[0x231A]
                              << " copyInfo=0x" << gpuRegisters_[0x231B]
                              << " clear=0x" << gpuRegisters_[0x231E]
                              << std::dec << '\n';
                    if (std::getenv("XERENGE_XENOS_FETCH_TRACE") != nullptr)
                    {
                        std::cerr << "Xenos fetch0=" << std::hex;
                        for (uint32_t i = 0; i < 6; ++i)
                            std::cerr << " " << gpuRegisters_[0x4800 + i];
                        std::cerr << " fetch1=";
                        for (uint32_t i = 0; i < 6; ++i)
                            std::cerr << " " << gpuRegisters_[0x4806 + i];
                        std::cerr << std::dec << '\n';
                        if (std::getenv("XERENGE_XENOS_CONSTANT_TRACE") != nullptr)
                        {
                            std::cerr << "Xenos vs constants:" << std::hex;
                            for (uint32_t i = 0; i < 24; ++i)
                                std::cerr << " " << gpuRegisters_[0x4000 + i];
                            std::cerr << std::dec << '\n';
                        }
                        const uint32_t fetch0 = gpuRegisters_[0x4800];
                        const uint32_t fetch1 = gpuRegisters_[0x4801];
                        if ((fetch0 & 0x3u) == 3u)
                        {
                            const uint32_t physicalAddress = (fetch0 >> 2) << 2;
                            const uint32_t address = gpuPhysicalToGuest(physicalAddress);
                            const uint32_t words = (fetch1 >> 2) & 0xFFFFFFu;
                            std::cerr << "Xenos vertex-data physical=0x" << std::hex
                                      << physicalAddress << " guest=0x" << address
                                      << " words=" << std::dec << words << ':';
                            for (uint32_t vertex = 0; vertex < 3; ++vertex)
                            {
                                std::cerr << " [";
                                for (uint32_t word = 0; word < words; ++word)
                                    std::cerr << (word ? " " : "") << std::hex
                                              << loadGuestBE(guestBase,
                                                  address + (vertex * words + word) * 4);
                                std::cerr << "]";
                            }
                            std::cerr << std::dec << '\n';
                        }
                    }
                }
            }
            if (opcode == 0x64u)
            {
                ++swapPacketCount_;
                if (length >= 5 && 4 < available)
                {
                    lastFrameWidth_ = ringLoad(readPointer_ + 3);
                    lastFrameHeight_ = ringLoad(readPointer_ + 4);
                    ++frameCount_;
                }
            }
        }

        if (length == 0 || length > available)
            break;
        ++packetCount_;
        readPointer_ = (readPointer_ + length) % ringSizeDwords_;
        available -= length;
    }

    // The read pointer writeback is the first synchronization primitive the
    // guest observes.  Use the same guest endian representation as PPC stores.
    if (readPointerWriteback_ != 0)
        storeGuestBE(guestBase, readPointerWriteback_, readPointer_);
}
