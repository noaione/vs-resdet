#pragma once

#include <VapourSynth4.h>
#include <VSVulkan4.h>

#include <vulkan/vulkan_core.h>

/* VkFFT is header-only and was written against Vulkan's linked prototypes.
 * VSVulkan4 intentionally exposes no linked Vulkan entry points, so redirect
 * every Vulkan call made by VkFFT through this per-thread dispatch object. */
struct VkFFTDispatch {
    const VSVulkanFunctions *core = nullptr;

    PFN_vkAllocateCommandBuffers allocateCommandBuffers = nullptr;
    PFN_vkAllocateDescriptorSets allocateDescriptorSets = nullptr;
    PFN_vkAllocateMemory allocateMemory = nullptr;
    PFN_vkBeginCommandBuffer beginCommandBuffer = nullptr;
    PFN_vkGetPhysicalDeviceProperties getPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties getPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkGetBufferMemoryRequirements getBufferMemoryRequirements = nullptr;
    PFN_vkBindBufferMemory bindBufferMemory = nullptr;
    PFN_vkCmdBindDescriptorSets cmdBindDescriptorSets = nullptr;
    PFN_vkCmdBindPipeline cmdBindPipeline = nullptr;
    PFN_vkCmdCopyBuffer cmdCopyBuffer = nullptr;
    PFN_vkCmdDispatch cmdDispatch = nullptr;
    PFN_vkCmdPipelineBarrier cmdPipelineBarrier = nullptr;
    PFN_vkCmdPushConstants cmdPushConstants = nullptr;
    PFN_vkCmdPushDescriptorSetKHR cmdPushDescriptorSetKHR = nullptr;
    PFN_vkCreateBuffer createBuffer = nullptr;
    PFN_vkCreateCommandPool createCommandPool = nullptr;
    PFN_vkCreateComputePipelines createComputePipelines = nullptr;
    PFN_vkCreateDescriptorPool createDescriptorPool = nullptr;
    PFN_vkCreateDescriptorSetLayout createDescriptorSetLayout = nullptr;
    PFN_vkCreateDevice createDevice = nullptr;
    PFN_vkCreateFence createFence = nullptr;
    PFN_vkCreatePipelineLayout createPipelineLayout = nullptr;
    PFN_vkCreateShaderModule createShaderModule = nullptr;
    PFN_vkDestroyBuffer destroyBuffer = nullptr;
    PFN_vkDestroyDescriptorPool destroyDescriptorPool = nullptr;
    PFN_vkDestroyDescriptorSetLayout destroyDescriptorSetLayout = nullptr;
    PFN_vkDestroyPipeline destroyPipeline = nullptr;
    PFN_vkDestroyPipelineLayout destroyPipelineLayout = nullptr;
    PFN_vkDestroyShaderModule destroyShaderModule = nullptr;
    PFN_vkEndCommandBuffer endCommandBuffer = nullptr;
    PFN_vkEnumeratePhysicalDevices enumeratePhysicalDevices = nullptr;
    PFN_vkFreeCommandBuffers freeCommandBuffers = nullptr;
    PFN_vkFreeMemory freeMemory = nullptr;
    PFN_vkGetDeviceProcAddr getDeviceProcAddr = nullptr;
    PFN_vkGetDeviceQueue getDeviceQueue = nullptr;
    PFN_vkMapMemory mapMemory = nullptr;
    PFN_vkUnmapMemory unmapMemory = nullptr;
    PFN_vkQueueSubmit queueSubmit = nullptr;
    PFN_vkWaitForFences waitForFences = nullptr;
    PFN_vkResetFences resetFences = nullptr;
    PFN_vkUpdateDescriptorSets updateDescriptorSets = nullptr;
};

inline thread_local VkFFTDispatch *activeVkFFTDispatch = nullptr;

