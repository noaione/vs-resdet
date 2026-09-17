#include <glslang/Include/glslang_c_interface.h>

#include "vkfft_dispatch.hpp"

#include "shader_sources.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace vsresdet;

constexpr int kVapourSynthApi = VAPOURSYNTH_API_VERSION;
constexpr uint32_t kUnpackLocalSize = 64;
constexpr uint32_t kScoreLocalSize = 64;
constexpr size_t kErrorBufferSize = 1024;
constexpr const char *kWidthScoresProperty = "resdet_width_scores";
constexpr const char *kHeightScoresProperty = "resdet_height_scores";
constexpr const char *kMethodProperty = "resdet_method";

enum class Method : uint32_t {
    Sign = 0,
    Magnitude = 1,
    Original = 2,
    ZeroCrossing = 3,
};

struct UnpackParameters {
    uint32_t width;
    uint32_t height;
    uint32_t strideBytes;
    uint32_t bytesPerSample;
    uint32_t bitsPerSample;
    uint32_t sampleType;
};

struct TransposeParameters {
    uint32_t width;
    uint32_t height;
};

struct ScoreParameters {
    uint32_t width;
    uint32_t height;
    uint32_t axis;
    uint32_t range;
    uint32_t outputOffset;
    uint32_t method;
};

static_assert(sizeof(UnpackParameters) <= 128);
static_assert(sizeof(TransposeParameters) <= 128);
static_assert(sizeof(ScoreParameters) <= 128);

struct Buffer {
    VSGPUBuffer *handle = nullptr;
    VSVulkanBufferInfo info{};
};

struct PipelineState {
    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline unpackPipeline = VK_NULL_HANDLE;
    VkPipeline transposePipeline = VK_NULL_HANDLE;
    VkPipeline scorePipeline = VK_NULL_HANDLE;
};

struct AnalyzeData {
    VSNode *input = nullptr;
    VSVideoInfo vi{};
    int range = 1;
    Method method = Method::Sign;

    VSCore *core = nullptr;
    const VSAPI *vsapi = nullptr;
    const VSVULKANAPI *gpu = nullptr;
    VSVulkanCoreHandles handles{};
    const VSVulkanFunctions *functions = nullptr;
    VkFFTDispatch dispatch{};

    VSGPUExecPool *execPool = nullptr;
    VkCommandPool vkfftCommandPool = VK_NULL_HANDLE;
    VkFence vkfftFence = VK_NULL_HANDLE;
    VkQueue computeQueue = VK_NULL_HANDLE;

    Buffer planBuffer;
    Buffer planTempBuffer;
    uint64_t fftBufferSize = 0;
    VkBuffer fftWidthBuffer = VK_NULL_HANDLE;
    VkBuffer fftHeightBuffer = VK_NULL_HANDLE;
    VkFFTApplication fftWidth{};
    VkFFTApplication fftHeight{};
    bool fftWidthInitialized = false;
    bool fftHeightInitialized = false;
    bool fftInitialized = false;
    PipelineState pipelines{};

    std::mutex analyzeMutex;
};

template <typename T>
T loadInstanceFunction(const VSVulkanCoreHandles &handles, const char *name) {
    return reinterpret_cast<T>(handles.getInstanceProcAddr(handles.instance, name));
}

static std::string errorMessage(const char *prefix, const char *detail) {
    std::string result(prefix);
    if (detail && *detail) {
        result += ": ";
        result += detail;
    }
    return result;
}

static const char *methodName(Method method) {
    switch (method) {
    case Method::Sign:
        return "sign";
    case Method::Magnitude:
        return "mag";
    case Method::Original:
        return "orig";
    case Method::ZeroCrossing:
        return "zerox";
    }
    return "sign";
}

static bool parseMethod(const VSMap *map, const VSAPI *vsapi,
                        Method &method, std::string &error) {
    method = Method::Sign;
    if (vsapi->mapNumElements(map, "method") == 0) {
        return true;
    }

    int mapError = 0;
    const char *value = vsapi->mapGetData(map, "method", 0, &mapError);
    if (mapError || !value) {
        error = "resdet.Analyze: method must be one of sign, mag, orig, or zerox";
        return false;
    }
    const std::string name(value);
    if (name == "sign") {
        method = Method::Sign;
    } else if (name == "mag") {
        method = Method::Magnitude;
    } else if (name == "orig") {
        method = Method::Original;
    } else if (name == "zerox") {
        method = Method::ZeroCrossing;
    } else {
        error = "resdet.Analyze: method must be one of sign, mag, orig, or zerox";
        return false;
    }
    return true;
}

static std::string vkfftError(VkFFTResult result) {
    std::ostringstream stream;
    stream << "VkFFT failed with result " << static_cast<int>(result);
    return stream.str();
}

