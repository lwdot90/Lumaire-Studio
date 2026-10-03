#pragma once
#include <vulkan/vulkan.h>
#include "core/blend.h"
#include "vulkan/buffer.h"
#include <array>

namespace compositor {
// Worker-owned pipeline. Records only: never submits, waits, reads back or
// mutates document state. Caller owns device/queue lifetime and synchronization.
class BlendKernel {
public:
    BlendKernel(VkPhysicalDevice physical,VkDevice device,std::shared_ptr<MemoryAdmission> memory={});
    ~BlendKernel();
    BlendKernel(const BlendKernel&)=delete;
    BlendKernel& operator=(const BlendKernel&)=delete;
    struct Buffer { VkBuffer handle; VkDeviceSize bytes; };
    // Each slot may be updated only after its prior GPU use has retired. Three
    // nonaliasing buffers contain count FP32 premultiplied-linear vec4 pixels.
    // Caller supplies input-ready/output-consumer barriers and dispatch limits.
    void record(VkCommandBuffer command,std::size_t slot,Buffer source,Buffer backdrop,Buffer output,
                std::uint32_t count,engine::BlendMode mode);
private:
    void release() noexcept;
    VkDevice device_;
    VulkanBuffer thresholds_;
    VkDescriptorSetLayout descriptors_=VK_NULL_HANDLE;
    VkDescriptorPool pool_=VK_NULL_HANDLE;
    VkPipelineLayout layout_=VK_NULL_HANDLE;
    VkPipeline pipeline_=VK_NULL_HANDLE;
    std::array<VkDescriptorSet,2> sets_{};
};
}
