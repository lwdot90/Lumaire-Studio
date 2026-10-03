#include "vulkan/display_kernel.h"
#include "display_spv.h"
#include <cstddef>
#include <stdexcept>
#include <string>

namespace compositor {
static_assert(sizeof(DisplayParameters)==80 && offsetof(DisplayParameters,size)==32 && offsetof(DisplayParameters,background)==64);
void DisplayParameters::validate() const {
    if(size[0]==0 || size[1]==0 || size[0]>32768 || size[1]>32768 || std::uint64_t(size[0])*size[1]>64*1024*1024 ||
       canvas[0]==0 || canvas[1]==0 || canvas[0]>30000 || canvas[1]>30000 || cell==0 || cell>32768 || bgra>1)
        throw std::invalid_argument("Invalid display dimensions or packing");
    for(auto value:{origin.x,origin.y,step.x,step.y}) if(!std::isfinite(value)) throw std::invalid_argument("Invalid display geometry");
    if(step.x<=0 || step.y<=0 || !std::isfinite(origin.x+(size[0]-1)*step.x) || !std::isfinite(origin.y+(size[1]-1)*step.y))
        throw std::invalid_argument("Display geometry overflow");
    for(auto value:background) if(!std::isfinite(value) || value<0 || value>1) throw std::invalid_argument("Invalid checker color");
}
VkDeviceSize DisplayKernel::paddedPixels(std::uint64_t count) {
    if(count==0 || count>64*1024*1024) throw std::length_error("Display allocation exceeds viewport limits");
    return (count+chunkPixels-1)/chunkPixels*chunkPixels;
}
namespace {
void checkDisplay(VkResult result,const char* operation) {
    if(result!=VK_SUCCESS) throw std::runtime_error(std::string(operation)+": "+std::to_string(result));
}
}
DisplayKernel::DisplayKernel(VkDevice device):device_(device) {
    if(!device) throw std::invalid_argument("Missing display device");
    VkShaderModule shader=VK_NULL_HANDLE;
    try {
        std::array<VkDescriptorSetLayoutBinding,2> bindings{};
        for(std::uint32_t i=0;i<2;++i) bindings[i]={i,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo descriptors{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};descriptors.bindingCount=2;descriptors.pBindings=bindings.data();
        checkDisplay(vkCreateDescriptorSetLayout(device_,&descriptors,nullptr,&descriptors_),"Display descriptor layout");
        const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(DisplayParameters)};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};layout.setLayoutCount=1;layout.pSetLayouts=&descriptors_;layout.pushConstantRangeCount=1;layout.pPushConstantRanges=&range;
        checkDisplay(vkCreatePipelineLayout(device_,&layout,nullptr,&layout_),"Display pipeline layout");
        VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};module.codeSize=sizeof(compositorDisplaySpv);module.pCode=compositorDisplaySpv;
        checkDisplay(vkCreateShaderModule(device_,&module,nullptr,&shader),"Display shader");
        VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pipeline.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};pipeline.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;
        pipeline.stage.module=shader;pipeline.stage.pName="main";pipeline.layout=layout_;
        checkDisplay(vkCreateComputePipelines(device_,VK_NULL_HANDLE,1,&pipeline,nullptr,&pipeline_),"Display pipeline");
        vkDestroyShaderModule(device_,shader,nullptr);shader=VK_NULL_HANDLE;
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,4};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=2;pool.poolSizeCount=1;pool.pPoolSizes=&size;
        checkDisplay(vkCreateDescriptorPool(device_,&pool,nullptr,&pool_),"Display descriptor pool");
        const std::array<VkDescriptorSetLayout,2> layouts{descriptors_,descriptors_};
        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};allocation.descriptorPool=pool_;allocation.descriptorSetCount=2;allocation.pSetLayouts=layouts.data();
        checkDisplay(vkAllocateDescriptorSets(device_,&allocation,sets_.data()),"Display descriptor sets");
    } catch(...) {if(shader) vkDestroyShaderModule(device_,shader,nullptr);release();throw;}
}
void DisplayKernel::release() noexcept {
    if(pool_) vkDestroyDescriptorPool(device_,pool_,nullptr);
    if(pipeline_) vkDestroyPipeline(device_,pipeline_,nullptr);
    if(layout_) vkDestroyPipelineLayout(device_,layout_,nullptr);
    if(descriptors_) vkDestroyDescriptorSetLayout(device_,descriptors_,nullptr);
}
DisplayKernel::~DisplayKernel(){release();}
void DisplayKernel::record(VkCommandBuffer command,std::size_t slot,Buffer source,Buffer output,DisplayParameters parameters) {
    parameters.validate();const auto count=std::uint64_t(parameters.size[0])*parameters.size[1],padded=paddedPixels(count);
    if(!command || slot>=sets_.size() || !source.handle || !output.handle || source.handle==output.handle || source.bytes<padded*16 || output.bytes<padded*4)
        throw std::invalid_argument("Invalid display buffers or descriptor slot");
    const std::array<VkDescriptorBufferInfo,2> info{{{source.handle,0,chunkPixels*16},{output.handle,0,chunkPixels*4}}};
    std::array<VkWriteDescriptorSet,2> writes{};
    for(std::size_t i=0;i<2;++i) {writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=sets_[slot];writes[i].dstBinding=static_cast<std::uint32_t>(i);writes[i].descriptorCount=1;
        writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;writes[i].pBufferInfo=&info[i];}
    vkUpdateDescriptorSets(device_,2,writes.data(),0,nullptr);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_);
    for(std::uint64_t base=0;base<count;base+=chunkPixels) {
        parameters.base=static_cast<std::uint32_t>(base);parameters.count=static_cast<std::uint32_t>(std::min(chunkPixels,count-base));
        const std::array<std::uint32_t,2> offsets{static_cast<std::uint32_t>(base*16),static_cast<std::uint32_t>(base*4)};
        vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout_,0,1,&sets_[slot],2,offsets.data());
        vkCmdPushConstants(command,layout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(parameters),&parameters);
        vkCmdDispatch(command,(parameters.count+63)/64,1,1);
    }
}
}
