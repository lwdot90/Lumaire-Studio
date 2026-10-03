#pragma once
#include "core/raster.h"
#include "vulkan/sample_kernel.h"
#include "vulkan/tile_cache.h"

namespace compositor {
// One worker, one shared upload/consumer timeline. Derived pixels stay FP32 on
// the device. No waits/readback. Caller must retire all timeline uses before
// destruction; external mip readers must submit through the same tile cache.
class GpuMipCache {
public:
    static constexpr VkDeviceSize slotBytes=256*256*sizeof(engine::Pixel);
    enum class State {Idle,Pending,Complete,NeedsSpace,Cancelled};
    GpuMipCache(VkPhysicalDevice physical,VkDevice device,VulkanTileCache& uploads,VkDeviceSize budget,
                std::shared_ptr<MemoryAdmission> memory={});
    GpuMipCache(const GpuMipCache&)=delete;
    GpuMipCache& operator=(const GpuMipCache&)=delete;
    void start(std::shared_ptr<const engine::RasterSnapshot> source,unsigned level,std::vector<engine::TileCoord> pieces,std::stop_token stop={});
    State advance();
    void cancel();
    State state() const {return state_;}
    std::size_t lease(engine::TileCoord coordinate) const;
    VkBuffer buffer() const {return pool_.handle();}
    VkDeviceSize bytes() const {return pool_.bytes();}
    std::size_t generatedPieces() const {return generated_;}
    std::size_t residentPieces() const;
private:
    struct Key {
        const engine::RasterSnapshot* source;
        unsigned level;
        engine::TileCoord coordinate;
        bool operator==(const Key&) const = default;
    };
    struct Entry {
        std::optional<Key> key;
        std::shared_ptr<const engine::RasterSnapshot> source;
        std::uint64_t lastUse=0;
        bool complete=false;
    };
    struct Task {Key key;std::size_t slot;unsigned next=0;std::vector<Key> parents;};
    static VkDeviceSize checkedBudget(VkPhysicalDevice physical,VkDeviceSize budget);
    bool retired() const;
    bool pinned(const Key& key) const;
    std::optional<std::size_t> find(const Key& key) const;
    std::optional<std::size_t> allocate(const Key& key);
    engine::Extent extent(unsigned level) const;
    void discardPartial();
    void record(Task& task,engine::Extent area,engine::Extent parent);
    VkDevice device_;
    VulkanTileCache& uploads_;
    SampleKernel sampler_;
    VulkanBuffer pool_,scratch_,parameters_,table_;
    std::vector<Entry> entries_;
    std::shared_ptr<const engine::RasterSnapshot> source_;
    std::vector<Key> requested_;
    std::vector<Task> tasks_;
    std::uint64_t clock_=0;
    std::size_t generated_=0;
    State state_=State::Idle;
    std::stop_token stop_;
    bool cancelled_=false,failed_=false;
};
}
