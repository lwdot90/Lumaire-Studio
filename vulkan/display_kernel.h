#pragma once
#include "core/affine.h"
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>

namespace compositor {
struct alignas(16) DisplayParameters {
    engine::Coordinate origin,step;
    std::array<std::uint32_t,2> size,canvas;
    std::uint32_t base=0,count=0,cell=16,bgra=0;
    std::array<float,2> background;
    std::array<std::uint32_t,2> padding{};
    void validate() const;
};
// Bounded dynamic-buffer ranges allow 4K+ frames above maxStorageBufferRange.
// Both allocations must be padded to a whole 65536-pixel chunk. Updates one
// retired descriptor slot once, then records all chunks without mutation.
// Requires shaderFloat64 and nonaliasing source/output allocations. Caller owns
// barriers, submission and buffer lifetime.
class DisplayKernel {
public:
    static constexpr VkDeviceSize chunkPixels=65536;
    static VkDeviceSize paddedPixels(std::uint64_t count);
    explicit DisplayKernel(VkDevice device);
    ~DisplayKernel();
    DisplayKernel(const DisplayKernel&)=delete;
    DisplayKernel& operator=(const DisplayKernel&)=delete;
    struct Buffer {VkBuffer handle;VkDeviceSize bytes;};
    void record(VkCommandBuffer command,std::size_t slot,Buffer source,Buffer output,DisplayParameters parameters);
private:
    void release() noexcept;
    VkDevice device_;
    VkDescriptorSetLayout descriptors_=VK_NULL_HANDLE;
    VkDescriptorPool pool_=VK_NULL_HANDLE;
    VkPipelineLayout layout_=VK_NULL_HANDLE;
    VkPipeline pipeline_=VK_NULL_HANDLE;
    std::array<VkDescriptorSet,2> sets_{};
};
}
