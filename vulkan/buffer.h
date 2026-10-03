#pragma once
#include "core/memory_admission.h"
#include <cstdint>
#include <vulkan/vulkan.h>

namespace compositor {
// Render-worker-owned allocation. Destruction does not wait: owner must retire
// every queue use before releasing it and keep the device alive. Not copyable.
class VulkanBuffer {
public:
    enum class Memory { Device, Host };
    VulkanBuffer(VkPhysicalDevice physical,VkDevice device,VkDeviceSize bytes,
                 VkBufferUsageFlags usage,Memory memory,std::shared_ptr<MemoryAdmission> admission={});
    ~VulkanBuffer();
    VulkanBuffer(const VulkanBuffer&)=delete;
    VulkanBuffer& operator=(const VulkanBuffer&)=delete;
    VkBuffer handle() const { return buffer_; }
    VkDeviceSize bytes() const { return bytes_; }
    VkDeviceSize allocationBytes() const { return allocationBytes_; }
    void* mapped() const { return mapped_; }
    // Whole-allocation operations are valid for noncoherent memory, including
    // a final partial atom. Caller synchronizes host writes/reads with GPU use.
    void flush();
    void invalidate();
private:
    void release() noexcept;
    VkDevice device_;
    VkBuffer buffer_=VK_NULL_HANDLE;
    VkDeviceMemory memory_=VK_NULL_HANDLE;
    VkDeviceSize bytes_,allocationBytes_=0;
    void* mapped_=nullptr;
    std::optional<MemoryAdmission::Reservation> memoryCharge_;
};
}
