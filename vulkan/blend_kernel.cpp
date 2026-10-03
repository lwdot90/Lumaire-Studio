#include "vulkan/blend_kernel.h"
#include "blend_spv.h"
#include "core/hard_mix_thresholds.h"
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

namespace compositor {
namespace {
// Push constants use the append-only shader dispatch IDs, not persisted names.
static_assert(engine::blendModeCount==24);
static_assert(static_cast<unsigned>(engine::BlendMode::LinearBurn)==13);
static_assert(static_cast<unsigned>(engine::BlendMode::LinearDodge)==14);
static_assert(static_cast<unsigned>(engine::BlendMode::SoftLight)==15);
static_assert(static_cast<unsigned>(engine::BlendMode::HardLight)==16);
static_assert(static_cast<unsigned>(engine::BlendMode::VividLight)==17);
static_assert(static_cast<unsigned>(engine::BlendMode::LinearLight)==18);
static_assert(static_cast<unsigned>(engine::BlendMode::PinLight)==19);
static_assert(static_cast<unsigned>(engine::BlendMode::HardMix)==20);
static_assert(static_cast<unsigned>(engine::BlendMode::Exclusion)==21);
static_assert(static_cast<unsigned>(engine::BlendMode::Subtract)==22);
static_assert(static_cast<unsigned>(engine::BlendMode::Divide)==23);
void check(VkResult result,const char* operation) {
    if(result!=VK_SUCCESS) throw std::runtime_error(std::string(operation)+": VkResult "+std::to_string(result));
}
}
BlendKernel::BlendKernel(VkPhysicalDevice physical,VkDevice device,std::shared_ptr<MemoryAdmission> memory)
    :device_(device),thresholds_(physical,device,sizeof(engine::hardMixThresholdBits),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VulkanBuffer::Memory::Host,std::move(memory)) {
    std::memcpy(thresholds_.mapped(),engine::hardMixThresholdBits.data(),sizeof(engine::hardMixThresholdBits));
    thresholds_.flush();
    if(!device_) throw std::invalid_argument("Blend kernel requires a device");
    VkShaderModule shader=VK_NULL_HANDLE;
    try {
        std::array<VkDescriptorSetLayoutBinding,4> bindings{};
        for(std::uint32_t i=0;i<bindings.size();++i) bindings[i]={i,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo descriptors{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        descriptors.bindingCount=static_cast<std::uint32_t>(bindings.size()); descriptors.pBindings=bindings.data();
        check(vkCreateDescriptorSetLayout(device_,&descriptors,nullptr,&descriptors_),"Blend descriptor layout");
        const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,8};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout.setLayoutCount=1; layout.pSetLayouts=&descriptors_; layout.pushConstantRangeCount=1; layout.pPushConstantRanges=&range;
        check(vkCreatePipelineLayout(device_,&layout,nullptr,&layout_),"Blend pipeline layout");
        VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; module.codeSize=sizeof(compositorBlendSpv); module.pCode=compositorBlendSpv;
        check(vkCreateShaderModule(device_,&module,nullptr,&shader),"Blend shader");
        VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; pipeline.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;
        pipeline.stage.module=shader; pipeline.stage.pName="main"; pipeline.layout=layout_;
        check(vkCreateComputePipelines(device_,VK_NULL_HANDLE,1,&pipeline,nullptr,&pipeline_),"Blend compute pipeline");
        vkDestroyShaderModule(device_,shader,nullptr); shader=VK_NULL_HANDLE;
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,8};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets=2; pool.poolSizeCount=1; pool.pPoolSizes=&size;
        check(vkCreateDescriptorPool(device_,&pool,nullptr,&pool_),"Blend descriptor pool");
        const std::array<VkDescriptorSetLayout,2> layouts{descriptors_,descriptors_};
        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocation.descriptorPool=pool_;
        allocation.descriptorSetCount=2; allocation.pSetLayouts=layouts.data();
        check(vkAllocateDescriptorSets(device_,&allocation,sets_.data()),"Blend descriptor sets");
    } catch(...) { if(shader) vkDestroyShaderModule(device_,shader,nullptr); release(); throw; }
}
BlendKernel::~BlendKernel() { release(); }
void BlendKernel::release() noexcept {
    if(pool_) vkDestroyDescriptorPool(device_,pool_,nullptr);
    if(pipeline_) vkDestroyPipeline(device_,pipeline_,nullptr);
    if(layout_) vkDestroyPipelineLayout(device_,layout_,nullptr);
    if(descriptors_) vkDestroyDescriptorSetLayout(device_,descriptors_,nullptr);
}
void BlendKernel::record(VkCommandBuffer command,std::size_t slot,Buffer source,Buffer backdrop,Buffer output,
                         std::uint32_t count,engine::BlendMode mode) {
    engine::blendIdentifier(mode);
    if(slot>=sets_.size() || !command) throw std::invalid_argument("Invalid blend command or slot");
    if(count==0) return;
    // At most one canonical 256² tile per dispatch. This is below Vulkan's
    // required minimum storage-buffer range and x workgroup-count limits.
    if(count>256*256) throw std::length_error("Blend dispatch exceeds one tile");
    if(source.handle==backdrop.handle || source.handle==output.handle || backdrop.handle==output.handle)
        throw std::invalid_argument("Blend buffers must not alias");
    const VkDeviceSize bytes=VkDeviceSize(count)*sizeof(engine::Pixel);
    static_assert(sizeof(engine::Pixel)==16);
    const std::array<Buffer,4> buffers{source,backdrop,output,{thresholds_.handle(),thresholds_.bytes()}};
    std::array<VkDescriptorBufferInfo,4> info{}; std::array<VkWriteDescriptorSet,4> writes{};
    for(std::size_t i=0;i<buffers.size();++i) {
        if(!buffers[i].handle || buffers[i].bytes<(i==3 ? thresholds_.bytes() : bytes)) throw std::invalid_argument("Blend buffer is too small");
        info[i]={buffers[i].handle,0,i==3 ? thresholds_.bytes() : bytes}; writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet=sets_[slot]; writes[i].dstBinding=static_cast<std::uint32_t>(i); writes[i].descriptorCount=1;
        writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[i].pBufferInfo=&info[i];
    }
    vkUpdateDescriptorSets(device_,4,writes.data(),0,nullptr);
    const std::array<std::uint32_t,2> parameters{count,static_cast<std::uint32_t>(mode)};
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout_,0,1,&sets_[slot],0,nullptr);
    vkCmdPushConstants(command,layout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(parameters),parameters.data());
    vkCmdDispatch(command,(count+63)/64,1,1);
}
}
