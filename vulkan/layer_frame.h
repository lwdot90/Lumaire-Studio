#pragma once
#include "core/document.h"
#include "vulkan/layer_block.h"

namespace compositor {
// A viewport-sized linear result, not a full-canvas intermediate. The caller
// clips to the canvas and converts for display. All GPU work uses one queue.
// No waits/readback; output becomes visible only after every block copy retires.
// Owner retires BOTH timelines and external output readers before destruction.
class GpuLayerFrame {
public:
    enum class State { Idle, Pending, Complete, Cancelled, NeedsCpu };
    GpuLayerFrame(VkPhysicalDevice physical,VkDevice device,VkQueue queue,
                  std::uint32_t family,std::size_t tileBudget,VkDeviceSize frameBudget,VkDeviceSize mipBudget=64*1024*1024,
                  std::shared_ptr<MemoryAdmission> memory={});
    ~GpuLayerFrame();
    GpuLayerFrame(const GpuLayerFrame&)=delete;
    GpuLayerFrame& operator=(const GpuLayerFrame&)=delete;
    // An exact completed snapshot/grid hit preserves output without submissions.
    // As for a miss, the caller must first retire external output readers.
    // Changed snapshots at the same grid retain proven-equal 256x256 blocks;
    // cancelled/partial frames never serve as reuse sources.
    void start(engine::DocumentPtr document,engine::Coordinate origin,engine::Coordinate step,
               std::uint32_t width,std::uint32_t height,std::stop_token stop={});
    State advance();
    void cancel();
    VkBuffer output() const;
    VkDeviceSize outputBytes() const {return output_ ? output_->bytes() : 0;}
    State state() const {return state_;}
    const engine::DocumentPtr& document() const {return document_;}
    VkSemaphore timeline() const {return timeline_;}
    std::uint64_t lastSubmitted() const {return serial_;}
    VkSemaphore blockTimeline() const {return block_.timeline();}
    std::uint64_t lastBlockSubmitted() const {return block_.lastSubmitted();}
    std::size_t splitCount() const {return splits_;}
    std::size_t blockCount() const {return copied_;}
private:
    struct Rect {std::uint32_t x,y,width,height;};
    bool copyRetired() const;
    void release() noexcept;
    void copyBlock();
    VkPhysicalDevice physical_;
    VkDevice device_;
    VkQueue queue_;
    VkDeviceSize budget_;
    std::shared_ptr<MemoryAdmission> memory_;
    GpuLayerBlock block_;
    std::unique_ptr<VulkanBuffer> output_;
    VkCommandPool pool_=VK_NULL_HANDLE;
    VkCommandBuffer command_=VK_NULL_HANDLE;
    VkSemaphore timeline_=VK_NULL_HANDLE;
    std::uint64_t serial_=0;
    engine::DocumentPtr document_;
    engine::Coordinate origin_,step_;
    std::uint32_t width_=0,height_=0;
    std::vector<Rect> pending_;
    Rect active_{};
    std::stop_token stop_;
    bool rendering_=false,cancelled_=false,failed_=false;
    std::size_t splits_=0,copied_=0;
    State state_=State::Idle;
};
}