static bool checkedSize(int64_t value, uint32_t &result) {
    if (value <= 0 || static_cast<uint64_t>(value) > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    result = static_cast<uint32_t>(value);
    return true;
}

static VkFFTDispatch makeDispatch(const VSVulkanCoreHandles &handles,
                                  const VSVulkanFunctions *functions) {
    VkFFTDispatch dispatch{};
    dispatch.core = functions;

    dispatch.enumeratePhysicalDevices = loadInstanceFunction<PFN_vkEnumeratePhysicalDevices>(
        handles, "vkEnumeratePhysicalDevices");
    dispatch.createDevice = loadInstanceFunction<PFN_vkCreateDevice>(
        handles, "vkCreateDevice");

    dispatch.getPhysicalDeviceProperties = loadInstanceFunction<PFN_vkGetPhysicalDeviceProperties>(
        handles, "vkGetPhysicalDeviceProperties");
    dispatch.getPhysicalDeviceMemoryProperties = loadInstanceFunction<PFN_vkGetPhysicalDeviceMemoryProperties>(
        handles, "vkGetPhysicalDeviceMemoryProperties");

    const auto loadDevice = [&](const char *name) {
        return functions->vkGetDeviceProcAddr(handles.device, name);
    };
#define LOAD_DEVICE(member, name) \
    dispatch.member = reinterpret_cast<PFN_vk##name>(loadDevice("vk" #name))
    LOAD_DEVICE(allocateCommandBuffers, AllocateCommandBuffers);
    LOAD_DEVICE(allocateDescriptorSets, AllocateDescriptorSets);
    LOAD_DEVICE(allocateMemory, AllocateMemory);
    LOAD_DEVICE(beginCommandBuffer, BeginCommandBuffer);
    dispatch.getBufferMemoryRequirements = reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(loadDevice("vkGetBufferMemoryRequirements"));
    dispatch.bindBufferMemory = reinterpret_cast<PFN_vkBindBufferMemory>(loadDevice("vkBindBufferMemory"));
    LOAD_DEVICE(cmdBindDescriptorSets, CmdBindDescriptorSets);
    LOAD_DEVICE(cmdBindPipeline, CmdBindPipeline);
    LOAD_DEVICE(cmdCopyBuffer, CmdCopyBuffer);
    LOAD_DEVICE(cmdDispatch, CmdDispatch);
    dispatch.mapMemory = reinterpret_cast<PFN_vkMapMemory>(loadDevice("vkMapMemory"));
    dispatch.unmapMemory = reinterpret_cast<PFN_vkUnmapMemory>(loadDevice("vkUnmapMemory"));
    dispatch.queueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(loadDevice("vkQueueSubmit"));
    dispatch.waitForFences = reinterpret_cast<PFN_vkWaitForFences>(loadDevice("vkWaitForFences"));
    dispatch.resetFences = reinterpret_cast<PFN_vkResetFences>(loadDevice("vkResetFences"));
    dispatch.cmdPipelineBarrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(loadDevice("vkCmdPipelineBarrier"));
    dispatch.cmdPushConstants = reinterpret_cast<PFN_vkCmdPushConstants>(loadDevice("vkCmdPushConstants"));
    dispatch.cmdPushDescriptorSetKHR = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(loadDevice("vkCmdPushDescriptorSetKHR"));
    if (!dispatch.cmdPushDescriptorSetKHR) {
        dispatch.cmdPushDescriptorSetKHR = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(
            functions->vkCmdPushDescriptorSet);
    }
#define LOAD_DEVICE(member, name) \
    dispatch.member = reinterpret_cast<PFN_vk##name>(loadDevice("vk" #name))
    LOAD_DEVICE(createBuffer, CreateBuffer);
    LOAD_DEVICE(createCommandPool, CreateCommandPool);
    LOAD_DEVICE(createComputePipelines, CreateComputePipelines);
    LOAD_DEVICE(createDescriptorPool, CreateDescriptorPool);
    LOAD_DEVICE(createDescriptorSetLayout, CreateDescriptorSetLayout);
    LOAD_DEVICE(createFence, CreateFence);
    LOAD_DEVICE(createPipelineLayout, CreatePipelineLayout);
    LOAD_DEVICE(createShaderModule, CreateShaderModule);
    LOAD_DEVICE(destroyBuffer, DestroyBuffer);
    LOAD_DEVICE(destroyDescriptorPool, DestroyDescriptorPool);
    LOAD_DEVICE(destroyDescriptorSetLayout, DestroyDescriptorSetLayout);
    LOAD_DEVICE(destroyPipeline, DestroyPipeline);
    LOAD_DEVICE(destroyPipelineLayout, DestroyPipelineLayout);
    LOAD_DEVICE(destroyShaderModule, DestroyShaderModule);
    LOAD_DEVICE(endCommandBuffer, EndCommandBuffer);
    LOAD_DEVICE(freeCommandBuffers, FreeCommandBuffers);
    LOAD_DEVICE(freeMemory, FreeMemory);
    LOAD_DEVICE(getDeviceQueue, GetDeviceQueue);
    LOAD_DEVICE(updateDescriptorSets, UpdateDescriptorSets);
    dispatch.getDeviceProcAddr = functions->vkGetDeviceProcAddr;
#undef LOAD_DEVICE
    return dispatch;
}

static bool dispatchIsComplete(const VkFFTDispatch &dispatch) {
    return dispatch.core
        && dispatch.allocateCommandBuffers
        && dispatch.allocateDescriptorSets
        && dispatch.allocateMemory
        && dispatch.beginCommandBuffer
        && dispatch.getPhysicalDeviceProperties
        && dispatch.getPhysicalDeviceMemoryProperties
        && dispatch.getBufferMemoryRequirements
        && dispatch.bindBufferMemory
        && dispatch.cmdBindDescriptorSets
        && dispatch.cmdBindPipeline
        && dispatch.cmdCopyBuffer
        && dispatch.cmdDispatch
        && dispatch.mapMemory
        && dispatch.unmapMemory
        && dispatch.queueSubmit
        && dispatch.waitForFences
        && dispatch.resetFences
        && dispatch.cmdPipelineBarrier
        && dispatch.cmdPushConstants
        && dispatch.cmdPushDescriptorSetKHR
        && dispatch.createBuffer
        && dispatch.createCommandPool
        && dispatch.createComputePipelines
        && dispatch.createDescriptorPool
        && dispatch.createDescriptorSetLayout
        && dispatch.createDevice
        && dispatch.createFence
        && dispatch.createPipelineLayout
        && dispatch.createShaderModule
        && dispatch.destroyBuffer
        && dispatch.destroyDescriptorPool
        && dispatch.destroyDescriptorSetLayout
        && dispatch.destroyPipeline
        && dispatch.destroyPipelineLayout
        && dispatch.destroyShaderModule
        && dispatch.endCommandBuffer
        && dispatch.enumeratePhysicalDevices
        && dispatch.freeCommandBuffers
        && dispatch.freeMemory
        && dispatch.getDeviceProcAddr
        && dispatch.getDeviceQueue
        && dispatch.updateDescriptorSets;
}

static Buffer createBuffer(AnalyzeData &data, VkDeviceSize size,
                           VkMemoryPropertyFlags required,
                           VkMemoryPropertyFlags preferred,
                           std::string &error) {
    Buffer buffer;
    char errorText[kErrorBufferSize]{};
    buffer.handle = data.gpu->createGPUBuffer(
        data.core,
        size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        required,
        preferred,
        &buffer.info,
        errorText,
        sizeof(errorText));
    if (!buffer.handle) {
        error = errorText[0] ? errorText : "VapourSynth could not allocate a GPU buffer";
    }
    return buffer;
}

static void destroyBuffer(AnalyzeData &data, Buffer &buffer) {
    if (buffer.handle) {
        data.gpu->destroyGPUBuffer(buffer.handle);
        buffer = {};
    }
}

static VkShaderModule createShaderModule(AnalyzeData &data, const char *source,
                                         std::string &error) {
    char shaderError[kErrorBufferSize]{};
    VSGPUShader *shader = data.gpu->compileGPUShader(
        data.core, slGLSL, source, shaderError, sizeof(shaderError));
    if (!shader) {
        error = errorMessage("VapourSynth could not compile a resdet shader", shaderError);
        return VK_NULL_HANDLE;
    }

    size_t codeSize = 0;
    const uint32_t *code = data.gpu->getGPUShaderCode(shader, &codeSize);
    VkShaderModule module = VK_NULL_HANDLE;
    if (!code || codeSize == 0 || codeSize % sizeof(uint32_t) != 0) {
        error = "VapourSynth returned invalid SPIR-V for a resdet shader";
    } else {
        VkShaderModuleCreateInfo createInfo{
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .codeSize = codeSize,
            .pCode = code,
        };
        if (data.functions->vkCreateShaderModule(
                data.handles.device, &createInfo, nullptr, &module) != VK_SUCCESS) {
            error = "Vulkan could not create a resdet shader module";
        }
    }
    data.gpu->freeGPUShader(shader);
    return module;
}

static bool createPipelines(AnalyzeData &data, std::string &error) {
    VkDescriptorSetLayoutBinding bindings[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo descriptorInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
        .bindingCount = 2,
        .pBindings = bindings,
    };
    if (data.functions->vkCreateDescriptorSetLayout(
            data.handles.device, &descriptorInfo, nullptr,
            &data.pipelines.descriptorSetLayout) != VK_SUCCESS) {
        error = "Vulkan could not create the resdet descriptor layout";
        return false;
    }

    VkPushConstantRange pushConstants{
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = static_cast<uint32_t>(sizeof(UnpackParameters)),
    };
    VkPipelineLayoutCreateInfo layoutInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = &data.pipelines.descriptorSetLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pushConstants,
    };
    if (data.functions->vkCreatePipelineLayout(
            data.handles.device, &layoutInfo, nullptr,
            &data.pipelines.pipelineLayout) != VK_SUCCESS) {
        error = "Vulkan could not create the resdet pipeline layout";
        return false;
    }

    VkShaderModule unpackModule = createShaderModule(data, kUnpackYShader, error);
    if (!unpackModule) {
        return false;
    }
    VkShaderModule scoreModule = createShaderModule(data, kScoreMethodsShader, error);
    if (!scoreModule) {
        data.functions->vkDestroyShaderModule(data.handles.device, unpackModule, nullptr);
        return false;
    }
    VkShaderModule transposeModule = createShaderModule(data, kTransposeShader, error);
    if (!transposeModule) {
        data.functions->vkDestroyShaderModule(data.handles.device, unpackModule, nullptr);
        data.functions->vkDestroyShaderModule(data.handles.device, scoreModule, nullptr);
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[3]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stages[0].module = unpackModule;
    stages[0].pName = "main";
    stages[1] = stages[0];
    stages[1].module = transposeModule;
    stages[2] = stages[0];
    stages[2].module = scoreModule;

    VkComputePipelineCreateInfo pipelineInfos[3]{};
    for (auto &info : pipelineInfos) {
        info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        info.layout = data.pipelines.pipelineLayout;
    }
    pipelineInfos[0].stage = stages[0];
    pipelineInfos[1].stage = stages[1];
    pipelineInfos[2].stage = stages[2];
    VkPipeline created[3]{VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    if (data.functions->vkCreateComputePipelines(
            data.handles.device, VK_NULL_HANDLE, 3, pipelineInfos,
            nullptr, created) != VK_SUCCESS) {
        error = "Vulkan could not create the resdet compute pipelines";
    } else {
        data.pipelines.unpackPipeline = created[0];
        data.pipelines.transposePipeline = created[1];
        data.pipelines.scorePipeline = created[2];
    }

    data.functions->vkDestroyShaderModule(data.handles.device, unpackModule, nullptr);
    data.functions->vkDestroyShaderModule(data.handles.device, transposeModule, nullptr);
    data.functions->vkDestroyShaderModule(data.handles.device, scoreModule, nullptr);
    return error.empty();
}

static void destroyPipelines(AnalyzeData &data) {
    if (data.pipelines.unpackPipeline) {
        data.functions->vkDestroyPipeline(data.handles.device,
                                          data.pipelines.unpackPipeline, nullptr);
    }
    if (data.pipelines.scorePipeline) {
        data.functions->vkDestroyPipeline(data.handles.device,
                                          data.pipelines.scorePipeline, nullptr);
    }
    if (data.pipelines.transposePipeline) {
        data.functions->vkDestroyPipeline(data.handles.device,
                                          data.pipelines.transposePipeline, nullptr);
    }
    if (data.pipelines.pipelineLayout) {
        data.functions->vkDestroyPipelineLayout(data.handles.device,
                                                data.pipelines.pipelineLayout, nullptr);
    }
    if (data.pipelines.descriptorSetLayout) {
        data.functions->vkDestroyDescriptorSetLayout(
            data.handles.device, data.pipelines.descriptorSetLayout, nullptr);
    }
    data.pipelines = {};
}

static void pushStorageDescriptors(AnalyzeData &data, VkCommandBuffer commandBuffer,
                                   VkBuffer first, VkDeviceSize firstSize,
                                   VkBuffer second, VkDeviceSize secondSize) {
    VkDescriptorBufferInfo bufferInfo[2]{
        {.buffer = first, .offset = 0, .range = firstSize},
        {.buffer = second, .offset = 0, .range = secondSize},
    };
    VkWriteDescriptorSet writes[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = VK_NULL_HANDLE;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &bufferInfo[i];
    }
    data.functions->vkCmdPushDescriptorSet(
        commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
        data.pipelines.pipelineLayout, 0, 2, writes);
}

static void bufferBarrier(AnalyzeData &data, VkCommandBuffer commandBuffer,
                          VkBuffer buffer, VkDeviceSize size,
                          VkPipelineStageFlags2 sourceStage,
                          VkAccessFlags2 sourceAccess,
                          VkPipelineStageFlags2 destinationStage,
                          VkAccessFlags2 destinationAccess) {
    VkBufferMemoryBarrier2 barrier{
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = sourceStage,
        .srcAccessMask = sourceAccess,
        .dstStageMask = destinationStage,
        .dstAccessMask = destinationAccess,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffer,
        .offset = 0,
        .size = size,
    };
    VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pNext = nullptr,
        .dependencyFlags = 0,
        .memoryBarrierCount = 0,
        .pMemoryBarriers = nullptr,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &barrier,
        .imageMemoryBarrierCount = 0,
        .pImageMemoryBarriers = nullptr,
    };
    data.functions->vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

static const VSFrame *Analyze(AnalyzeData &data, const VSFrame *input,
                              VSFrameContext *frameContext, std::string &error) {
    (void)frameContext;
    std::lock_guard<std::mutex> lock(data.analyzeMutex);

    if (data.vsapi->getFrameResidency(input) != nrGPU) {
        error = "resdet.Analyze requires a GPU-resident input clip";
        return nullptr;
    }

    VSVulkanPlaneInfo plane{};
    if (data.gpu->getGPUPlane(input, 0, &plane) != 0) {
        error = "resdet.Analyze could not access the GPU-resident Y plane";
        return nullptr;
    }

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    if (!checkedSize(data.vi.width, width) || !checkedSize(data.vi.height, height)
        || !checkedSize(data.vsapi->getStride(input, 0), stride)) {
        error = "The VapourSynth frame dimensions or stride are too large";
        return nullptr;
    }

    const VSVideoFormat &format = data.vi.format;
    if (format.bytesPerSample != 1 && format.bytesPerSample != 2
        && format.bytesPerSample != 4) {
        error = "resdet.Analyze received an unsupported Y-plane sample width";
        return nullptr;
    }
    if (format.sampleType == stInteger && format.bitsPerSample > 32) {
        error = "resdet.Analyze only supports integer samples up to 32 bits";
        return nullptr;
    }
    if (format.sampleType == stFloat && format.bytesPerSample != 2
        && format.bytesPerSample != 4) {
        error = "resdet.Analyze only supports 16-bit and 32-bit float samples";
        return nullptr;
    }
    if (data.range < 1 || data.range * 2 >= std::min(width, height)) {
        error = "resdet.Analyze range leaves no meaningful sign-score samples";
        return nullptr;
    }

    const VkDeviceSize sampleBytes = static_cast<VkDeviceSize>(width) * height * sizeof(float);
    const VkDeviceSize scoreBytes =
        (static_cast<VkDeviceSize>(width) + static_cast<VkDeviceSize>(height)) * sizeof(float);
    // Keep the transform in the buffer bound when the VkFFT plan was created.
    // This avoids rebinding VkFFT's descriptors for every frame and also makes
    // the plan's axis-to-axis ping-pong path deterministic on Vulkan drivers
    // that expose push descriptors only through Vulkan 1.4's core name.
    const VkBuffer dctBuffer = data.planBuffer.info.buffer;
    Buffer scoreBuffer = createBuffer(
        data,
        scoreBytes,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        error);
    if (!scoreBuffer.handle || !scoreBuffer.info.mapped) {
        if (scoreBuffer.handle) {
            destroyBuffer(data, scoreBuffer);
        }
        if (error.empty()) {
            error = "resdet.Analyze could not obtain a coherent mapped score buffer";
        }
        return nullptr;
    }

    char errorText[kErrorBufferSize]{};
    VSGPUExecContext *context = data.gpu->gpuExecAcquire(
        data.execPool, errorText, sizeof(errorText));
    if (!context) {
        destroyBuffer(data, scoreBuffer);
        error = errorText[0] ? errorText : "VapourSynth could not acquire a GPU execution context";
        return nullptr;
    }

    data.gpu->gpuExecReadsFrame(context, input);
    VkCommandBuffer commandBuffer = data.gpu->gpuExecCommandBuffer(context);

    UnpackParameters unpack{
        .width = width,
        .height = height,
        .strideBytes = stride,
        .bytesPerSample = static_cast<uint32_t>(format.bytesPerSample),
        .bitsPerSample = static_cast<uint32_t>(format.bitsPerSample),
        .sampleType = format.sampleType == stFloat ? 1u : 0u,
    };
    data.functions->vkCmdBindPipeline(
        commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
        data.pipelines.unpackPipeline);
    pushStorageDescriptors(data, commandBuffer, plane.buffer, plane.bufferSize,
                           dctBuffer, sampleBytes);
    data.dispatch.cmdPushConstants(
        commandBuffer, data.pipelines.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
        0, sizeof(unpack), &unpack);
    const uint64_t sampleCount = static_cast<uint64_t>(width) * height;
    data.functions->vkCmdDispatch(
        commandBuffer, static_cast<uint32_t>((sampleCount + kUnpackLocalSize - 1) / kUnpackLocalSize),
        1, 1);

    bufferBarrier(data, commandBuffer, dctBuffer, sampleBytes,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);

    VkFFTLaunchParams launchParameters{};
    launchParameters.commandBuffer = &commandBuffer;
    launchParameters.buffer = &data.fftWidthBuffer;
    {
        ActiveVkFFTDispatchScope active(&data.dispatch);
        VkFFTResult result = VkFFTAppend(&data.fftWidth, 0, &launchParameters);
        if (result != VKFFT_SUCCESS) {
            error = vkfftError(result);
            data.gpu->gpuExecAbandon(context);
            destroyBuffer(data, scoreBuffer);
            return nullptr;
        }
    }

    bufferBarrier(data, commandBuffer, dctBuffer, sampleBytes,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_READ_BIT);

    auto recordTransposePass = [&](VkBuffer source, VkBuffer destination,
                                   uint32_t sourceWidth, uint32_t sourceHeight) {
        TransposeParameters transpose{
            .width = sourceWidth,
            .height = sourceHeight,
        };
        data.functions->vkCmdBindPipeline(
            commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
            data.pipelines.transposePipeline);
        pushStorageDescriptors(data, commandBuffer, source, sampleBytes,
                               destination, sampleBytes);
        data.dispatch.cmdPushConstants(
            commandBuffer, data.pipelines.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
            0, sizeof(transpose), &transpose);
        data.functions->vkCmdDispatch(
            commandBuffer, static_cast<uint32_t>((sampleCount + kUnpackLocalSize - 1) / kUnpackLocalSize),
            1, 1);
    };

    recordTransposePass(dctBuffer, data.planTempBuffer.info.buffer, width, height);
    bufferBarrier(data, commandBuffer, data.planTempBuffer.info.buffer, sampleBytes,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);

    launchParameters.buffer = &data.fftHeightBuffer;
    {
        ActiveVkFFTDispatchScope active(&data.dispatch);
        VkFFTResult result = VkFFTAppend(&data.fftHeight, 0, &launchParameters);
        if (result != VKFFT_SUCCESS) {
            error = vkfftError(result);
            data.gpu->gpuExecAbandon(context);
            destroyBuffer(data, scoreBuffer);
            return nullptr;
        }
    }

    bufferBarrier(data, commandBuffer, data.planTempBuffer.info.buffer, sampleBytes,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    recordTransposePass(data.planTempBuffer.info.buffer, dctBuffer, height, width);
    bufferBarrier(data, commandBuffer, dctBuffer, sampleBytes,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_READ_BIT);

    const VkBuffer scoreSource = dctBuffer;
    auto recordScorePass = [&](uint32_t axis, uint32_t outputOffset) {
        ScoreParameters score{
            .width = width,
            .height = height,
            .axis = axis,
            .range = static_cast<uint32_t>(data.range),
            .outputOffset = outputOffset,
            .method = static_cast<uint32_t>(data.method),
        };
        data.functions->vkCmdBindPipeline(
            commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
            data.pipelines.scorePipeline);
        pushStorageDescriptors(data, commandBuffer, scoreSource, sampleBytes,
                               scoreBuffer.info.buffer, scoreBytes);
        data.dispatch.cmdPushConstants(
            commandBuffer, data.pipelines.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
            0, sizeof(score), &score);
        uint32_t length = axis == 0 ? width : height;
        data.functions->vkCmdDispatch(commandBuffer,
                                      (length + kScoreLocalSize - 1) / kScoreLocalSize,
                                      1, 1);
    };

    recordScorePass(0, 0);
    bufferBarrier(data, commandBuffer, scoreBuffer.info.buffer, scoreBytes,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_WRITE_BIT);
    recordScorePass(1, width);
    bufferBarrier(data, commandBuffer, scoreBuffer.info.buffer, scoreBytes,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_HOST_BIT,
                  VK_ACCESS_2_HOST_READ_BIT);

    std::memset(errorText, 0, sizeof(errorText));
    if (data.gpu->gpuExecSubmit(context, nullptr, errorText,
                                sizeof(errorText)) != 0) {
        error = errorText[0] ? errorText : "VapourSynth could not submit the resdet GPU work";
        data.gpu->gpuExecPoolWaitIdle(data.execPool, nullptr, 0);
        destroyBuffer(data, scoreBuffer);
        return nullptr;
    }

    std::memset(errorText, 0, sizeof(errorText));
    int drain = data.gpu->gpuExecPoolWaitIdle(
        data.execPool, errorText, sizeof(errorText));
    if (drain != gdDrained) {
        error = errorText[0] ? errorText : "The resdet GPU execution did not complete";
        destroyBuffer(data, scoreBuffer);
        return nullptr;
    }

    const float *mappedScores = static_cast<const float *>(scoreBuffer.info.mapped);
    std::vector<double> widthScores(width);
    std::vector<double> heightScores(height);
    std::transform(mappedScores, mappedScores + width, widthScores.begin(),
                   [](float value) { return static_cast<double>(value); });
    std::transform(mappedScores + width, mappedScores + width + height,
                   heightScores.begin(), [](float value) { return static_cast<double>(value); });

    const VSFrame *planeSources[3] = {input, input, input};
    const int planes[3] = {0, 1, 2};
    VSFrame *output = data.vsapi->newVideoFrame2(
        &format, data.vi.width, data.vi.height, planeSources, planes, input, data.core);
    if (!output) {
        error = "VapourSynth could not create the GPU Analyze output frame";
        destroyBuffer(data, scoreBuffer);
        return nullptr;
    }

    VSMap *properties = data.vsapi->getFramePropertiesRW(output);
    if (data.vsapi->mapSetFloatArray(properties, kWidthScoresProperty,
                                     widthScores.data(), static_cast<int>(widthScores.size())) != 0
        || data.vsapi->mapSetFloatArray(properties, kHeightScoresProperty,
                                        heightScores.data(), static_cast<int>(heightScores.size())) != 0
        || data.vsapi->mapSetData(properties, kMethodProperty,
                                  methodName(data.method),
                                  static_cast<int>(std::strlen(methodName(data.method))),
                                  dtUtf8, 0) != 0) {
        data.vsapi->freeFrame(output);
        error = "VapourSynth could not attach resdet score properties";
        destroyBuffer(data, scoreBuffer);
        return nullptr;
    }
    const int64_t bounds[2] = {data.range, static_cast<int64_t>(data.vi.width) - data.range};
    const int64_t heightBounds[2] = {data.range, static_cast<int64_t>(data.vi.height) - data.range};
    data.vsapi->mapSetIntArray(properties, "resdet_width_bounds", bounds, 2);
    data.vsapi->mapSetIntArray(properties, "resdet_height_bounds", heightBounds, 2);
    data.vsapi->mapSetInt(properties, "resdet_range", data.range, 0);

    destroyBuffer(data, scoreBuffer);
    return output;
}

static bool initializeVkFFT(AnalyzeData &data, std::string &error) {
    VkDeviceSize bufferBytes = static_cast<VkDeviceSize>(data.vi.width)
        * static_cast<VkDeviceSize>(data.vi.height) * sizeof(float);

    data.planBuffer = createBuffer(data, bufferBytes,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, error);
    if (!data.planBuffer.handle) {
        return false;
    }
    // The second buffer holds the transposed image between the two 1D passes.
    // Together, the width pass, transpose, height pass, and transpose back are
    // a 2D DCT-II while keeping each VkFFT plan on a single, well-defined axis.
    data.planTempBuffer = createBuffer(data, bufferBytes * 2,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, error);
    if (!data.planTempBuffer.handle) {
        destroyBuffer(data, data.planBuffer);
        return false;
    }

    VkCommandPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = data.handles.computeQueueFamily,
    };
    if (data.functions->vkCreateCommandPool(data.handles.device, &poolInfo,
                                            nullptr, &data.vkfftCommandPool) != VK_SUCCESS) {
        error = "Vulkan could not create the VkFFT command pool";
        destroyBuffer(data, data.planTempBuffer);
        destroyBuffer(data, data.planBuffer);
        return false;
    }
    VkFenceCreateInfo fenceInfo{
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
    };
    if (data.functions->vkCreateFence(data.handles.device, &fenceInfo,
                                      nullptr, &data.vkfftFence) != VK_SUCCESS) {
        error = "Vulkan could not create the VkFFT fence";
        data.functions->vkDestroyCommandPool(data.handles.device,
                                             data.vkfftCommandPool, nullptr);
        data.vkfftCommandPool = VK_NULL_HANDLE;
        destroyBuffer(data, data.planTempBuffer);
        destroyBuffer(data, data.planBuffer);
        return false;
    }

    VkDeviceQueueInfo2 queueInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2,
        .pNext = nullptr,
        .flags = 0,
        .queueFamilyIndex = data.handles.computeQueueFamily,
        .queueIndex = data.handles.computeQueueIndex,
    };
    data.functions->vkGetDeviceQueue2(data.handles.device, &queueInfo, &data.computeQueue);

    data.gpu->lockVulkanQueue(data.core, vqCompute);
    VkFFTResult result = VKFFT_SUCCESS;
    {
        ActiveVkFFTDispatchScope active(&data.dispatch);
        auto initializePlan = [&](VkFFTApplication &application, VkBuffer *buffer,
                                  uint64_t transformSize, uint64_t batchCount) {
            VkFFTConfiguration configuration{};
            configuration.FFTdim = 1;
            configuration.size[0] = transformSize;
            configuration.numberBatches = batchCount;
            configuration.performDCT = 2;
            configuration.device = &data.handles.device;
            configuration.physicalDevice = &data.handles.physicalDevice;
            configuration.queue = &data.computeQueue;
            configuration.commandPool = &data.vkfftCommandPool;
            configuration.fence = &data.vkfftFence;
            configuration.isCompilerInitialized = 1;
            configuration.bufferNum = 1;
            configuration.bufferSize = &data.fftBufferSize;
            configuration.buffer = buffer;
            return initializeVkFFT(&application, configuration);
        };
        data.fftBufferSize = bufferBytes;
        data.fftWidthBuffer = data.planBuffer.info.buffer;
        data.fftHeightBuffer = data.planTempBuffer.info.buffer;
        result = initializePlan(data.fftWidth, &data.fftWidthBuffer,
                                static_cast<uint64_t>(data.vi.width),
                                static_cast<uint64_t>(data.vi.height));
        if (result == VKFFT_SUCCESS) {
            data.fftWidthInitialized = true;
            result = initializePlan(data.fftHeight, &data.fftHeightBuffer,
                                    static_cast<uint64_t>(data.vi.height),
                                    static_cast<uint64_t>(data.vi.width));
            if (result == VKFFT_SUCCESS) {
                data.fftHeightInitialized = true;
            }
        }
        if (result != VKFFT_SUCCESS && data.fftWidthInitialized) {
            deleteVkFFT(&data.fftWidth);
            data.fftWidthInitialized = false;
        }
    }
    data.gpu->unlockVulkanQueue(data.core, vqCompute);
    if (result != VKFFT_SUCCESS) {
        error = vkfftError(result);
        if (data.vkfftFence) {
            data.functions->vkDestroyFence(data.handles.device, data.vkfftFence, nullptr);
            data.vkfftFence = VK_NULL_HANDLE;
        }
        if (data.vkfftCommandPool) {
            data.functions->vkDestroyCommandPool(data.handles.device,
                                                 data.vkfftCommandPool, nullptr);
            data.vkfftCommandPool = VK_NULL_HANDLE;
        }
        destroyBuffer(data, data.planTempBuffer);
        destroyBuffer(data, data.planBuffer);
        return false;
    }
    data.fftInitialized = true;
    return true;
}

static void freeAnalyzeData(AnalyzeData *data) {
    if (!data) {
        return;
    }
    if (data->execPool) {
        char error[kErrorBufferSize]{};
        data->gpu->gpuExecPoolWaitIdle(data->execPool, error, sizeof(error));
        data->gpu->freeGPUExecPool(data->execPool);
        data->execPool = nullptr;
    }
    if (data->fftInitialized) {
        ActiveVkFFTDispatchScope active(&data->dispatch);
        if (data->fftHeightInitialized) {
            deleteVkFFT(&data->fftHeight);
            data->fftHeightInitialized = false;
        }
        if (data->fftWidthInitialized) {
            deleteVkFFT(&data->fftWidth);
            data->fftWidthInitialized = false;
        }
        data->fftInitialized = false;
    }
    destroyPipelines(*data);
    destroyBuffer(*data, data->planTempBuffer);
    destroyBuffer(*data, data->planBuffer);
    if (data->vkfftFence) {
        data->functions->vkDestroyFence(data->handles.device, data->vkfftFence, nullptr);
    }
    if (data->vkfftCommandPool) {
        data->functions->vkDestroyCommandPool(data->handles.device,
                                             data->vkfftCommandPool, nullptr);
    }
    if (data->input) {
        data->vsapi->freeNode(data->input);
    }
    delete data;
}

static void VS_CC AnalyzeFree(void *instanceData, VSCore *, const VSAPI *) {
    freeAnalyzeData(static_cast<AnalyzeData *>(instanceData));
}

static const VSFrame *VS_CC AnalyzeGetFrame(
    int n, int activationReason, void *instanceData, void **,
    VSFrameContext *frameContext, VSCore *core, const VSAPI *vsapi) {
    auto *data = static_cast<AnalyzeData *>(instanceData);
    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, data->input, frameContext);
        return nullptr;
    }
    if (activationReason != arAllFramesReady) {
        return nullptr;
    }

    const VSFrame *input = vsapi->getFrameFilter(n, data->input, frameContext);
    if (!input) {
        return nullptr;
    }
    std::string error;
    const VSFrame *output = Analyze(*data, input, frameContext, error);
    if (!output) {
        vsapi->setFilterError(error.c_str(), frameContext);
    }
    vsapi->freeFrame(input);
    (void)core;
    return output;
}

