#include "renderer.h"
#include "core/checkerboard.h"
#include "vulkan/layer_frame.h"
#include "vulkan/display_kernel.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <utility>
#ifdef TRACY_ENABLE
#include <tracy/Tracy.hpp>
#endif

namespace compositor {
namespace {
void check(VkResult result,const char* operation) {
    if(result!=VK_SUCCESS) throw std::runtime_error(std::string(operation)+" failed (VkResult "+std::to_string(result)+")");
}
float linear(float encoded) { return encoded<=.04045F ? encoded/12.92F : std::pow((encoded+.055F)/1.055F,2.4F); }
}
struct VulkanRenderer::State {
    VkInstance instance;
    Diagnostic diagnostic;
    std::function<void(FrameTiming)> complete;
    std::function<void(bool)> presentHook;
    std::string preferred;
    ResourceLimits limits=ResourceLimits::detect();
    std::shared_ptr<MemoryAdmission> memory;
    VkPhysicalDevice physical=VK_NULL_HANDLE;
    VkDevice device=VK_NULL_HANDLE;
    VkQueue queue=VK_NULL_HANDLE;
    uint32_t family=0,timestampBits=0;
    float timestampPeriod=0;
    VkCommandPool pool=VK_NULL_HANDLE;
    VkQueryPool queries=VK_NULL_HANDLE;
    struct Slot {
        VkCommandBuffer command=VK_NULL_HANDLE;
        VkFence fence=VK_NULL_HANDLE;
        VkSemaphore acquired=VK_NULL_HANDLE;
        bool submitted=false;
        bool document=false;
        FrameTiming timing;
    };
    std::array<Slot,2> slots{};
    std::size_t next=0;
    VkSurfaceKHR surface=VK_NULL_HANDLE;
    VkSwapchainKHR swapchain=VK_NULL_HANDLE;
    VkRenderPass pass=VK_NULL_HANDLE;
    VkExtent2D extent{};
    int requestedWidth=0,requestedHeight=0;
    bool srgb=false,bgra=false,transfer=false,float64=false;
    std::vector<VkImage> swapImages;
    std::unique_ptr<GpuLayerFrame> documentFrame;
    std::unique_ptr<DisplayKernel> display;
    std::unique_ptr<VulkanBuffer> displayBytes;
    std::optional<FrameRequest> frameInput;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;
    // A semaphore per acquired image, not per CPU frame slot: acquire proves
    // the previous presentation's semaphore wait on this image is finished.
    std::vector<VkSemaphore> rendered;