#define vkGetPhysicalDeviceProperties(...) activeVkFFTDispatch->getPhysicalDeviceProperties(__VA_ARGS__)
#define vkGetPhysicalDeviceMemoryProperties(...) activeVkFFTDispatch->getPhysicalDeviceMemoryProperties(__VA_ARGS__)
#define vkAllocateCommandBuffers(...) activeVkFFTDispatch->allocateCommandBuffers(__VA_ARGS__)
#define vkAllocateDescriptorSets(...) activeVkFFTDispatch->allocateDescriptorSets(__VA_ARGS__)
#define vkAllocateMemory(...) activeVkFFTDispatch->allocateMemory(__VA_ARGS__)
#define vkBeginCommandBuffer(...) activeVkFFTDispatch->beginCommandBuffer(__VA_ARGS__)
#define vkGetBufferMemoryRequirements(...) activeVkFFTDispatch->getBufferMemoryRequirements(__VA_ARGS__)
#define vkBindBufferMemory(...) activeVkFFTDispatch->bindBufferMemory(__VA_ARGS__)
#define vkCmdBindDescriptorSets(...) activeVkFFTDispatch->cmdBindDescriptorSets(__VA_ARGS__)
#define vkCmdBindPipeline(...) activeVkFFTDispatch->cmdBindPipeline(__VA_ARGS__)
#define vkCmdCopyBuffer(...) activeVkFFTDispatch->cmdCopyBuffer(__VA_ARGS__)
#define vkCmdDispatch(...) activeVkFFTDispatch->cmdDispatch(__VA_ARGS__)
#define vkMapMemory(...) activeVkFFTDispatch->mapMemory(__VA_ARGS__)
#define vkUnmapMemory(...) activeVkFFTDispatch->unmapMemory(__VA_ARGS__)
#define vkQueueSubmit(...) activeVkFFTDispatch->queueSubmit(__VA_ARGS__)
#define vkWaitForFences(...) activeVkFFTDispatch->waitForFences(__VA_ARGS__)
#define vkResetFences(...) activeVkFFTDispatch->resetFences(__VA_ARGS__)
#define vkCmdPipelineBarrier(...) activeVkFFTDispatch->cmdPipelineBarrier(__VA_ARGS__)
#define vkCmdPushConstants(...) activeVkFFTDispatch->cmdPushConstants(__VA_ARGS__)
#define vkCmdPushDescriptorSetKHR(...) activeVkFFTDispatch->cmdPushDescriptorSetKHR(__VA_ARGS__)
#define vkCreateBuffer(...) activeVkFFTDispatch->createBuffer(__VA_ARGS__)
#define vkCreateCommandPool(...) activeVkFFTDispatch->createCommandPool(__VA_ARGS__)
#define vkCreateComputePipelines(...) activeVkFFTDispatch->createComputePipelines(__VA_ARGS__)
#define vkCreateDescriptorPool(...) activeVkFFTDispatch->createDescriptorPool(__VA_ARGS__)
#define vkCreateDescriptorSetLayout(...) activeVkFFTDispatch->createDescriptorSetLayout(__VA_ARGS__)
#define vkCreateDevice(...) activeVkFFTDispatch->createDevice(__VA_ARGS__)
#define vkCreateFence(...) activeVkFFTDispatch->createFence(__VA_ARGS__)
#define vkCreatePipelineLayout(...) activeVkFFTDispatch->createPipelineLayout(__VA_ARGS__)
#define vkCreateShaderModule(...) activeVkFFTDispatch->createShaderModule(__VA_ARGS__)
#define vkDestroyBuffer(...) activeVkFFTDispatch->destroyBuffer(__VA_ARGS__)
#define vkDestroyDescriptorPool(...) activeVkFFTDispatch->destroyDescriptorPool(__VA_ARGS__)
#define vkDestroyDescriptorSetLayout(...) activeVkFFTDispatch->destroyDescriptorSetLayout(__VA_ARGS__)
#define vkDestroyPipeline(...) activeVkFFTDispatch->destroyPipeline(__VA_ARGS__)
#define vkDestroyPipelineLayout(...) activeVkFFTDispatch->destroyPipelineLayout(__VA_ARGS__)
#define vkDestroyShaderModule(...) activeVkFFTDispatch->destroyShaderModule(__VA_ARGS__)
#define vkEndCommandBuffer(...) activeVkFFTDispatch->endCommandBuffer(__VA_ARGS__)
#define vkEnumeratePhysicalDevices(...) activeVkFFTDispatch->enumeratePhysicalDevices(__VA_ARGS__)
#define vkFreeCommandBuffers(...) activeVkFFTDispatch->freeCommandBuffers(__VA_ARGS__)
#define vkFreeMemory(...) activeVkFFTDispatch->freeMemory(__VA_ARGS__)
#define vkGetDeviceProcAddr(...) activeVkFFTDispatch->getDeviceProcAddr(__VA_ARGS__)
#define vkGetDeviceQueue(...) activeVkFFTDispatch->getDeviceQueue(__VA_ARGS__)
#define vkUpdateDescriptorSets(...) activeVkFFTDispatch->updateDescriptorSets(__VA_ARGS__)

#ifndef VKFFT_BACKEND
#define VKFFT_BACKEND 0
#endif
#include "vkFFT.h"

#undef vkGetPhysicalDeviceProperties
#undef vkGetPhysicalDeviceMemoryProperties
#undef vkAllocateCommandBuffers
#undef vkAllocateDescriptorSets
#undef vkAllocateMemory
#undef vkBeginCommandBuffer
#undef vkGetBufferMemoryRequirements
#undef vkBindBufferMemory
#undef vkCmdBindDescriptorSets
#undef vkCmdBindPipeline
#undef vkCmdCopyBuffer
#undef vkCmdDispatch
#undef vkMapMemory
#undef vkUnmapMemory
#undef vkQueueSubmit
#undef vkWaitForFences
#undef vkResetFences
#undef vkCmdPipelineBarrier
#undef vkCmdPushConstants
#undef vkCmdPushDescriptorSetKHR
#undef vkCreateBuffer
#undef vkCreateCommandPool
#undef vkCreateComputePipelines
#undef vkCreateDescriptorPool
#undef vkCreateDescriptorSetLayout
#undef vkCreateDevice
#undef vkCreateFence
#undef vkCreatePipelineLayout
#undef vkCreateShaderModule
#undef vkDestroyBuffer
#undef vkDestroyDescriptorPool
#undef vkDestroyDescriptorSetLayout
#undef vkDestroyPipeline
#undef vkDestroyPipelineLayout
#undef vkDestroyShaderModule
#undef vkEndCommandBuffer
#undef vkEnumeratePhysicalDevices
#undef vkFreeCommandBuffers
#undef vkFreeMemory
#undef vkGetDeviceProcAddr
#undef vkGetDeviceQueue
#undef vkUpdateDescriptorSets

struct ActiveVkFFTDispatchScope {
    VkFFTDispatch *previous;

    explicit ActiveVkFFTDispatchScope(VkFFTDispatch *dispatch)
        : previous(activeVkFFTDispatch) {
        activeVkFFTDispatch = dispatch;
    }

    ~ActiveVkFFTDispatchScope() {
        activeVkFFTDispatch = previous;
    }
};
