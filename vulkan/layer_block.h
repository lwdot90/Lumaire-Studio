#pragma once
#include "core/layer_stack.h"
#include "vulkan/sample_kernel.h"
#include "vulkan/blend_kernel.h"
#include "vulkan/tile_cache.h"
#include "vulkan/mip_cache.h"

namespace compositor {
// Worker-owned asynchronous linear-RGBA block renderer. No pixel readback or
// waits; caller polls advance(). At most two layer submissions are in flight,
// with separately retired parameter/descriptor slots. Owner retires GPU work before destruction and all
// external output readers before starting another block. Requires shaderFloat64.
class GpuLayerBlock {
public:
    enum class State { Idle, Pending, Complete, NeedsSplit, Cancelled };
    GpuLayerBlock(VkPhysicalDevice physical,VkDevice device,VkQueue queue,
                  std::uint32_t family,std::size_t tileBudget,VkDeviceSize mipBudget=64*1024*1024,
                  std::shared_ptr<MemoryAdmission> memory={});
    void start(const engine::LayerStack& stack,engine::Coordinate origin,
               engine::Coordinate step,std::uint32_t width,std::uint32_t height,
               std::uint32_t offsetX=0,std::uint32_t offsetY=0,std::stop_token stop={});
    State advance();
    void cancel();
    State state() const { return state_; }
    VkBuffer output() const;
    VkSemaphore timeline() const { return cache_.timeline(); }
    std::uint64_t lastSubmitted() const { return cache_.lastSubmitted(); }
    std::size_t layerCount() const { return layers_.size(); }
    std::size_t uploads() const { return uploads_; }
private:
    State advanceOne();
    bool retired() const;
    VkPhysicalDevice physical_;
    VkDevice device_;
    VkDeviceSize mipBudget_;
    std::shared_ptr<MemoryAdmission> memory_;
    VulkanTileCache cache_;
    SampleKernel sampler_;
    BlendKernel blender_;
    VulkanBuffer sample_,first_,second_;
    std::array<std::unique_ptr<VulkanBuffer>,2> parameters_,table_;
    std::array<std::uint64_t,2> retirement_{};
    std::unique_ptr<GpuMipCache> mips_;
    std::optional<std::size_t> mipLayer_;
    std::vector<engine::LayerStack::Prepared> layers_;
    std::vector<engine::ReductionPlan> reductions_;
    engine::SampleRegion bounds_{};
    engine::Coordinate origin_,step_;
    std::uint32_t width_=0,height_=0;
    std::array<std::uint32_t,2> offset_{};
    std::size_t next_=0,uploads_=0;
    std::uint64_t serial_=0;
    std::stop_token stop_;
    bool initialized_=false,current_=false,cancelled_=false;
    State state_=State::Idle;
};
}