    void initialize(VkSurfaceKHR target) {
        uint32_t count=0; check(vkEnumeratePhysicalDevices(instance,&count,nullptr),"enumerate devices");
        std::vector<VkPhysicalDevice> devices(count);
        check(vkEnumeratePhysicalDevices(instance,&count,devices.data()),"enumerate devices");
        int best=-1;
        for(auto candidate:devices) {
            VkPhysicalDeviceProperties props{}; vkGetPhysicalDeviceProperties(candidate,&props);
            auto reject=[&](std::string reason){ diagnostic(std::string(props.deviceName)+": rejected: "+reason,false); };
            if(!preferred.empty() && std::string(props.deviceName).find(preferred)==std::string::npos) { reject("device-name filter"); continue; }
            if(props.apiVersion<VK_API_VERSION_1_2) { reject("requires Vulkan 1.2"); continue; }
            VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; features.pNext=&timeline;
            vkGetPhysicalDeviceFeatures2(candidate,&features);
            if(!timeline.timelineSemaphore) { reject("timeline semaphores unavailable"); continue; }
            uint32_t extensionCount=0; check(vkEnumerateDeviceExtensionProperties(candidate,nullptr,&extensionCount,nullptr),"device extensions");
            std::vector<VkExtensionProperties> extensions(extensionCount);
            check(vkEnumerateDeviceExtensionProperties(candidate,nullptr,&extensionCount,extensions.data()),"device extensions");
            if(std::none_of(extensions.begin(),extensions.end(),[](const auto& e){return std::strcmp(e.extensionName,VK_KHR_SWAPCHAIN_EXTENSION_NAME)==0;})) {
                reject("swapchain extension unavailable"); continue;
            }
            bool formatOkay=true;
            for(auto format:{VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R16_SFLOAT}) {
                VkFormatProperties p{}; vkGetPhysicalDeviceFormatProperties(candidate,format,&p);
                constexpr auto common=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT|
                                      VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
                if((p.optimalTilingFeatures&common)!=common) { formatOkay=false; break; }
                if(!(p.optimalTilingFeatures&VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
                    VkFormatProperties fp32{};
                    vkGetPhysicalDeviceFormatProperties(candidate,format==VK_FORMAT_R16_SFLOAT ? VK_FORMAT_R32_SFLOAT:VK_FORMAT_R32G32B32A32_SFLOAT,&fp32);
                    if(!(fp32.optimalTilingFeatures&VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) { formatOkay=false; break; }
                    diagnostic(std::string(props.deviceName)+": FP32 storage intermediate required",false);
                }
            }
            if(!formatOkay) { reject("canonical sampled/filter/transfer or storage fallback formats unavailable"); continue; }
            uint32_t queues=0; vkGetPhysicalDeviceQueueFamilyProperties(candidate,&queues,nullptr);
            std::vector<VkQueueFamilyProperties> families(queues); vkGetPhysicalDeviceQueueFamilyProperties(candidate,&queues,families.data());
            bool found=false;
            for(uint32_t i=0;i<queues;++i) {
                VkBool32 supports=false; check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate,i,target,&supports),"surface queue support");
                const auto required=VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT;
                if((families[i].queueFlags&required)!=required || !supports) continue;
                int score=props.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 300 :
                          props.deviceType==VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 200 : 100;
                if(score>best) { best=score; physical=candidate; family=i; timestampBits=families[i].timestampValidBits; timestampPeriod=props.limits.timestampPeriod; }
                found=true; break;
            }
            if(!found) reject("no combined graphics/compute/present queue");
        }
        if(!physical) throw std::runtime_error("No compatible Vulkan device; using CPU canvas");
        float priority=1;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex=family; queueInfo.queueCount=1; queueInfo.pQueuePriorities=&priority;
        VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES}; timeline.timelineSemaphore=VK_TRUE;
        const char* extension=VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; info.pNext=&timeline;
        VkPhysicalDeviceFeatures supported{};vkGetPhysicalDeviceFeatures(physical,&supported);float64=supported.shaderFloat64;
        VkPhysicalDeviceFeatures enabled{};enabled.shaderFloat64=float64;info.pEnabledFeatures=&enabled;
        info.queueCreateInfoCount=1; info.pQueueCreateInfos=&queueInfo; info.enabledExtensionCount=1; info.ppEnabledExtensionNames=&extension;
        check(vkCreateDevice(physical,&info,nullptr,&device),"create device"); vkGetDeviceQueue(device,family,0,&queue);
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; poolInfo.queueFamilyIndex=family; poolInfo.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        check(vkCreateCommandPool(device,&poolInfo,nullptr,&pool),"command pool");
        for(auto& slot:slots) {
            VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; allocation.commandPool=pool;
            allocation.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocation.commandBufferCount=1;
            check(vkAllocateCommandBuffers(device,&allocation,&slot.command),"command buffer");
            VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fence.flags=VK_FENCE_CREATE_SIGNALED_BIT;
            check(vkCreateFence(device,&fence,nullptr,&slot.fence),"frame fence");
            VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            check(vkCreateSemaphore(device,&semaphore,nullptr,&slot.acquired),"acquire semaphore");
        }
        if(timestampBits) {
            VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; query.queryType=VK_QUERY_TYPE_TIMESTAMP; query.queryCount=4;
            check(vkCreateQueryPool(device,&query,nullptr,&queries),"timestamp query pool");
        }
        VkPhysicalDeviceProperties props{}; vkGetPhysicalDeviceProperties(physical,&props);
        diagnostic(std::string("Vulkan: ")+props.deviceName+"; driver="+std::to_string(props.driverVersion)+
                   "; FIFO; 2 frame slots; timestamp bits="+std::to_string(timestampBits),false);
    }
    bool poll() {
        bool active=false;
        for(std::size_t i=0;i<slots.size();++i) {
            auto& slot=slots[i]; if(!slot.submitted) continue;
            const auto status=vkGetFenceStatus(device,slot.fence);
            if(status==VK_NOT_READY) { active=true; continue; }
            check(status,"frame completion");
            slot.timing.completedNs=monotonicNs();
            if(queries && !slot.document) {
                std::array<uint64_t,2> ticks{};
                check(vkGetQueryPoolResults(device,queries,static_cast<uint32_t>(i*2),2,sizeof(ticks),ticks.data(),sizeof(uint64_t),VK_QUERY_RESULT_64_BIT),"timestamp read");
                const uint64_t mask=timestampBits==64 ? UINT64_MAX : (uint64_t{1}<<timestampBits)-1;
                slot.timing.gpuNs=double((ticks[1]-ticks[0])&mask)*timestampPeriod;
            }
            slot.submitted=false; complete(slot.timing);
        }
        return active;
    }
    void releaseSwapchain() {
        if(!device) return;
        // Lifecycle only. Queue idleness also retires presentation before image
        // semaphores and old surfaces are released (not just rendering fences).
        const auto result=vkDeviceWaitIdle(device);
        if(result==VK_SUCCESS) poll();
        else for(auto& slot:slots) slot.submitted=false;
        // The lifecycle wait retired block, frame-copy and presentation reads.
        documentFrame.reset();display.reset();displayBytes.reset();frameInput.reset();swapImages.clear();
        for(auto f:framebuffers) vkDestroyFramebuffer(device,f,nullptr);
        framebuffers.clear();
        for(auto v:views) vkDestroyImageView(device,v,nullptr);
        views.clear();
        for(auto s:rendered) vkDestroySemaphore(device,s,nullptr);
        rendered.clear();
        if(pass) vkDestroyRenderPass(device,pass,nullptr);
        pass=VK_NULL_HANDLE;
        if(swapchain) vkDestroySwapchainKHR(device,swapchain,nullptr);
        swapchain=VK_NULL_HANDLE;
        surface=VK_NULL_HANDLE;
    }
    void createSwapchain(const FrameRequest& request) {
        releaseSwapchain();
        if(!device) initialize(request.surface);
        VkSurfaceCapabilitiesKHR caps{}; check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,request.surface,&caps),"surface capabilities");
        if(!(caps.supportedUsageFlags&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) throw std::runtime_error("Surface cannot be a color attachment");
        transfer=(caps.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_DST_BIT)!=0;
        if(caps.currentExtent.width!=UINT32_MAX) extent=caps.currentExtent;
        else extent={std::clamp(static_cast<uint32_t>(request.width),caps.minImageExtent.width,caps.maxImageExtent.width),
                     std::clamp(static_cast<uint32_t>(request.height),caps.minImageExtent.height,caps.maxImageExtent.height)};
        if(extent.width==0 || extent.height==0) return;
        uint32_t count=0; check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,request.surface,&count,nullptr),"surface formats");
        std::vector<VkSurfaceFormatKHR> formats(count); check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,request.surface,&count,formats.data()),"surface formats");
        auto chosen=std::find_if(formats.begin(),formats.end(),[](auto f){return f.colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
            (f.format==VK_FORMAT_B8G8R8A8_UNORM || f.format==VK_FORMAT_R8G8B8A8_UNORM);});
        if(chosen==formats.end()) chosen=std::find_if(formats.begin(),formats.end(),[](auto f){return f.colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
            (f.format==VK_FORMAT_B8G8R8A8_SRGB || f.format==VK_FORMAT_R8G8B8A8_SRGB);});
        if(chosen==formats.end()) throw std::runtime_error("No supported SDR surface format");
        srgb=chosen->format==VK_FORMAT_B8G8R8A8_SRGB || chosen->format==VK_FORMAT_R8G8B8A8_SRGB;
        bgra=chosen->format==VK_FORMAT_B8G8R8A8_SRGB || chosen->format==VK_FORMAT_B8G8R8A8_UNORM;
        uint32_t images=std::max(2U,caps.minImageCount); if(caps.maxImageCount) images=std::min(images,caps.maxImageCount);
        VkCompositeAlphaFlagBitsKHR alpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        for(auto value:{VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
            if(caps.supportedCompositeAlpha&value) { alpha=value; break; }
        VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR}; info.surface=request.surface;
        info.minImageCount=images; info.imageFormat=chosen->format; info.imageColorSpace=chosen->colorSpace; info.imageExtent=extent;
        info.imageArrayLayers=1; info.imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; info.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;
        if(transfer) info.imageUsage|=VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.preTransform=caps.currentTransform; info.compositeAlpha=alpha; info.presentMode=VK_PRESENT_MODE_FIFO_KHR; info.clipped=VK_TRUE;
        check(vkCreateSwapchainKHR(device,&info,nullptr,&swapchain),"create swapchain"); surface=request.surface;
        requestedWidth=request.width; requestedHeight=request.height;
        check(vkGetSwapchainImagesKHR(device,swapchain,&images,nullptr),"swapchain images");
        std::vector<VkImage> imageHandles(images); check(vkGetSwapchainImagesKHR(device,swapchain,&images,imageHandles.data()),"swapchain images");
        swapImages=imageHandles;
        VkAttachmentDescription attachment{}; attachment.format=chosen->format; attachment.samples=VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED; attachment.finalLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{}; subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS; subpass.colorAttachmentCount=1; subpass.pColorAttachments=&ref;
        VkSubpassDependency dependency{}; dependency.srcSubpass=VK_SUBPASS_EXTERNAL; dependency.dstSubpass=0;
        dependency.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; dependency.dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; passInfo.attachmentCount=1; passInfo.pAttachments=&attachment;
        passInfo.subpassCount=1; passInfo.pSubpasses=&subpass; passInfo.dependencyCount=1; passInfo.pDependencies=&dependency;
        check(vkCreateRenderPass(device,&passInfo,nullptr,&pass),"render pass");
        for(auto image:imageHandles) {
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image=image; view.viewType=VK_IMAGE_VIEW_TYPE_2D; view.format=chosen->format;
            view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            VkImageView handle{}; check(vkCreateImageView(device,&view,nullptr,&handle),"swapchain view"); views.push_back(handle);
            VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fb.renderPass=pass; fb.attachmentCount=1; fb.pAttachments=&views.back();
            fb.width=extent.width; fb.height=extent.height; fb.layers=1;
            VkFramebuffer framebuffer{}; check(vkCreateFramebuffer(device,&fb,nullptr,&framebuffer),"framebuffer"); framebuffers.push_back(framebuffer);
            VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VkSemaphore done{}; check(vkCreateSemaphore(device,&semaphore,nullptr,&done),"present semaphore"); rendered.push_back(done);
        }
        diagnostic("Swapchain "+std::to_string(extent.width)+"x"+std::to_string(extent.height)+"; images="+std::to_string(images),false);
    }
    bool prepareDocument(const FrameRequest& request) {
        if(!float64 || !transfer) throw std::runtime_error("GPU document presentation needs FP64 and transfer-capable surface; using CPU");
        if(!frameInput || frameInput->stop!=request.stop || frameInput->requestedNs!=request.requestedNs) {
            if(documentFrame && documentFrame->state()==GpuLayerFrame::State::Pending) {
                documentFrame->cancel();
                if(documentFrame->advance()==GpuLayerFrame::State::Pending) return false;
            }
            // The previous displayed frame can still be read by a presentation
            // command. Poll those fences before reusing either linear or SDR data.
            if(std::any_of(slots.begin(),slots.end(),[](const Slot& slot){return slot.submitted;})) return false;
            if(!documentFrame) documentFrame=std::make_unique<GpuLayerFrame>(physical,device,queue,family,limits.gpuTiles,limits.gpuFrame,limits.gpuMips,memory);
            if(!display) display=std::make_unique<DisplayKernel>(device);
            const auto pixels=DisplayKernel::paddedPixels(std::uint64_t(extent.width)*extent.height);
            if(!displayBytes || displayBytes->bytes()!=pixels*4) {
                displayBytes.reset();
                displayBytes=std::make_unique<VulkanBuffer>(physical,device,pixels*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VulkanBuffer::Memory::Device,memory);
            }
            const auto origin=request.view.pixelOrigin();
            documentFrame->start(request.document,{origin.x,origin.y},{1/request.view.zoom,1/request.view.zoom},extent.width,extent.height,request.stop);
            frameInput=request;
        }
        const auto status=documentFrame->advance();
        if(status==GpuLayerFrame::State::NeedsCpu) throw std::runtime_error("GPU tile budget or source geometry requires CPU fallback");
        return status==GpuLayerFrame::State::Complete;
    }
    void recordDocument(VkCommandBuffer command,std::size_t slot,std::uint32_t image,const FrameRequest& request) {
        VkMemoryBarrier ready{VK_STRUCTURE_TYPE_MEMORY_BARRIER};ready.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        ready.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&ready,0,nullptr,0,nullptr);
        DisplayParameters parameters{};const auto origin=request.view.pixelOrigin();parameters.origin={origin.x,origin.y};parameters.step={1/request.view.zoom,1/request.view.zoom};
        parameters.size={extent.width,extent.height};parameters.canvas={static_cast<std::uint32_t>(request.document->width),static_cast<std::uint32_t>(request.document->height)};
        parameters.cell=static_cast<std::uint32_t>(std::clamp(16*request.view.dpr,1.,32768.));parameters.bgra=bgra;
        parameters.background={engine::decodeSrgb(.85f),engine::decodeSrgb(.7f)};
        display->record(command,slot,{documentFrame->output(),documentFrame->outputBytes()},{displayBytes->handle(),displayBytes->bytes()},parameters);
        ready.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;ready.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&ready,0,nullptr,0,nullptr);
        VkImageMemoryBarrier imageBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};imageBarrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;imageBarrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        imageBarrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;imageBarrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        imageBarrier.image=swapImages[image];imageBarrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};imageBarrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        // Chain the layout transition after the acquire semaphore's TRANSFER
        // wait. TOP_OF_PIPE would let the transition race presentation reads,
        // even though the old image contents are intentionally discarded.
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&imageBarrier);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={extent.width,extent.height,1};
        vkCmdCopyBufferToImage(command,displayBytes->handle(),swapImages[image],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
        imageBarrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;imageBarrier.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        imageBarrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;imageBarrier.dstAccessMask=0;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&imageBarrier);
    }
    bool render(const FrameRequest& request) {
#ifdef TRACY_ENABLE
        ZoneScopedN("M1 Vulkan canvas");
#endif
        if(request.stop.stop_requested()) return true;
        if(!swapchain || surface!=request.surface || requestedWidth!=request.width || requestedHeight!=request.height) createSwapchain(request);
        if(!swapchain) return true;
        if(request.document && !prepareDocument(request)) return request.stop.stop_requested();
        if(request.stop.stop_requested()) return true;
        auto& slot=slots[next];
        if(slot.submitted) return false; // Drain asynchronously; never wait in the GUI thread.
        uint32_t image=0;
        auto result=vkAcquireNextImageKHR(device,swapchain,16000000,slot.acquired,VK_NULL_HANDLE,&image);
        if(result==VK_TIMEOUT || result==VK_NOT_READY) return false;
        if(result==VK_ERROR_OUT_OF_DATE_KHR) { releaseSwapchain(); return false; }
        if(result!=VK_SUBOPTIMAL_KHR) check(result,"acquire image");
        bool recreate=result==VK_SUBOPTIMAL_KHR;
        check(vkResetCommandBuffer(slot.command,0),"reset commands");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(slot.command,&begin),"begin commands");
        const auto query=static_cast<uint32_t>(next*2);
        if(queries) { vkCmdResetQueryPool(slot.command,queries,query,2); vkCmdWriteTimestamp(slot.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,queries,query); }
        if(request.document) recordDocument(slot.command,next,image,request);
        else {
        const float background=srgb ? linear(.12F):.12F;
        VkClearValue clear{}; clear.color={{background,background,background,1}};
        VkRenderPassBeginInfo renderInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; renderInfo.renderPass=pass; renderInfo.framebuffer=framebuffers[image];
        renderInfo.renderArea={{0,0},extent}; renderInfo.clearValueCount=1; renderInfo.pClearValues=&clear;
        vkCmdBeginRenderPass(slot.command,&renderInfo,VK_SUBPASS_CONTENTS_INLINE);
        for(const auto& r:checkerboard(request.view,static_cast<int>(extent.width),static_cast<int>(extent.height),request.cursor,request.showCursor)) {
            VkClearAttachment color{}; color.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; color.colorAttachment=0;
            const float value=srgb ? linear(r.value):r.value; color.clearValue.color={{value,value,value,1}};
            VkClearRect rect{{{r.x,r.y},{static_cast<uint32_t>(r.width),static_cast<uint32_t>(r.height)}},0,1};
            vkCmdClearAttachments(slot.command,1,&color,1,&rect);
        }
        vkCmdEndRenderPass(slot.command);
        }
        if(queries) vkCmdWriteTimestamp(slot.command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,queries,query+1);
        check(vkEndCommandBuffer(slot.command),"end commands");
        check(vkResetFences(device,1,&slot.fence),"reset fence");
        VkPipelineStageFlags stage=request.document ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.waitSemaphoreCount=1; submit.pWaitSemaphores=&slot.acquired; submit.pWaitDstStageMask=&stage;
        submit.commandBufferCount=1; submit.pCommandBuffers=&slot.command; submit.signalSemaphoreCount=1; submit.pSignalSemaphores=&rendered[image];
        slot.timing={request.requestedNs,monotonicNs(),0,-1};
        // Present-command timestamps omit the earlier layered submissions;
        // report no GPU duration for documents rather than a misleading total.
        slot.document=bool(request.document);
        check(vkQueueSubmit(queue,1,&submit,slot.fence),"submit"); slot.submitted=true;
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; present.waitSemaphoreCount=1; present.pWaitSemaphores=&rendered[image];
        present.swapchainCount=1; present.pSwapchains=&swapchain; present.pImageIndices=&image;
        presentHook(true); result=vkQueuePresentKHR(queue,&present); presentHook(false);
        next=(next+1)%slots.size();
        if(result==VK_ERROR_OUT_OF_DATE_KHR || result==VK_SUBOPTIMAL_KHR) recreate=true;
        else check(result,"present");
        if(recreate) {releaseSwapchain();return false;}
        return true;
    }
    ~State() {
        if(!device) return;
        try { releaseSwapchain(); } catch(...) {} // Device may already be lost.
        if(queries) vkDestroyQueryPool(device,queries,nullptr);
        for(auto& slot:slots) { if(slot.fence) vkDestroyFence(device,slot.fence,nullptr); if(slot.acquired) vkDestroySemaphore(device,slot.acquired,nullptr); }
        if(pool) vkDestroyCommandPool(device,pool,nullptr);
        vkDestroyDevice(device,nullptr);
    }
};

