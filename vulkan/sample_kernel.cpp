#include "vulkan/sample_kernel.h"
#include "sample_spv.h"
#include <cstddef>
#include <stdexcept>
#include <string>

namespace compositor {
static_assert(sizeof(SampleParameters)==240 && offsetof(SampleParameters,outputOffset)==144 &&
              offsetof(SampleParameters,footprint)==160 && offsetof(SampleParameters,fp32Source)==176 &&
              offsetof(SampleParameters,inputOrigin)==192 && offsetof(SampleParameters,inputScale)==208 && offsetof(SampleParameters,tapOrigin)==224);
static_assert(offsetof(SampleParameters,origin)==48 && offsetof(SampleParameters,extent)==80 &&
              offsetof(SampleParameters,defaultPixel)==112 && offsetof(SampleParameters,width)==128);
void SampleParameters::validate() const {
    inverse.inverse();
    for(auto v:{origin.x,origin.y,step.x,step.y}) if(!std::isfinite(v)) throw std::invalid_argument("Invalid sample coordinates");
    if(width==0 || height==0 || width>256 || height>256 || sampling>2 || !std::isfinite(opacity) || opacity<0 || opacity>1)
        throw std::invalid_argument("Invalid sample dimensions or appearance");
    for(double value:{footprint.x,footprint.y})
        if(!std::isfinite(value) || value<1 || value>16) throw std::invalid_argument("Unsupported direct GPU filter footprint");
    if(fp32Source>1) throw std::invalid_argument("Invalid source precision");
    for(double value:{inputOrigin.x,inputOrigin.y,tapOrigin.x,tapOrigin.y})
        if(!std::isfinite(value)) throw std::invalid_argument("Invalid sampling-grid origin");
    if(!std::isfinite(inputScale) || inputScale<1 || inputScale>32768)
        throw std::invalid_argument("Invalid mip grid scale");
    for(auto axis:{0,1}) {
        const double value=axis==0 ? tapOrigin.x : tapOrigin.y;
        if(value!=std::floor(value) || std::abs(value)>1073741824 || std::abs(double(extent[static_cast<std::size_t>(axis)])-value)>1073741824)
            throw std::out_of_range("Tap origin requires CPU geometry fallback");
    }
    if(outputOffset[0]>32768-width || outputOffset[1]>32768-height) throw std::invalid_argument("Sample output offset exceeds viewport limits");
    if(extent[0]<-1073741824 || extent[1]<-1073741824 || extent[0]>1073741824 || extent[1]>1073741824)
        throw std::out_of_range("Source grid requires CPU geometry fallback");
    engine::Extent source{extent[0],extent[1],extent[2],extent[3]};source.validate();
    const auto gx=engine::floorTile(source.x),gy=engine::floorTile(source.y);
    if(grid[0]!=gx || grid[1]!=gy || grid[2]!=engine::floorTile(source.x+source.width-1)-gx+1 ||
       grid[3]!=engine::floorTile(source.y+source.height-1)-gy+1) throw std::invalid_argument("Invalid source grid table");
    engine::pack(defaultPixel);
}
namespace {
void checkSample(VkResult result,const char* operation) {
    if(result!=VK_SUCCESS) throw std::runtime_error(std::string(operation)+": "+std::to_string(result));
}
}
SampleKernel::SampleKernel(VkDevice device):device_(device) {
    if(!device) throw std::invalid_argument("Missing sampling device");
    VkShaderModule shader=VK_NULL_HANDLE;
    try {
        std::array<VkDescriptorSetLayoutBinding,4> bindings{};
        for(std::uint32_t i=0;i<4;++i) bindings[i]={i,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo descriptors{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};descriptors.bindingCount=4;descriptors.pBindings=bindings.data();
        checkSample(vkCreateDescriptorSetLayout(device_,&descriptors,nullptr,&descriptors_),"Sample descriptor layout");
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};layout.setLayoutCount=1;layout.pSetLayouts=&descriptors_;
        checkSample(vkCreatePipelineLayout(device_,&layout,nullptr,&layout_),"Sample pipeline layout");
        VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};module.codeSize=sizeof(compositorSampleSpv);module.pCode=compositorSampleSpv;
        checkSample(vkCreateShaderModule(device_,&module,nullptr,&shader),"Sample shader");
        VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pipeline.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        pipeline.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;pipeline.stage.module=shader;pipeline.stage.pName="main";pipeline.layout=layout_;
        checkSample(vkCreateComputePipelines(device_,VK_NULL_HANDLE,1,&pipeline,nullptr,&pipeline_),"Sample pipeline");
        vkDestroyShaderModule(device_,shader,nullptr);shader=VK_NULL_HANDLE;
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,8};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=2;pool.poolSizeCount=1;pool.pPoolSizes=&size;
        checkSample(vkCreateDescriptorPool(device_,&pool,nullptr,&pool_),"Sample descriptor pool");
        const std::array<VkDescriptorSetLayout,2> layouts{descriptors_,descriptors_};
        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};allocation.descriptorPool=pool_;allocation.descriptorSetCount=2;allocation.pSetLayouts=layouts.data();
        checkSample(vkAllocateDescriptorSets(device_,&allocation,sets_.data()),"Sample descriptor sets");
    } catch(...) {if(shader) vkDestroyShaderModule(device_,shader,nullptr);release();throw;}
}
void SampleKernel::release() noexcept {
    if(pool_) vkDestroyDescriptorPool(device_,pool_,nullptr);
    if(pipeline_) vkDestroyPipeline(device_,pipeline_,nullptr);
    if(layout_) vkDestroyPipelineLayout(device_,layout_,nullptr);
    if(descriptors_) vkDestroyDescriptorSetLayout(device_,descriptors_,nullptr);
}
SampleKernel::~SampleKernel(){release();}
void SampleKernel::record(VkCommandBuffer command,std::size_t slot,Buffer pool,Buffer table,Buffer parametersBuffer,
                          Buffer output,const SampleParameters& parameters) {
    parameters.validate();
    if(!command || slot>=sets_.size()) throw std::invalid_argument("Invalid sample command or slot");
    const std::array<Buffer,4> buffers{pool,table,parametersBuffer,output};
    const std::array<VkDeviceSize,4> minimum{parameters.fp32Source ? 1048576u : 524288u,VkDeviceSize(parameters.grid[2])*VkDeviceSize(parameters.grid[3])*16,
        sizeof(SampleParameters),VkDeviceSize(parameters.width)*parameters.height*16};
    std::array<VkDescriptorBufferInfo,4> info{};std::array<VkWriteDescriptorSet,4> writes{};
    for(std::size_t i=0;i<4;++i) {
        if(!buffers[i].handle || buffers[i].bytes<minimum[i]) throw std::invalid_argument("Sample buffer too small");
        for(std::size_t j=0;j<i;++j) if(buffers[i].handle==buffers[j].handle) throw std::invalid_argument("Sample buffers alias");
        info[i]={buffers[i].handle,0,i==0 ? pool.bytes : minimum[i]};writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet=sets_[slot];writes[i].dstBinding=static_cast<std::uint32_t>(i);writes[i].descriptorCount=1;
        writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[i].pBufferInfo=&info[i];
    }
    vkUpdateDescriptorSets(device_,4,writes.data(),0,nullptr);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout_,0,1,&sets_[slot],0,nullptr);
    vkCmdDispatch(command,(parameters.width*parameters.height+63)/64,1,1);
}
}
