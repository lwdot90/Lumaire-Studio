#pragma once
#include "vulkan/buffer.h"
#include "vulkan/tile_upload.h"
#include <array>
#include <memory>

namespace compositor {
// One render worker / one ordered queue. Owns a device tile pool, two staging
// allocations and two command buffers. Never waits or reads pixels back.
// Owner MUST retire lastSubmitted() before destruction, including error paths,
// and keep the device alive. Queue submission must be externally serialized.
// Timeline feature must be enabled; queueFamily must match queue and support
// compute. After submission failure, use device/queue teardown retirement, not
// the last successful timeline alone. Consumers may only read pool storage.
class VulkanTileCache {
public:
    VulkanTileCache(VkPhysicalDevice physical,VkDevice device,VkQueue queue,
                    std::uint32_t queueFamily,std::size_t payloadBudget,std::shared_ptr<MemoryAdmission> memory={});
    ~VulkanTileCache();
    VulkanTileCache(const VulkanTileCache&)=delete;
    VulkanTileCache& operator=(const VulkanTileCache&)=delete;
    // Polls the timeline; false means both staging/command slots are busy.
    bool begin();
    // A failed/cancelled read requires cancel() of this unsubmitted batch.
    std::optional<TileResidency::Lease> acquire(const engine::TilePtr& tile,std::stop_token stop={});
    // Closes upload recording and returns the command for read-only consumers
    // of leased pool slots (compute or transfer). No further acquire is allowed
    // in this batch. Caller must not end/submit/reset this command independently.
    VkCommandBuffer consumers();
    // Ends and submits that command. Signals after uploads AND consumers.
    std::uint64_t submit();
    void cancel();
    VkBuffer buffer() const { return pool_.handle(); }
    VkDeviceSize bytes() const { return pool_.bytes(); }
    VkDeviceSize allocationBytes() const;
    VkSemaphore timeline() const { return timeline_; }
    std::uint64_t lastSubmitted() const { return serial_; }
    std::size_t uploadCount() const { return uploads_; }
private:
    static std::size_t checkedBudget(VkPhysicalDevice physical,std::size_t budget);
    void release() noexcept;
    void requireActive() const;
    VkDevice device_;
    VkQueue queue_;
    TileResidency residency_;
    VulkanBuffer pool_;
    std::array<std::unique_ptr<VulkanBuffer>,2> staging_;
    VkCommandPool commands_=VK_NULL_HANDLE;
    std::array<VkCommandBuffer,2> command_{};
    VkSemaphore timeline_=VK_NULL_HANDLE;
    std::array<std::uint64_t,2> retirement_{};
    std::uint64_t serial_=0;
    std::size_t active_=0,uploads_=0;
    bool pending_=false,closed_=false,failed_=false;
};
}
