#include "vulkan/tile_cache.h"
#include "io/spill_store.h"
#include <limits>
#include <stdexcept>
#include <string>

namespace compositor {
namespace {
void checkCache(VkResult result,const char* operation) {
    if(result!=VK_SUCCESS) throw std::runtime_error(std::string(operation)+": "+std::to_string(result));
}
}
std::size_t VulkanTileCache::checkedBudget(VkPhysicalDevice physical,std::size_t budget) {
    if(!physical) throw std::invalid_argument("Missing tile cache device");
    const auto bytes=budget/TileResidency::slotBytes*TileResidency::slotBytes;
    VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(physical,&properties);
    if(!bytes || bytes>properties.limits.maxStorageBufferRange)
        throw std::invalid_argument("Tile pool budget outside storage-buffer limits");
    return bytes;
}
VulkanTileCache::VulkanTileCache(VkPhysicalDevice physical,VkDevice device,VkQueue queue,
                               std::uint32_t queueFamily,std::size_t payloadBudget,std::shared_ptr<MemoryAdmission> memory):
    device_(device),queue_(queue),residency_(checkedBudget(physical,payloadBudget)),
    pool_(physical,device,residency_.capacity()*TileResidency::slotBytes,
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VulkanBuffer::Memory::Device,memory) {
    if(!queue) throw std::invalid_argument("Missing tile cache queue");
    try {
        for(auto& staging:staging_) staging=std::make_unique<VulkanBuffer>(physical,device,pool_.bytes(),VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VulkanBuffer::Memory::Host,memory);
        VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        info.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;info.queueFamilyIndex=queueFamily;
        checkCache(vkCreateCommandPool(device_,&info,nullptr,&commands_),"Create tile command pool");
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool=commands_;allocation.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;allocation.commandBufferCount=2;
        checkCache(vkAllocateCommandBuffers(device_,&allocation,command_.data()),"Allocate tile commands");
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};type.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};semaphore.pNext=&type;
        checkCache(vkCreateSemaphore(device_,&semaphore,nullptr,&timeline_),"Create tile timeline");
    } catch(...) {release();throw;}
}
void VulkanTileCache::release() noexcept {
    if(commands_) vkDestroyCommandPool(device_,commands_,nullptr);
    if(timeline_) vkDestroySemaphore(device_,timeline_,nullptr);
    commands_=VK_NULL_HANDLE;timeline_=VK_NULL_HANDLE;
}
VulkanTileCache::~VulkanTileCache() {release();}
void VulkanTileCache::requireActive() const {
    if(failed_ || !pending_) throw std::logic_error("No usable tile batch");
}
bool VulkanTileCache::begin() {
    if(failed_ || pending_) throw std::logic_error("Tile cache unavailable or batch already active");
    if(serial_==std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("Tile timeline exhausted");
    std::uint64_t completed=0;
    checkCache(vkGetSemaphoreCounterValue(device_,timeline_,&completed),"Poll tile timeline");
    std::size_t slot=0;
    while(slot<retirement_.size() && retirement_[slot]>completed) ++slot;
    if(slot==retirement_.size()) return false;
    checkCache(vkResetCommandBuffer(command_[slot],0),"Reset tile command");
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};beginInfo.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checkCache(vkBeginCommandBuffer(command_[slot],&beginInfo),"Begin tile command");
    residency_.begin(completed);
    active_=slot;uploads_=0;pending_=true;closed_=false;
    // Order old pool reads/writes before any overwrites in this submission.
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(command_[active_],VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    return true;
}
std::optional<TileResidency::Lease> VulkanTileCache::acquire(const engine::TilePtr& tile,std::stop_token stop) {
    requireActive();
    if(closed_) throw std::logic_error("Tile upload phase already closed");
    if(stop.stop_requested()) throw io::SpillError(io::SpillErrorCode::Cancelled,"Tile acquisition cancelled");
    auto lease=residency_.acquire(tile);
    if(lease && lease->upload) {
        auto& staging=*staging_[active_];
        const auto offset=uploads_*TileResidency::slotBytes;
        encodeTileUpload(*tile,{static_cast<std::uint8_t*>(staging.mapped())+offset,TileResidency::slotBytes},stop);
        recordTileUpload(command_[active_],{staging.handle(),staging.bytes()},offset,{pool_.handle(),pool_.bytes()},lease->slot);
        ++uploads_;
    }
    return lease;
}
VkCommandBuffer VulkanTileCache::consumers() {
    requireActive();
    if(!closed_) {
        if(uploads_) staging_[active_]->flush();
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(command_[active_],VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        closed_=true;
    }
    return command_[active_];
}
std::uint64_t VulkanTileCache::submit() {
    requireActive();
    consumers();
    checkCache(vkEndCommandBuffer(command_[active_]),"End tile command");
    const auto next=serial_+1;
    VkTimelineSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    signal.signalSemaphoreValueCount=1;signal.pSignalSemaphoreValues=&next;
    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};submitInfo.pNext=&signal;
    submitInfo.commandBufferCount=1;submitInfo.pCommandBuffers=&command_[active_];
    submitInfo.signalSemaphoreCount=1;submitInfo.pSignalSemaphores=&timeline_;
    const auto result=vkQueueSubmit(queue_,1,&submitInfo,VK_NULL_HANDLE);
    if(result!=VK_SUCCESS) {failed_=true;checkCache(result,"Submit tile command");}
    residency_.submitted(next);retirement_[active_]=next;serial_=next;pending_=false;
    return next;
}
void VulkanTileCache::cancel() {
    requireActive();
    checkCache(vkResetCommandBuffer(command_[active_],0),"Cancel tile command");
    residency_.cancel();pending_=false;
}
VkDeviceSize VulkanTileCache::allocationBytes() const {
    return pool_.allocationBytes()+staging_[0]->allocationBytes()+staging_[1]->allocationBytes();
}
}
