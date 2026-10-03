#include "vulkan/buffer.h"
#include <stdexcept>
#include <string>

namespace compositor {
namespace {
void checkBuffer(VkResult result,const char* operation) {
    if(result!=VK_SUCCESS) throw std::runtime_error(std::string(operation)+": "+std::to_string(result));
}
}
VulkanBuffer::VulkanBuffer(VkPhysicalDevice physical,VkDevice device,VkDeviceSize bytes,
                         VkBufferUsageFlags usage,Memory memory,std::shared_ptr<MemoryAdmission> admission):device_(device),bytes_(bytes) {
    if(!physical || !device || !bytes || !usage || (memory!=Memory::Device && memory!=Memory::Host))
        throw std::invalid_argument("Invalid Vulkan buffer allocation");
    try {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size=bytes;info.usage=usage;info.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        checkBuffer(vkCreateBuffer(device_,&info,nullptr,&buffer_),"Create buffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_,buffer_,&requirements);
        VkPhysicalDeviceMemoryProperties properties{};
        vkGetPhysicalDeviceMemoryProperties(physical,&properties);
        const VkMemoryPropertyFlags required=memory==Memory::Device ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT : VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        std::uint32_t selected=properties.memoryTypeCount;
        int best=-1;
        for(std::uint32_t i=0;i<properties.memoryTypeCount;++i) {
            const auto flags=properties.memoryTypes[i].propertyFlags;
            if(!(requirements.memoryTypeBits&(1u<<i)) || (flags&required)!=required) continue;
            // Host-cached coherent memory is preferred, but noncoherent is
            // supported. Device allocations do not map even on UMA hardware.
            const int score=memory==Memory::Host ?
                ((flags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? 2 : 0)+
                ((flags&VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? 1 : 0) : 0;
            if(score>best) {selected=i;best=score;}
        }
        if(selected==properties.memoryTypeCount) throw std::runtime_error("No compatible Vulkan buffer memory");
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize=requirements.size;allocation.memoryTypeIndex=selected;
        // Host-visible allocations are also additional GPU charges until RSS
        // overlap is proven. Include driver-required padding, not just payload.
        if(admission) memoryCharge_=admission->require(requirements.size,MemoryAdmission::Kind::Gpu);
        checkBuffer(vkAllocateMemory(device_,&allocation,nullptr,&memory_),"Allocate buffer memory");
        allocationBytes_=requirements.size;
        checkBuffer(vkBindBufferMemory(device_,buffer_,memory_,0),"Bind buffer memory");
        if(memory==Memory::Host)
            checkBuffer(vkMapMemory(device_,memory_,0,VK_WHOLE_SIZE,0,&mapped_),"Map buffer memory");
        if(memoryCharge_) memoryCharge_->commit();
    } catch(...) {release();throw;}
}
void VulkanBuffer::flush() {
    if(!mapped_) throw std::logic_error("Cannot flush unmapped buffer");
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory=memory_;range.size=VK_WHOLE_SIZE;
    checkBuffer(vkFlushMappedMemoryRanges(device_,1,&range),"Flush buffer memory");
}
void VulkanBuffer::invalidate() {
    if(!mapped_) throw std::logic_error("Cannot invalidate unmapped buffer");
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory=memory_;range.size=VK_WHOLE_SIZE;
    checkBuffer(vkInvalidateMappedMemoryRanges(device_,1,&range),"Invalidate buffer memory");
}
void VulkanBuffer::release() noexcept {
    if(mapped_) vkUnmapMemory(device_,memory_);
    if(buffer_) vkDestroyBuffer(device_,buffer_,nullptr);
    if(memory_) vkFreeMemory(device_,memory_,nullptr);
    memoryCharge_.reset();
    mapped_=nullptr;buffer_=VK_NULL_HANDLE;memory_=VK_NULL_HANDLE;
}
VulkanBuffer::~VulkanBuffer() {release();}
}