VulkanRenderer::VulkanRenderer(VkInstance instance,Diagnostic diagnostic,std::function<void(FrameTiming)> complete,
                             std::function<void(bool)> presentHook,std::string deviceName,ResourceLimits limits,std::shared_ptr<MemoryAdmission> memory)
    :state_(std::make_unique<State>()) {
    state_->instance=instance; state_->diagnostic=std::move(diagnostic); state_->complete=std::move(complete);
    state_->presentHook=std::move(presentHook); state_->preferred=std::move(deviceName);state_->limits=limits;state_->memory=std::move(memory);
    thread_=std::thread([this]{run();});
}
VulkanRenderer::~VulkanRenderer() {
    std::stop_source cancel{std::nostopstate};
    { std::lock_guard lock(mutex_); quit_=true; pending_.reset();cancel=cancellation_; }
    cancel.request_stop();cv_.notify_all();thread_.join();
}
void VulkanRenderer::request(FrameRequest request) {
    std::stop_source previous{std::nostopstate};
    {
        std::lock_guard lock(mutex_);
        if(failed_ || detach_ || quit_) return;
        previous=cancellation_;cancellation_=std::stop_source{};
        request.stop=cancellation_.get_token();pending_=std::move(request);
    }
    // Stop callbacks may wake an IO wait. Invoke them outside the mailbox lock;
    // the worker never holds this lock during synchronous tile rehydration.
    previous.request_stop();cv_.notify_all();
}
void VulkanRenderer::detach() {
    std::unique_lock lock(mutex_); pending_.reset(); detach_=true;
    const auto generation=++detachGeneration_;
    auto cancel=cancellation_;
    lock.unlock();cancel.request_stop();cv_.notify_all();lock.lock();
    // Completion belongs to this barrier, not to the latest frame request.
    // A producer may submit again before this waiter reacquires the mutex.
    cv_.wait(lock,[this,generation]{return completedDetachGeneration_>=generation;});
}
void VulkanRenderer::run() {
    bool active=false;
    // A retry belongs to this worker, not the producer mailbox. Keeping the
    // mailbox empty while waiting lets a newer request (or lifecycle barrier)
    // wake the worker immediately without an unconditional retry sleep.
    std::optional<FrameRequest> retry;
    for(;;) {
        std::unique_lock lock(mutex_);
        const auto ready=[this]{return quit_ || detach_ || pending_.has_value();};
        if(active || retry) cv_.wait_for(lock,std::chrono::milliseconds(2),ready); else cv_.wait(lock,ready);
        if(quit_) break;
        if(detach_) {
            retry.reset();
            lock.unlock();
            try { state_->releaseSwapchain(); } catch(const std::exception& e) { state_->diagnostic(e.what(),true); }
            lock.lock(); detach_=false; completedDetachGeneration_=detachGeneration_;
            active=false; cv_.notify_all(); continue;
        }
        auto request=pending_ ? std::exchange(pending_,std::nullopt) : std::exchange(retry,std::nullopt);
        // A fresh producer request supersedes any older unfinished frame.
        retry.reset();lock.unlock();
        try {
            active=state_->poll();
            if(request && request->surface && request->width>0 && request->height>0) {
                const bool consumed=state_->render(*request);
                active=true;
                if(!consumed) retry=std::move(request);
            }
        } catch(const std::exception& e) {
            { std::lock_guard guard(mutex_); failed_=true; pending_.reset(); }
            retry.reset();state_->diagnostic(e.what(),true); active=false;
        }
    }
    state_.reset(); // All GPU object destruction occurs on the owning worker.
}
}