static void VS_CC AnalyzeCreate(const VSMap *in, VSMap *out, void *,
                                VSCore *core, const VSAPI *vsapi) {
    int error = 0;
    VSNode *input = vsapi->mapGetNode(in, "clip", 0, &error);
    if (!input || error) {
        vsapi->mapSetError(out, "resdet.Analyze: missing clip");
        return;
    }
    const VSVideoInfo *vi = vsapi->getVideoInfo(input);
    if (!vi || vsapi->getNodeType(input) != mtVideo || !vi->format.numPlanes) {
        vsapi->freeNode(input);
        vsapi->mapSetError(out, "resdet.Analyze: clip must be a video node");
        return;
    }

    Method method;
    std::string methodError;
    if (!parseMethod(in, vsapi, method, methodError)) {
        vsapi->freeNode(input);
        vsapi->mapSetError(out, methodError.c_str());
        return;
    }

    int range = 1;
    if (vsapi->mapNumElements(in, "range") > 0) {
        range = static_cast<int>(vsapi->mapGetInt(in, "range", 0, &error));
    }
    if (error || range < 1) {
        vsapi->freeNode(input);
        vsapi->mapSetError(out, "resdet.Analyze: range must be a positive integer");
        return;
    }
    if (method == Method::ZeroCrossing) {
        range = 1;
    }

    auto data = std::make_unique<AnalyzeData>();
    data->input = input;
    data->vi = *vi;
    data->range = range;
    data->method = method;
    data->core = core;
    data->vsapi = vsapi;
    data->gpu = vsapi->getVulkanAPI();

    char errorText[kErrorBufferSize]{};
    if (!data->gpu
        || data->gpu->getVulkanHandles(core, &data->handles,
                                       errorText, sizeof(errorText)) != 0
        || !(data->functions = data->gpu->getVulkanFunctions(
                 core, errorText, sizeof(errorText)))) {
        vsapi->freeNode(input);
        vsapi->mapSetError(out, errorText[0] ? errorText : "resdet.Analyze: Vulkan is unavailable");
        return;
    }
    data->dispatch = makeDispatch(data->handles, data->functions);
    if (!dispatchIsComplete(data->dispatch)) {
        vsapi->freeNode(input);
        vsapi->mapSetError(out, "resdet.Analyze: required Vulkan entry points are unavailable");
        return;
    }

    static std::once_flag glslangInit;
    static bool glslangReady = false;
    std::call_once(glslangInit, [] { glslangReady = glslang_initialize_process() != 0; });
    if (!glslangReady) {
        vsapi->freeNode(input);
        vsapi->mapSetError(out, "resdet.Analyze: could not initialize the VkFFT shader compiler");
        return;
    }

    data->execPool = data->gpu->createGPUExecPool(
        core, vqCompute, errorText, sizeof(errorText));
    if (!data->execPool) {
        vsapi->freeNode(input);
        vsapi->mapSetError(out, errorText[0] ? errorText : "resdet.Analyze: could not create a GPU execution pool");
        return;
    }

    std::string setupError;
    if (!createPipelines(*data, setupError)
        || !initializeVkFFT(*data, setupError)) {
        std::string message = setupError.c_str();
        if (message.empty()) {
            message = "resdet.Analyze: GPU setup failed";
        }
        freeAnalyzeData(data.release());
        vsapi->mapSetError(out, message.c_str());
        return;
    }

    AnalyzeData *rawData = data.release();
    VSNode *node = vsapi->createVideoFilterEx2(
        "resdet.Analyze", vi, AnalyzeGetFrame, AnalyzeFree,
        fmParallel, ffGPUOutput, nullptr, 0, rawData, core);
    if (!node) {
        freeAnalyzeData(rawData);
        vsapi->mapSetError(out, "resdet.Analyze: could not create the GPU filter node");
        return;
    }
    vsapi->mapSetNode(out, "clip", node, 0);
    vsapi->freeNode(node);
}

} // namespace

VS_EXTERNAL_API(void) VapourSynthPluginInit2(
    VSPlugin *plugin, const VSPLUGINAPI *vspapi) {
    vspapi->configPlugin(
        "com.vs-resdet.analyze", "resdet", "A GPU-accelerated source resolution detection for upscaled images and videos.",
        VS_MAKE_VERSION(0, 1), kVapourSynthApi, 0, plugin);
    vspapi->registerFunction(
        "Analyze", "clip:vnode:gpu;method:data:opt;range:int:opt;", "clip:vnode:gpu;",
        AnalyzeCreate, nullptr, plugin);
}
