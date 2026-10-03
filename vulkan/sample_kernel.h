#pragma once
#include "core/affine.h"
#include "core/sampling.h"
#include <vulkan/vulkan.h>
#include <array>

namespace compositor {
struct alignas(16) SampleParameters {
    engine::Affine inverse;
    engine::Coordinate origin,step;
    std::array<std::int32_t,4> extent,grid;
    engine::Pixel defaultPixel;
    std::uint32_t width,height,sampling;
    float opacity;
    std::array<std::uint32_t,2> outputOffset{};
    std::array<std::uint32_t,2> padding{};
    engine::Coordinate footprint{1,1};
    std::uint32_t fp32Source=0;
    std::array<std::uint32_t,3> sourcePadding{};
    engine::Coordinate inputOrigin{};
    double inputScale=1,mappingPadding=0;
    engine::Coordinate tapOrigin{};
    void validate() const;
};
// Records FP64 inverse affine nearest/bilinear/Lanczos sampling from canonical
// RGBA16F or derived RGBA32F slots, both padded to a 256x256 grid. Direct
// Lanczos footprints are bounded to 16 source pixels per axis;
// mip selection/generation and oversized-footprint fallback belong to callers.
// Requires shaderFloat64 enabled. Caller retires each descriptor slot before
// reuse and owns parameter/table/output allocation lifetime and barriers.
class SampleKernel {
public:
    explicit SampleKernel(VkDevice device);
    ~SampleKernel();
    SampleKernel(const SampleKernel&)=delete;
    SampleKernel& operator=(const SampleKernel&)=delete;
    struct Buffer {VkBuffer handle;VkDeviceSize bytes;};
    // parametersBuffer must contain exactly the validated parameters supplied
    // here, unchanged through GPU completion. Table covers the full source grid;
    // nonresident entries must use -2, never masquerade as asset defaults (-1).
    void record(VkCommandBuffer command,std::size_t slot,Buffer pool,Buffer table,
                Buffer parametersBuffer,Buffer output,const SampleParameters& parameters);
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
