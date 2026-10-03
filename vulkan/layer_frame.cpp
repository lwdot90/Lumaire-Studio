#include "vulkan/layer_frame.h"
#include "vulkan/display_kernel.h"
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace compositor {
namespace {
void checkFrame(VkResult result,const char* operation) {
    if(result!=VK_SUCCESS) throw std::runtime_error(std::string(operation)+": "+std::to_string(result));
}
}
GpuLayerFrame::GpuLayerFrame(VkPhysicalDevice physical,VkDevice device,VkQueue queue,std::uint32_t family,
                           std::size_t tileBudget,VkDeviceSize frameBudget,VkDeviceSize mipBudget,std::shared_ptr<MemoryAdmission> memory):
    physical_(physical),device_(device),queue_(queue),budget_(frameBudget),memory_(std::move(memory)),block_(physical,device,queue,family,tileBudget,mipBudget,memory_) {
    try {
        VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};info.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;info.queueFamilyIndex=family;
        checkFrame(vkCreateCommandPool(device_,&info,nullptr,&pool_),"Create frame command pool");
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};allocation.commandPool=pool_;allocation.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;allocation.commandBufferCount=1;
        checkFrame(vkAllocateCommandBuffers(device_,&allocation,&command_),"Allocate frame command");
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};type.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};semaphore.pNext=&type;
        checkFrame(vkCreateSemaphore(device_,&semaphore,nullptr,&timeline_),"Create frame timeline");
    } catch(...) {release();throw;}
}
void GpuLayerFrame::release() noexcept {
    if(pool_) vkDestroyCommandPool(device_,pool_,nullptr);
    if(timeline_) vkDestroySemaphore(device_,timeline_,nullptr);
}
GpuLayerFrame::~GpuLayerFrame(){release();}
bool GpuLayerFrame::copyRetired() const {
    std::uint64_t completed=0;checkFrame(vkGetSemaphoreCounterValue(device_,timeline_,&completed),"Poll frame copies");return completed>=serial_;
}
void GpuLayerFrame::start(engine::DocumentPtr document,engine::Coordinate origin,engine::Coordinate step,std::uint32_t width,std::uint32_t height,std::stop_token stop) {
    if(failed_ || state_==State::Pending || !copyRetired()) throw std::logic_error("Previous frame still active or queue failed");
    if(!document || width==0 || height==0 || width>32768 || height>32768 || !std::isfinite(step.x) || !std::isfinite(step.y) || step.x<=0 || step.y<=0)
        throw std::invalid_argument("Invalid GPU frame request");
    engine::SampleRegion{origin.x,origin.y,origin.x+(width-1)*step.x,origin.y+(height-1)*step.y}.validate();
    const VkDeviceSize bytes=DisplayKernel::paddedPixels(std::uint64_t(width)*height)*sizeof(engine::Pixel);
    if(bytes>budget_) throw std::length_error("GPU viewport exceeds frame budget");
    stop_=stop;
    if(stop_.stop_requested()) {cancel();return;}
    // Re-exposing an unchanged immutable snapshot needs presentation, not new
    // layer dispatches. Identity includes the exact absolute sampling grid;
    // request timestamps, tab focus and fit-mode metadata are not pixel inputs.
    // Never reuse cancelled, partial or failed output, even for the same key.
    if(state_==State::Complete && document_==document && width_==width && height_==height &&
       origin_.x==origin.x && origin_.y==origin.y && step_.x==step.x && step_.y==step.y) return;
    std::vector<Rect> pending;
    const bool reuse=state_==State::Complete && document_ && width_==width && height_==height &&
        origin_.x==origin.x && origin_.y==origin.y && step_.x==step.x && step_.y==step.y;
    for(std::uint32_t y=0;y<height;y+=256) for(std::uint32_t x=0;x<width;x+=256) {
        const Rect region{x,y,std::min(256u,width-x),std::min(256u,height-y)};
        const engine::SampleRegion bounds{origin.x+x*step.x,origin.y+y*step.y,
            origin.x+(x+region.width-1)*step.x,origin.y+(y+region.height-1)*step.y};
        if(!reuse || !engine::sameRegion(document_->stack,document->stack,bounds,step)) pending.push_back(region);
    }
    if(!output_ || output_->bytes()!=bytes) {
        // Release retired storage before replacement so resizing cannot double
        // the frame allocation. Allocation failure exposes no stale result.
        state_=State::Idle;output_.reset();
        output_=std::make_unique<VulkanBuffer>(physical_,device_,bytes,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VulkanBuffer::Memory::Device,memory_);
        if(output_->allocationBytes()>budget_) {output_.reset();throw std::length_error("GPU frame allocation padding exceeds budget");}
    }
    document_=std::move(document);origin_=origin;step_=step;width_=width;height_=height;pending_=std::move(pending);
    rendering_=false;cancelled_=false;splits_=0;copied_=0;state_=State::Pending;
}
void GpuLayerFrame::cancel() {
    cancelled_=true;block_.cancel();
    if(state_!=State::Pending) {state_=State::Cancelled;document_.reset();pending_.clear();}
}
VkBuffer GpuLayerFrame::output() const {
    if(state_!=State::Complete) throw std::logic_error("No complete GPU frame");
    return output_->handle();
}
void GpuLayerFrame::copyBlock() {
    if(serial_==std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("Frame timeline exhausted");
    checkFrame(vkResetCommandBuffer(command_,0),"Reset frame copy");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checkFrame(vkBeginCommandBuffer(command_,&begin),"Begin frame copy");
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(command_,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    std::array<VkBufferCopy,256> rows{};
    for(std::uint32_t y=0;y<active_.height;++y)
        rows[y]={VkDeviceSize(y)*active_.width*sizeof(engine::Pixel),(VkDeviceSize(active_.y+y)*width_+active_.x)*sizeof(engine::Pixel),VkDeviceSize(active_.width)*sizeof(engine::Pixel)};
    vkCmdCopyBuffer(command_,block_.output(),output_->handle(),active_.height,rows.data());
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command_,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    checkFrame(vkEndCommandBuffer(command_),"End frame copy");
    const auto value=serial_+1;
    VkTimelineSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};signal.signalSemaphoreValueCount=1;signal.pSignalSemaphoreValues=&value;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.pNext=&signal;submit.commandBufferCount=1;submit.pCommandBuffers=&command_;submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&timeline_;
    const auto result=vkQueueSubmit(queue_,1,&submit,VK_NULL_HANDLE);
    if(result!=VK_SUCCESS) {failed_=true;checkFrame(result,"Submit frame copy");}
    serial_=value;++copied_;
}
GpuLayerFrame::State GpuLayerFrame::advance() {
    if(failed_) throw std::logic_error("Frame queue failed");
    if(stop_.stop_requested()) cancel();
    if(state_!=State::Pending || !copyRetired()) return state_;
    if(cancelled_) {
        if(block_.advance()==GpuLayerBlock::State::Pending) return state_;
        state_=State::Cancelled;document_.reset();pending_.clear();return state_;
    }
    if(!rendering_) {
        if(pending_.empty()) {state_=State::Complete;return state_;}
        active_=pending_.back();
        block_.start(document_->stack,origin_,step_,active_.width,active_.height,active_.x,active_.y,stop_);
        pending_.pop_back();rendering_=true;
    }
    GpuLayerBlock::State result;
    try {result=block_.advance();}
    catch(const std::out_of_range&) {
        // The sampler explicitly rejects source origins outside its supported
        // grid. Preserve the captured document for a CPU fallback, never narrow.
        block_.cancel();block_.advance();state_=State::NeedsCpu;pending_.clear();return state_;
    }
    if(stop_.stop_requested() || result==GpuLayerBlock::State::Cancelled) {cancel();return state_;}
    if(result==GpuLayerBlock::State::Complete) {copyBlock();rendering_=false;}
    else if(result==GpuLayerBlock::State::NeedsSplit) {
        if(active_.width==1 && active_.height==1) {state_=State::NeedsCpu;pending_.clear();return state_;}
        auto first=active_,second=active_;
        if(active_.width>=active_.height && active_.width>1) {
            first.width=active_.width/2;second.x+=first.width;second.width-=first.width;
        } else {first.height=active_.height/2;second.y+=first.height;second.height-=first.height;}
        pending_.reserve(pending_.size()+2);pending_.push_back(second);pending_.push_back(first);++splits_;rendering_=false;
    }
    return state_;
}
}
