#include "vulkan/blend_kernel.h"
#include "io/spill_store.h"
#include <filesystem>
#include "vulkan/tile_upload.h"
#include "vulkan/buffer.h"
#include "vulkan/tile_cache.h"
#include "vulkan/sample_kernel.h"
#include "vulkan/layer_block.h"
#include "vulkan/layer_frame.h"
#include "vulkan/display_kernel.h"
#include "vulkan/mip_cache.h"
#include "rendering/mip_cache.h"
#include "rendering/layer_sampler.h"
#include "rendering/display_pixel.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <vector>

using namespace compositor;
using namespace compositor::engine;
namespace {
void expect(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
void check(VkResult result,const char* message) { if(result!=VK_SUCCESS) throw std::runtime_error(std::string(message)+": "+std::to_string(result)); }
template<class F> void rejects(F f) { bool caught=false;try {f();} catch(const std::exception&) {caught=true;} expect(caught,"Invalid dispatch accepted"); }
struct Context {
    VkInstance instance=VK_NULL_HANDLE; VkPhysicalDevice physical=VK_NULL_HANDLE; VkDevice device=VK_NULL_HANDLE;
    VkQueue queue=VK_NULL_HANDLE; std::uint32_t family=0; VkCommandPool pool=VK_NULL_HANDLE;
    VkCommandBuffer command=VK_NULL_HANDLE; VkFence fence=VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger=VK_NULL_HANDLE; std::atomic<unsigned> errors{0};
    bool float64=false;
    static VKAPI_ATTR VkBool32 VKAPI_CALL message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,VkDebugUtilsMessageTypeFlagsEXT,
        const VkDebugUtilsMessengerCallbackDataEXT* data,void* user) {
        if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {++static_cast<Context*>(user)->errors;std::cerr<<data->pMessage<<'\n';}
        return VK_FALSE;
    }
    bool initialize(bool requireValidation,const std::string& preferred) {
        std::uint32_t count=0; check(vkEnumerateInstanceLayerProperties(&count,nullptr),"Enumerate layers");
        std::vector<VkLayerProperties> layers(count); check(vkEnumerateInstanceLayerProperties(&count,layers.data()),"Enumerate layers");
        const bool validation=std::any_of(layers.begin(),layers.end(),[](const auto& p){return std::strcmp(p.layerName,"VK_LAYER_KHRONOS_validation")==0;});
        if(requireValidation) expect(validation,"Required Vulkan validation layer missing");
        check(vkEnumerateInstanceExtensionProperties(nullptr,&count,nullptr),"Enumerate extensions");
        std::vector<VkExtensionProperties> extensions(count); check(vkEnumerateInstanceExtensionProperties(nullptr,&count,extensions.data()),"Enumerate extensions");
        const bool debug=validation && std::any_of(extensions.begin(),extensions.end(),[](const auto& p){return std::strcmp(p.extensionName,VK_EXT_DEBUG_UTILS_EXTENSION_NAME)==0;});
        if(requireValidation) expect(debug,"Required validation debug messenger missing");
        bool synchronization=false;
        if(validation) {
            check(vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation",&count,nullptr),"Validation extensions");
            std::vector<VkExtensionProperties> supported(count);check(vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation",&count,supported.data()),"Validation extensions");
            synchronization=std::any_of(supported.begin(),supported.end(),[](const auto& p){return std::strcmp(p.extensionName,VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME)==0;});
        }
        if(requireValidation) expect(synchronization,"Required synchronization validation feature missing");
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.pApplicationName="Compositor GPU blend test"; app.apiVersion=VK_API_VERSION_1_2;
        const char* layer="VK_LAYER_KHRONOS_validation";
        std::vector<const char*> enabledExtensions;
        if(debug) enabledExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        if(synchronization) enabledExtensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
        VkDebugUtilsMessengerCreateInfoEXT diagnostic{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        diagnostic.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        diagnostic.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        diagnostic.pfnUserCallback=message; diagnostic.pUserData=this;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; info.pApplicationInfo=&app;
        if(validation) {info.enabledLayerCount=1;info.ppEnabledLayerNames=&layer;}
        info.enabledExtensionCount=static_cast<std::uint32_t>(enabledExtensions.size());info.ppEnabledExtensionNames=enabledExtensions.data();
        if(debug) info.pNext=&diagnostic;
        const VkValidationFeatureEnableEXT enabled=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
        VkValidationFeaturesEXT features{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};features.enabledValidationFeatureCount=1;features.pEnabledValidationFeatures=&enabled;
        if(synchronization) {features.pNext=info.pNext;info.pNext=&features;}
        check(vkCreateInstance(&info,nullptr,&instance),"Create instance");
        if(debug) {
            const auto create=reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT"));
            expect(create!=nullptr,"Missing debug messenger function"); check(create(instance,&diagnostic,nullptr,&messenger),"Create messenger");
        }
        check(vkEnumeratePhysicalDevices(instance,&count,nullptr),"Enumerate devices");
        std::vector<VkPhysicalDevice> devices(count); check(vkEnumeratePhysicalDevices(instance,&count,devices.data()),"Enumerate devices");
        int best=-1;
        for(const auto candidate:devices) {
            VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(candidate,&properties);
            if(properties.apiVersion<VK_API_VERSION_1_2 || (!preferred.empty() && std::string(properties.deviceName).find(preferred)==std::string::npos)) continue;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate,&count,nullptr); std::vector<VkQueueFamilyProperties> families(count);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate,&count,families.data());
            for(std::uint32_t i=0;i<count;++i) if(families[i].queueFlags&VK_QUEUE_COMPUTE_BIT) {
                const int score=properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
                if(score>best) {best=score;physical=candidate;family=i;} break;
            }
        }
        if(!physical) return false;
        VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(physical,&properties);
        std::cout<<"Device: "<<properties.deviceName<<"; driver="<<properties.driverVersion<<"; validation="<<validation<<"; synchronization="<<synchronization<<'\n';
        const float priority=1;
        VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
        VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};supported.pNext=&timeline;
        vkGetPhysicalDeviceFeatures2(physical,&supported);
        expect(timeline.timelineSemaphore,"Timeline semaphores required for tile upload test");
        float64=supported.features.shaderFloat64;
        VkPhysicalDeviceFeatures enabledFeatures{};enabledFeatures.shaderFloat64=float64;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; queueInfo.queueFamilyIndex=family; queueInfo.queueCount=1; queueInfo.pQueuePriorities=&priority;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; deviceInfo.queueCreateInfoCount=1; deviceInfo.pQueueCreateInfos=&queueInfo;
        deviceInfo.pNext=&timeline;
        deviceInfo.pEnabledFeatures=&enabledFeatures;
        check(vkCreateDevice(physical,&deviceInfo,nullptr,&device),"Create device"); vkGetDeviceQueue(device,family,0,&queue);
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; poolInfo.queueFamilyIndex=family; poolInfo.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        check(vkCreateCommandPool(device,&poolInfo,nullptr,&pool),"Command pool");
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; allocation.commandPool=pool; allocation.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocation.commandBufferCount=1;
        check(vkAllocateCommandBuffers(device,&allocation,&command),"Command buffer");
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; check(vkCreateFence(device,&fenceInfo,nullptr,&fence),"Fence");
        return true;
    }
    void releaseDevice() {
        if(device) {
            vkDeviceWaitIdle(device);
            if(fence) vkDestroyFence(device,fence,nullptr);
            if(pool) vkDestroyCommandPool(device,pool,nullptr);
            vkDestroyDevice(device,nullptr); device=VK_NULL_HANDLE;
        }
    }
    ~Context() {
        releaseDevice();
        if(messenger) reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT"))(instance,messenger,nullptr);
        if(instance) vkDestroyInstance(instance,nullptr);
    }
};
struct Buffer {
    VulkanBuffer allocation;
    VkBuffer buffer; void* mapped; VkDeviceSize bytes;
    Buffer(Context& context,VkDeviceSize length):allocation(context.physical,context.device,length,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,VulkanBuffer::Memory::Host),
        buffer(allocation.handle()),mapped(allocation.mapped()),bytes(length) {}
    Buffer(const Buffer&)=delete;
    void flush() {allocation.flush();}
    void invalidate() {allocation.invalidate();}
    BlendKernel::Buffer binding() const {return {buffer,bytes};}
};
void runMemoryAdmission(Context& context) {
    constexpr std::uint64_t mib=1024*1024;
    const auto accounted=[](const std::shared_ptr<MemoryAdmission>& memory,std::uint64_t bytes) {
        const auto ledger=memory->snapshot();
        expect(ledger.pendingCpu==0 && ledger.pendingGpu==0 && ledger.committedCpu==0 && ledger.committedGpu==bytes,
               "GPU allocation charge or rollback differs from live backing");
    };
    const auto denied=[](auto action) {
        bool caught=false;
        try {action();} catch(const std::length_error&) {caught=true;}
        expect(caught,"GPU allocation bypassed memory admission");
    };
    unsigned probes=0;
    unsigned failAt=std::numeric_limits<unsigned>::max();
    auto memory=std::make_shared<MemoryAdmission>(32*mib,mib,[&] {
        ++probes;
        return probes>=failAt ? MemorySample{std::nullopt,0} : MemorySample{64*mib,0};
    });
    constexpr VkBufferUsageFlags usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    {
        VulkanBuffer device(context.physical,context.device,33,usage,VulkanBuffer::Memory::Device,memory);
        expect(device.allocationBytes()>=33,"GPU allocation padding is smaller than payload");
        accounted(memory,device.allocationBytes());
        {
            VulkanBuffer host(context.physical,context.device,17,usage,VulkanBuffer::Memory::Host,memory);
            expect(host.mapped()!=nullptr,"Admitted host allocation was not mapped");
            accounted(memory,device.allocationBytes()+host.allocationBytes());
        }
        accounted(memory,device.allocationBytes());
    }
    accounted(memory,0);
    expect(probes==2,"Host/device allocations did not consult shared admission");
    for(const auto kind:{VulkanBuffer::Memory::Host,VulkanBuffer::Memory::Device}) {
        auto unavailable=std::make_shared<MemoryAdmission>(1,0,[]{return MemorySample{64*mib,0};});
        denied([&]{VulkanBuffer buffer(context.physical,context.device,33,usage,kind,unavailable);});
        accounted(unavailable,0);
    }
    probes=0;failAt=3;
    denied([&]{VulkanTileCache cache(context.physical,context.device,context.queue,context.family,TileResidency::slotBytes,memory);});
    expect(probes==3,"Tile staging allocations bypassed admission");
    accounted(memory,0);
    probes=0;failAt=std::numeric_limits<unsigned>::max();
    {
        VulkanTileCache uploads(context.physical,context.device,context.queue,context.family,TileResidency::slotBytes,memory);
        struct Retirement {VkQueue queue;~Retirement(){vkQueueWaitIdle(queue);}} retirement{context.queue};
        const auto base=uploads.allocationBytes();
        expect(probes==3,"Tile pool/staging admission is incomplete");
        accounted(memory,base);
        expect(uploads.begin(),"Tiny admission batch unavailable");
        const auto serial=uploads.submit();const auto timeline=uploads.timeline();
        accounted(memory,base);
        VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wait.semaphoreCount=1;wait.pSemaphores=&timeline;wait.pValues=&serial;
        check(vkWaitSemaphores(context.device,&wait,30000000000ULL),"Retire memory admission batch");
        accounted(memory,base);
        if(context.float64) {
            const auto before=probes;
            {
                GpuMipCache mips(context.physical,context.device,uploads,GpuMipCache::slotBytes,memory);
                expect(probes==before+4,"Mip pool/scratch/parameter/table admission is incomplete");
                const auto charge=memory->snapshot().committedGpu;
                expect(charge>base+GpuMipCache::slotBytes,"Mip intermediates did not retain their charges");
                accounted(memory,charge);
            }
            accounted(memory,base);
            failAt=probes+4;
            denied([&]{GpuMipCache mips(context.physical,context.device,uploads,GpuMipCache::slotBytes,memory);});
            expect(probes==failAt,"Mip parameter/table failure was not reached");
            accounted(memory,base);
            failAt=std::numeric_limits<unsigned>::max();
        }
    }
    accounted(memory,0);
    if(context.float64) {
        probes=0;failAt=11;
        denied([&]{GpuLayerBlock block(context.physical,context.device,context.queue,context.family,
                                      TileResidency::slotBytes,GpuMipCache::slotBytes,memory);});
        expect(probes==11,"Layer parameter/table failure was not reached");
        accounted(memory,0);
        probes=0;failAt=std::numeric_limits<unsigned>::max();
        {
            GpuLayerFrame frame(context.physical,context.device,context.queue,context.family,
                                TileResidency::slotBytes,2*mib,GpuMipCache::slotBytes,memory);
            expect(probes==11,"Frame/block buffer admission is incomplete");
            const auto base=memory->snapshot().committedGpu;
            accounted(memory,base);
            failAt=probes+1;
            denied([&]{frame.start(blankDocument(1,1),{0,0},{1,1},1,1);});
            expect(probes==failAt,"Frame output bypassed admission");
            accounted(memory,base);
            failAt=std::numeric_limits<unsigned>::max();
            frame.start(blankDocument(1,1),{0,0},{1,1},1,1);
            expect(memory->snapshot().committedGpu>=base+frame.outputBytes(),"Frame output has no retained allocation charge");
            frame.cancel();
            expect(frame.advance()==GpuLayerFrame::State::Cancelled,"Unsubmitted memory-test frame did not cancel");
        }
        accounted(memory,0);
    }
    std::cout<<"GPU allocation-size admission, host/device charges, retirement ownership and constructor rollback passed\n";
}
void runSpilledUploads(Context& context,const std::filesystem::path& directory) {
    constexpr auto bytes=TileResidency::slotBytes;
    auto memory=std::make_shared<MemoryAdmission>(64*1024*1024,0,[] {
        return MemorySample{1024*1024*1024,0};
    });
    io::SpillLimits limits;limits.maxBytes=1024*1024;limits.maxPayloadBytes=1024;
    limits.minFreeBytes=0;limits.maxEntries=8;
    auto disk=std::make_shared<io::SpillStore>(directory,limits);
    TileStore store(1024*1024,memory,disk);
    const std::array<Pixel,6> pixels{{{.125f,-.25f,2,.5f},{1,0,0,1},{0,0,0,0},
        {.5f,.25f,.125f,.75f},{0,1,0,1},{.000000059604645f,0,0,.000000059604645f}}};
    const std::array<TilePtr,2> tiles{store.create(2,3,pixels),store.constant(3,2,{2,-1,.125f,.5f})};
    expect(!tiles[0]->uniform() && tiles[1]->uniform(),"Spill fixture representations changed");
    VulkanTileCache cache(context.physical,context.device,context.queue,context.family,bytes);
    Buffer output(context,bytes);
    struct Retirement {VkQueue queue;~Retirement(){vkQueueWaitIdle(queue);}} retirement{context.queue};
    std::stop_source cancelled;cancelled.request_stop();
    for(const auto& tile:tiles) {
        const auto canonical=tile->canonicalBytes();
        if(tile->spillable()) expect(tile->spill() && !tile->spillStatus().resident,"Dense fixture did not evict its resident payload");
        else expect(tile->uniform() && !tile->spill(),"Constant fixture should remain inline");
        rejects([&]{tile->read(cancelled.get_token());});
        std::vector<std::uint8_t> staging(bytes,0xab);
        rejects([&]{encodeTileUpload(*tile,staging,cancelled.get_token());});
        expect(!tile->spillStatus().resident,"Cancelled encoding rehydrated canonical bytes");
        expect(cache.begin(),"Spilled cache batch unavailable");
        rejects([&]{cache.acquire(tile,cancelled.get_token());});cache.cancel();
        expect(cache.begin(),"Spilled cache batch did not recover after cancellation");
        const auto lease=cache.acquire(tile);
        expect(lease && lease->upload,"Spilled cache did not stage a new lease");
        const auto command=cache.consumers();
        const VkBufferCopy copy{lease->slot*bytes,0,bytes};
        vkCmdCopyBuffer(command,cache.buffer(),output.buffer,1,&copy);
        VkMemoryBarrier ready{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        ready.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;ready.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&ready,0,nullptr,0,nullptr);
        const auto serial=cache.submit();const auto timeline=cache.timeline();
        VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wait.semaphoreCount=1;wait.pSemaphores=&timeline;wait.pValues=&serial;
        check(vkWaitSemaphores(context.device,&wait,30000000000ULL),"Wait spilled staging readback");output.invalidate();
        const auto* actual=static_cast<const std::uint8_t*>(output.mapped);
        for(std::size_t y=0;y<256;++y) for(std::size_t x=0;x<256;++x) for(std::size_t c=0;c<8;++c) {
            const auto expected=y<static_cast<std::size_t>(tile->height()) && x<static_cast<std::size_t>(tile->width())
                ? canonical[(y*static_cast<std::size_t>(tile->width())+x)*8+c] : 0;
            expect(actual[(y*256+x)*8+c]==expected,"Spilled GPU staging differs from canonical half bits");
        }
        if(tile->spillable()) expect(tile->spill() && !tile->spillStatus().resident,"Staging retained a canonical read lease");
        else expect(tile->uniform() && !tile->spill(),"Staging changed inline constant representation");
        expect(cache.begin(),"Spilled cache hit batch unavailable");
        rejects([&]{cache.acquire(tile,cancelled.get_token());});cache.cancel();
        expect(cache.begin(),"Spilled cache hit did not recover after cancellation");
        const auto hit=cache.acquire(tile);expect(hit && !hit->upload,"Fresh spilled cache hit uploaded again");
        cache.cancel();
        expect(!tile->spillStatus().resident,"GPU cache hit unnecessarily rehydrated canonical bytes");
    }
    std::cout<<"Spilled dense and inline constant GPU uploads preserve exact half bits; cancellation and fresh cache acquisition passed\n";
}
void runUploads(Context& context) {
    constexpr std::size_t bytes=TileResidency::slotBytes;
    Buffer staging(context,bytes+16),pool(context,bytes*3);
    VulkanBuffer devicePool(context.physical,context.device,bytes*3,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VulkanBuffer::Memory::Device);
    expect(!devicePool.mapped() && devicePool.allocationBytes()>=bytes*3,"Invalid device allocation");
    rejects([&]{devicePool.flush();});
    rejects([&]{devicePool.invalidate();});
    struct Timeline {
        VkDevice device; VkSemaphore handle=VK_NULL_HANDLE;
        explicit Timeline(VkDevice d):device(d) {
            VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};type.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE;
            VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};info.pNext=&type;
            check(vkCreateSemaphore(device,&info,nullptr,&handle),"Create upload timeline");
        }
        ~Timeline(){vkDestroySemaphore(device,handle,nullptr);}
    } timeline(context.device);
    struct Retirement {VkQueue queue;~Retirement(){vkQueueWaitIdle(queue);}} retirement{context.queue};
    std::uint64_t serial=0;
    TileStore store(4*1024*1024);
    std::vector<Pixel> pixels(256*256);
    for(std::size_t i=0;i<pixels.size();++i)
        pixels[i]={float(i%257)/256, -.125f, 2, .5f};
    const std::array<TilePtr,3> tiles{store.constant(1,3,{65504,-65504,0,.5f}),
        store.create(256,256,pixels),store.constant(255,253,{0.000000059604645f,0,0,0.000000059604645f})};
    for(const auto& tile:tiles) {
        std::memset(staging.mapped,0xab,bytes+16);
        auto encoded=std::span(static_cast<std::uint8_t*>(staging.mapped)+8,bytes);
        encodeTileUpload(*tile,encoded);
        // Independent byte oracle uses the compact canonical tile payload;
        // row padding in the upload must be zero, never old staging contents.
        const auto canonical=tile->canonicalBytes();
        for(std::size_t y=0;y<256;++y) for(std::size_t x=0;x<256;++x)
            for(std::size_t c=0;c<8;++c) {
                const auto expected=y<static_cast<std::size_t>(tile->height()) && x<static_cast<std::size_t>(tile->width())
                    ? canonical[(y*static_cast<std::size_t>(tile->width())+x)*8+c] : 0;
                expect(encoded[(y*256+x)*8+c]==expected,"Upload packing mismatch");
            }
        expect(static_cast<std::uint8_t*>(staging.mapped)[7]==0xab &&
               static_cast<std::uint8_t*>(staging.mapped)[bytes+8]==0xab,"Upload packing overrun");
        rejects([&]{encodeTileUpload(*tile,encoded.first(bytes-1));});
        staging.flush();
        std::memset(pool.mapped,0xcd,bytes*3);pool.flush();
        check(vkResetCommandBuffer(context.command,0),"Reset upload command");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(vkBeginCommandBuffer(context.command,&begin),"Begin upload command");
        TileUploadBuffer source{staging.buffer,staging.bytes},destination{devicePool.handle(),devicePool.bytes()};
        rejects([&]{recordTileUpload(context.command,source,9,destination,1);});
        rejects([&]{recordTileUpload(context.command,source,20,destination,1);});
        rejects([&]{recordTileUpload(context.command,source,8,destination,3);});
        rejects([&]{recordTileUpload(context.command,source,8,destination,std::numeric_limits<std::size_t>::max());});
        rejects([&]{recordTileUpload(context.command,source,8,source,0);});
        rejects([&]{recordTileUpload(VK_NULL_HANDLE,source,8,destination,1);});
        vkCmdFillBuffer(context.command,devicePool.handle(),0,VK_WHOLE_SIZE,0xcdcdcdcdu);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(context.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        recordTileUpload(context.command,source,8,destination,1);
        barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(context.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        const VkBufferCopy readback{0,0,bytes*3};
        vkCmdCopyBuffer(context.command,devicePool.handle(),pool.buffer,1,&readback);
        barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(context.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        check(vkEndCommandBuffer(context.command),"End upload command");
        check(vkResetFences(context.device,1,&context.fence),"Reset upload fence");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&context.command;
        ++serial;
        VkTimelineSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        signal.signalSemaphoreValueCount=1;signal.pSignalSemaphoreValues=&serial;
        submit.pNext=&signal;submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&timeline.handle;
        check(vkQueueSubmit(context.queue,1,&submit,context.fence),"Submit upload");
        check(vkWaitForFences(context.device,1,&context.fence,VK_TRUE,30000000000ULL),"Wait upload");
        std::uint64_t completed=0;
        check(vkGetSemaphoreCounterValue(context.device,timeline.handle,&completed),"Observe upload timeline");
        expect(completed==serial,"Upload completion timeline mismatch");
        pool.invalidate();
        const auto* actual=static_cast<const std::uint8_t*>(pool.mapped);
        expect(std::memcmp(actual+bytes,encoded.data(),bytes)==0,"GPU tile transfer mismatch");
        for(std::size_t i=0;i<bytes;++i)
            expect(actual[i]==0xcd && actual[2*bytes+i]==0xcd,"GPU transfer overwrote adjacent slot");
    }
    std::cout<<"3 canonical tile uploads passed exact GPU byte/padding/adjacent-slot checks\n";
}
// Deterministically keep submissions in flight, independent of GPU speed.
// Declare after the resources it protects so unwinding retires them first.
struct QueueGate {
    Context& context;VkSemaphore semaphore=VK_NULL_HANDLE;bool opened=false;
    explicit QueueGate(Context& ctx):context(ctx) {
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};type.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};info.pNext=&type;
        check(vkCreateSemaphore(context.device,&info,nullptr,&semaphore),"Create queue gate");
    }
    void block() {
        const std::uint64_t value=1;const VkPipelineStageFlags stage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};timeline.waitSemaphoreValueCount=1;timeline.pWaitSemaphoreValues=&value;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.pNext=&timeline;submit.waitSemaphoreCount=1;submit.pWaitSemaphores=&semaphore;submit.pWaitDstStageMask=&stage;
        check(vkQueueSubmit(context.queue,1,&submit,VK_NULL_HANDLE),"Block test queue");
    }
    void open() {
        VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};signal.semaphore=semaphore;signal.value=1;
        check(vkSignalSemaphore(context.device,&signal),"Open queue gate");opened=true;
    }
    ~QueueGate() {
        if(!opened) {VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};signal.semaphore=semaphore;signal.value=1;vkSignalSemaphore(context.device,&signal);}
        vkQueueWaitIdle(context.queue);vkDestroySemaphore(context.device,semaphore,nullptr);
    }
};
void runCache(Context& context) {
    constexpr std::size_t bytes=TileResidency::slotBytes;
    VulkanTileCache cache(context.physical,context.device,context.queue,context.family,bytes*2);
    Buffer readback(context,bytes*3);
    expect(cache.allocationBytes()>=bytes*6,"Cache allocation accounting mismatch");
    TileStore store(4*1024*1024);
    auto a=store.constant(256,256,{1,0,0,1}),b=store.constant(3,5,{0,.5f,0,.5f}),c=store.constant(1,1,{2,-1,0,.5f});
    rejects([&]{VulkanTileCache invalid(context.physical,context.device,context.queue,context.family,bytes-1);});
    rejects([&]{VulkanTileCache invalid(context.physical,context.device,context.queue,context.family,std::numeric_limits<std::size_t>::max());});
    // Gate the queue with a host-signaled timeline. Destruction opens the gate
    // and retires the queue even if an assertion fails, before cache teardown.
    QueueGate gate(context);
    rejects([&]{cache.acquire(a);});
    rejects([&]{cache.submit();});
    expect(cache.begin(),"Initial cache batch unavailable");
    rejects([&]{cache.begin();});
    expect(cache.acquire(a)->upload,"Initial tile not uploaded");
    cache.cancel();
    expect(cache.lastSubmitted()==0,"Cancellation advanced timeline");
    gate.block();
    auto copy=[&](std::size_t slot,std::size_t output) {
        const auto command=cache.consumers();
        const VkBufferCopy region{slot*bytes,output*bytes,bytes};
        vkCmdCopyBuffer(command,cache.buffer(),readback.buffer,1,&region);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    };
    expect(cache.begin(),"First staging slot unavailable");
    auto first=cache.acquire(a);expect(first && first->upload,"Cancelled upload incorrectly cached");
    expect(!cache.acquire(a)->upload && cache.uploadCount()==1,"Shared tile redundantly uploaded");
    copy(first->slot,0);
    rejects([&]{cache.acquire(b);});
    expect(cache.submit()==1,"First cache serial mismatch");
    expect(cache.begin(),"Second staging slot unavailable");
    auto second=cache.acquire(b);expect(second && second->upload,"Second tile upload missing");
    expect(!cache.acquire(a)->upload,"In-flight immutable tile not reused");
    expect(!cache.acquire(c),"Cache evicted reserved or in-flight tile");
    copy(second->slot,1);copy(first->slot,2);
    expect(cache.submit()==2,"Second cache serial mismatch");
    expect(!cache.begin(),"Busy staging ring was reused");
    gate.open();
    auto wait=[&](std::uint64_t value) {
        const auto semaphore=cache.timeline();
        VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};info.semaphoreCount=1;info.pSemaphores=&semaphore;info.pValues=&value;
        check(vkWaitSemaphores(context.device,&info,30000000000ULL),"Wait cache timeline");
    };
    auto compare=[&](const TilePtr& tile,std::size_t output) {
        std::vector<std::uint8_t> expected(bytes);encodeTileUpload(*tile,expected);
        expect(std::memcmp(static_cast<const std::uint8_t*>(readback.mapped)+output*bytes,expected.data(),bytes)==0,"Resident tile bytes mismatch");
    };
    wait(2);readback.invalidate();compare(a,0);compare(b,1);compare(a,2);
    expect(cache.begin(),"Retired staging slot unavailable");
    expect(!cache.acquire(a)->upload,"Retired tile lost residency");
    auto third=cache.acquire(c);expect(third && third->upload && third->slot==second->slot,"Retired eviction chose reserved tile");
    cache.cancel();
    expect(cache.begin(),"Cancelled staging slot unavailable");
    expect(!cache.acquire(a)->upload,"Cancel discarded existing resident tile");
    third=cache.acquire(c);expect(third && third->upload,"Cancelled replacement treated as uploaded");
    copy(third->slot,0);copy(first->slot,1);
    expect(cache.submit()==3,"Replacement cache serial mismatch");
    wait(3);readback.invalidate();compare(c,0);compare(a,1);
    expect(cache.begin(),"Cached-only batch unavailable");
    expect(!cache.acquire(c)->upload && !cache.acquire(a)->upload && cache.uploadCount()==0,"Cached-only batch uploaded pixels");
    copy(third->slot,0);copy(first->slot,1);
    expect(cache.submit()==4,"Cached-only serial mismatch");
    wait(4);readback.invalidate();compare(c,0);compare(a,1);
    std::cout<<"GPU tile cache passed blocked-queue staging, sharing, pressure, cancellation and retired reuse checks\n";
}
void runSampling(Context& context) {
    if(!context.float64) {std::cout<<"SKIP sampling: shaderFloat64 unavailable; CPU fallback required\n";return;}
    const Extent extent{-259,-3,520,260};
    TileStore store(16*1024*1024);TileMap tiles;
    for(std::int64_t y=-1;y<=1;++y) for(std::int64_t x=-2;x<=1;++x) {
        if(x==0 && y==0) continue; // Real missing tile uses the asset default.
        const auto region=tileExtent(extent,{x,y});
        std::vector<Pixel> pixels(static_cast<std::size_t>(region.width*region.height));
        for(std::int64_t py=0;py<region.height;++py) for(std::int64_t px=0;px<region.width;++px) {
            const float alpha=(px+py)%7==0 ? 0.f : .5f;
            pixels[static_cast<std::size_t>(py*region.width+px)]={float((px+region.x+1024)%31)/16*alpha,-.25f*alpha,2*alpha,alpha};
        }
        tiles[{x,y}]=store.create(static_cast<int>(region.width),static_cast<int>(region.height),pixels);
    }
    RasterSnapshot raster(Id::generate(),extent,pack({.25f,.125f,0,.5f}),tiles);
    VulkanTileCache cache(context.physical,context.device,context.queue,context.family,12*TileResidency::slotBytes);
    SampleKernel kernel(context.device);
    Buffer table(context,12*16),parameters(context,sizeof(SampleParameters)),output(context,(65536+1)*sizeof(Pixel));
    struct Retirement {VkQueue queue;~Retirement(){vkQueueWaitIdle(queue);}} retirement{context.queue};
    std::uint64_t compared=0;double maximum=0;
    const std::array<Affine,5> transforms{{{}, {1,0,0,1,.125,-.375}, {.8,.6,-.6,.8,5,-7}, {-1,0,0,.5,0,0}, {1,.125,.25,1,-13,11}}};
    for(const auto& transform:transforms) for(int view=0;view<4;++view) for(std::uint32_t mode=0;mode<3;++mode) {
        if(view==3 && mode!=2) continue;
        SampleParameters p{};p.inverse=transform.inverse();p.origin={-270.25,-10.125};p.step={8.5,4.75};
        if(view==1) {p.origin={-257.0000001,-1.0000001};p.step={.03125,.0625};}
        if(view==2) {p.origin={-.5,.5};p.step={.125,.25};}
        p.extent={-259,-3,520,260};p.grid={-2,-1,4,3};p.defaultPixel=unpack(raster.defaultValue);
        p.width=65;p.height=63;p.sampling=mode;p.opacity=view==2 ? .375f : 1.f;
        if(view==2) {p.width=256;p.height=256;}
        if(mode==2) {
            p.width=13;p.height=11;
            p.footprint=view==0 ? Coordinate{1,1} : view==1 ? Coordinate{2,2} : view==2 ? Coordinate{1.5,4.25} : Coordinate{16,16};
        }
        p.validate();
        auto invalid=p;invalid.width=257;rejects([&]{invalid.validate();});
        invalid=p;invalid.grid[2]++;rejects([&]{invalid.validate();});
        invalid=p;invalid.opacity=std::numeric_limits<float>::quiet_NaN();rejects([&]{invalid.validate();});
        invalid=p;invalid.extent[0]=std::numeric_limits<std::int32_t>::min();rejects([&]{invalid.validate();});
        invalid=p;invalid.footprint.x=17;rejects([&]{invalid.validate();});
        invalid=p;invalid.footprint.y=std::numeric_limits<double>::quiet_NaN();rejects([&]{invalid.validate();});
        invalid=p;invalid.fp32Source=2;rejects([&]{invalid.validate();});
        expect(cache.begin(),"Sampling staging did not retire");
        std::array<std::array<std::int32_t,4>,12> entries{};
        for(auto& entry:entries) entry[0]=-1;
        for(const auto& [coordinate,tile]:tiles) {
            const auto lease=cache.acquire(tile);expect(lease.has_value(),"Sampling tile cache too small");
            const auto region=tileExtent(extent,coordinate);
            entries[static_cast<std::size_t>((coordinate.y+1)*4+coordinate.x+2)]={static_cast<std::int32_t>(lease->slot),static_cast<std::int32_t>(region.x),static_cast<std::int32_t>(region.y),0};
        }
        std::memcpy(table.mapped,entries.data(),sizeof(entries));table.flush();
        std::memcpy(parameters.mapped,&p,sizeof(p));parameters.flush();
        std::memset(output.mapped,0xcd,static_cast<std::size_t>(output.bytes));output.flush();
        const auto command=cache.consumers();
        rejects([&]{kernel.record(command,2,{cache.buffer(),cache.bytes()},{table.buffer,table.bytes},
                      {parameters.buffer,parameters.bytes},{output.buffer,output.bytes},p);});
        rejects([&]{kernel.record(command,0,{cache.buffer(),cache.bytes()},{table.buffer,table.bytes-1},
                      {parameters.buffer,parameters.bytes},{output.buffer,output.bytes},p);});
        kernel.record(command,0,{cache.buffer(),cache.bytes()},{table.buffer,table.bytes},
                      {parameters.buffer,parameters.bytes},{output.buffer,output.bytes},p);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        const auto serial=cache.submit();const auto timeline=cache.timeline();
        VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};wait.semaphoreCount=1;wait.pSemaphores=&timeline;wait.pValues=&serial;
        check(vkWaitSemaphores(context.device,&wait,30000000000ULL),"Wait sample result");output.invalidate();
        for(std::uint32_t y=0;y<p.height;++y) for(std::uint32_t x=0;x<p.width;++x) {
            const auto source=p.inverse.map({p.origin.x+x*p.step.x,p.origin.y+y*p.step.y});
            auto expected=mode==2 ? sampleLanczos([&](auto px,auto py){return raster.pixel(px,py);},source.x,source.y,p.footprint.x,p.footprint.y)
                                  : sample(raster,source.x,source.y,static_cast<Sampling>(mode));
            expected={expected.r*p.opacity,expected.g*p.opacity,expected.b*p.opacity,expected.a*p.opacity};
            Pixel actual;std::memcpy(&actual,static_cast<const char*>(output.mapped)+(y*p.width+x)*sizeof(Pixel),sizeof(Pixel));
            const std::array<float,4> a{actual.r,actual.g,actual.b,actual.a},b{expected.r,expected.g,expected.b,expected.a};
            for(std::size_t channel=0;channel<4;++channel) {
                const auto error=std::abs(double(a[channel])-b[channel]);maximum=std::max(maximum,error);
                if(!std::isfinite(a[channel]) || error>std::max(.002,.002*std::abs(double(b[channel]))))
                    throw std::runtime_error("GPU affine sampling mismatch mode="+std::to_string(mode)+" view="+std::to_string(view)+
                        " x="+std::to_string(x)+" y="+std::to_string(y)+" source="+std::to_string(source.x)+","+std::to_string(source.y)+
                        " channel="+std::to_string(channel)+" actual="+std::to_string(a[channel])+" expected="+std::to_string(b[channel]));
            }
            ++compared;
        }
        const auto* tail=static_cast<const std::uint8_t*>(output.mapped)+p.width*p.height*sizeof(Pixel);
        for(std::size_t i=0;i<sizeof(Pixel);++i) expect(tail[i]==0xcd,"Sample dispatch overwrote tail");
    }
    std::cout<<compared<<" GPU/CPU affine nearest/bilinear/Lanczos samples; max absolute error="<<maximum<<'\n';
}
void runMipSampling(Context& context) {
    if(!context.float64) return;
    constexpr int width=65,height=33;
    TileStore store(1024*1024);std::vector<Pixel> pixels;
    for(int y=0;y<height;++y) for(int x=0;x<width;++x) {
        const float alpha=(x+y)%5==0 ? 0.f : (x%3==0 ? 1.f : .375f);
        pixels.push_back({float(x%11)*alpha/7,-float(y%7)*alpha/5,alpha*2,alpha});
    }
    const auto tile=store.create(width,height,pixels);
    RasterSnapshot raster(Id::generate(),{0,0,width,height},{},{{{0,0},tile}});
    Buffer source(context,TileResidency::slotBytes),first(context,65536*sizeof(Pixel)),second(context,65536*sizeof(Pixel));
    Buffer table(context,16),parametersA(context,sizeof(SampleParameters)),parametersB(context,sizeof(SampleParameters));
    encodeTileUpload(*tile,{static_cast<std::uint8_t*>(source.mapped),static_cast<std::size_t>(source.bytes)});source.flush();
    const std::array<std::int32_t,4> entry{0,0,0,0};std::memcpy(table.mapped,entry.data(),16);table.flush();
    SampleParameters a{};a.origin={1,1};a.step={2,2};a.extent={0,0,width,height};a.grid={0,0,1,1};
    a.width=256;a.height=256;a.sampling=2;a.opacity=1;a.footprint={2,2};
    auto b=a;b.extent={0,0,(width+1)/2,(height+1)/2};b.fp32Source=1;
    std::memcpy(parametersA.mapped,&a,sizeof(a));parametersA.flush();std::memcpy(parametersB.mapped,&b,sizeof(b));parametersB.flush();
    SampleKernel kernel(context.device);
    struct Retirement {VkQueue queue;~Retirement(){vkQueueWaitIdle(queue);}} retirement{context.queue};
    check(vkResetCommandBuffer(context.command,0),"Reset mip command");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};check(vkBeginCommandBuffer(context.command,&begin),"Begin mip command");
    kernel.record(context.command,0,{source.buffer,source.bytes},{table.buffer,table.bytes},
                  {parametersA.buffer,parametersA.bytes},{first.buffer,first.bytes},a);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(context.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    rejects([&]{kernel.record(context.command,1,{first.buffer,first.bytes-1},{table.buffer,table.bytes},
                  {parametersB.buffer,parametersB.bytes},{second.buffer,second.bytes},b);});
    kernel.record(context.command,1,{first.buffer,first.bytes},{table.buffer,table.bytes},
                  {parametersB.buffer,parametersB.bytes},{second.buffer,second.bytes},b);
    barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(context.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    check(vkEndCommandBuffer(context.command),"End mip command");check(vkResetFences(context.device,1,&context.fence),"Reset mip fence");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&context.command;
    check(vkQueueSubmit(context.queue,1,&submit,context.fence),"Submit mip chain");
    check(vkWaitForFences(context.device,1,&context.fence,VK_TRUE,30000000000ULL),"Wait mip chain");second.invalidate();
    const auto cpuFirst=halveRegion([&](auto x,auto y){return raster.pixel(x,y);},width,height,{0,0,33,17});
    const PixelReader parent=[&](auto x,auto y){return x>=0 && y>=0 && x<33 && y<17 ? cpuFirst->pixels[static_cast<std::size_t>(y*33+x)] : Pixel{};};
    const auto cpuSecond=halveRegion(parent,33,17,{0,0,17,9});double maximum=0;
    for(int y=0;y<9;++y) for(int x=0;x<17;++x) {
        Pixel pixel;std::memcpy(&pixel,static_cast<const char*>(second.mapped)+static_cast<std::size_t>(y*256+x)*sizeof(Pixel),sizeof(Pixel));
        const auto expected=cpuSecond->pixels[static_cast<std::size_t>(y*17+x)];
        const std::array<float,4> actual{pixel.r,pixel.g,pixel.b,pixel.a},reference{expected.r,expected.g,expected.b,expected.a};
        for(std::size_t c=0;c<4;++c) {
            maximum=std::max(maximum,std::abs(double(actual[c])-reference[c]));
            expect(std::isfinite(actual[c]) && std::abs(actual[c]-reference[c])<=.003f,"Successive GPU mip levels differ from scalar FP32 chain");
        }
    }
    std::cout<<"153 successive GPU mip pixels; FP32 intermediate, odd dimensions, transparent parent exterior; max error="<<maximum<<'\n';
}
void runMipCache(Context& context) {
    if(!context.float64) return;
    const Extent extent{-2,-3,520,260};TileStore store(8*1024*1024);TileMap tiles;
    for(auto y=floorTile(extent.y);y<=floorTile(extent.y+extent.height-1);++y)
        for(auto x=floorTile(extent.x);x<=floorTile(extent.x+extent.width-1);++x) {
            const auto piece=tileExtent(extent,{x,y});
            if(x==1 && y==0) continue;
            tiles[{x,y}]=store.constant(static_cast<int>(piece.width),static_cast<int>(piece.height),{float(x+2)/4,.25f,-.125f,.75f});
        }
    auto source=std::make_shared<const RasterSnapshot>(Id::generate(),extent,pack({.125f,.25f,0,.5f}),tiles);
    Buffer readback(context,GpuMipCache::slotBytes);
    MipCache cpu(8*1024*1024);double maximum=0;std::size_t compared=0;
    for(std::size_t slots:{8u,3u}) {
        VulkanTileCache uploads(context.physical,context.device,context.queue,context.family,4*TileResidency::slotBytes);
        GpuMipCache mip(context.physical,context.device,uploads,slots*GpuMipCache::slotBytes);
        struct Retirement {VkQueue queue;~Retirement(){vkQueueWaitIdle(queue);}} retirement{context.queue};
        auto wait=[&] {
            const auto serial=uploads.lastSubmitted();const auto timeline=uploads.timeline();
            VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};info.semaphoreCount=1;info.pSemaphores=&timeline;info.pValues=&serial;
            check(vkWaitSemaphores(context.device,&info,30000000000ULL),"Wait mip cache test");
        };
        auto finish=[&] {
            for(int attempt=0;attempt<10000;++attempt) {
                const auto state=mip.advance();if(state!=GpuMipCache::State::Pending) return state;wait();
            }
            throw std::runtime_error("Mip cache made no progress");
        };
        auto read=[&](TileCoord coordinate) {
            expect(uploads.begin(),"Mip readback batch unavailable");
            const auto command=uploads.consumers();const VkBufferCopy copy{mip.lease(coordinate)*GpuMipCache::slotBytes,0,GpuMipCache::slotBytes};
            vkCmdCopyBuffer(command,mip.buffer(),readback.buffer,1,&copy);
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
            uploads.submit();wait();readback.invalidate();
        };
        mip.start(source,2,{{0,0}});rejects([&]{mip.lease({0,0});});
        rejects([&]{mip.start(source,2,{{0,0}});});
        expect(finish()==GpuMipCache::State::Complete,"Mip generation failed within three-slot dependency budget");
        read({0,0});
        for(int y=0;y<65;y+=4) for(int x=0;x<130;x+=3) {
            Pixel actual;std::memcpy(&actual,static_cast<const char*>(readback.mapped)+static_cast<std::size_t>(y*256+x)*sizeof(Pixel),sizeof(Pixel));
            const auto expected=*cpu.pixel(source,2,x,y);
            const std::array<float,4> a{actual.r,actual.g,actual.b,actual.a},b{expected.r,expected.g,expected.b,expected.a};
            for(std::size_t c=0;c<4;++c) {
                maximum=std::max(maximum,std::abs(double(a[c])-b[c]));
                expect(std::isfinite(a[c]) && std::abs(a[c]-b[c])<=.003f,"Mip cache GPU/CPU mismatch");
            }
            ++compared;
        }
        const auto serial=uploads.lastSubmitted(),generated=mip.generatedPieces();
        mip.start(source,2,{{0,0},{0,0}});
        expect(finish()==GpuMipCache::State::Complete && uploads.lastSubmitted()==serial && mip.generatedPieces()==generated,"Mip cache hit submitted new work");
        mip.cancel();expect(mip.state()==GpuMipCache::State::Cancelled,"Completed mip cancellation kept a published request");
        rejects([&]{mip.lease({0,0});});
        auto replacement=std::make_shared<const RasterSnapshot>(source->id,extent,PackedPixel{},TileMap{},source->revision);
        mip.start(replacement,1,{{1,0}});expect(finish()==GpuMipCache::State::Complete,"Replacement mip failed");read({1,0});
        Pixel zero;std::memcpy(&zero,readback.mapped,sizeof(Pixel));expect(zero==Pixel{},"Mip source identity aliased equal ID/revision");
        mip.start(source,1,{{0,0},{1,0}});expect(finish()==GpuMipCache::State::Complete,"Multiple requested mip pieces failed");
        expect(mip.lease({0,0})!=mip.lease({1,0}),"Simultaneous mip leases alias");read({1,0});
        Pixel edge;std::memcpy(&edge,readback.mapped,sizeof(Pixel));const auto expectedEdge=*cpu.pixel(source,1,256,0);
        expect(std::abs(edge.r-expectedEdge.r)<.003f && std::abs(edge.a-expectedEdge.a)<.003f,"Pinned edge mip piece was overwritten");
        mip.start(source,1,{{0,0}});mip.cancel();expect(finish()==GpuMipCache::State::Cancelled,"Pre-submit mip cancellation published output");
        mip.start(replacement,2,{{0,0}});
        const auto beforeCancel=uploads.lastSubmitted();
        for(int attempt=0;attempt<100 && uploads.lastSubmitted()==beforeCancel;++attempt) mip.advance();
        expect(uploads.lastSubmitted()>beforeCancel,"Cancellation fixture did not submit mip work");
        mip.cancel();
        expect(finish()==GpuMipCache::State::Cancelled,"In-progress mip cancellation published output");rejects([&]{mip.lease({0,0});});
        expect(mip.bytes()==slots*GpuMipCache::slotBytes && mip.residentPieces()<=slots,"Mip cache exceeded residency budget");
    }
    {
        VulkanTileCache uploads(context.physical,context.device,context.queue,context.family,4*TileResidency::slotBytes);
        GpuMipCache tiny(context.physical,context.device,uploads,GpuMipCache::slotBytes);
        struct Retirement {VkQueue queue;~Retirement(){vkQueueWaitIdle(queue);}} retirement{context.queue};
        tiny.start(source,2,{{0,0}});
        expect(tiny.advance()==GpuMipCache::State::NeedsSpace,"Unsatisfiable mip dependency budget was not reported");
        rejects([&]{tiny.lease({0,0});});
    }
    std::cout<<compared<<" bounded GPU mip-cache samples; max error="<<maximum<<"; reuse, identity, cancellation and pressure passed\n";
}
void runLayerBlocks(Context& context) {
    if(!context.float64) return;
    TileStore store(16*1024*1024);
    const Extent extent{-2,-3,520,260};
    auto raster=[&](bool alternate) {
        TileMap tiles;
        for(std::int64_t y=-1;y<=1;++y) for(std::int64_t x=-1;x<=2;++x) {
            if(x==1 && y==0) continue;
            const auto region=tileExtent(extent,{x,y});
            const Pixel value=alternate ? Pixel{.125f,.375f,-.125f,.5f} : Pixel{.75f,.0625f,.25f,.75f};
            tiles[{x,y}]=store.constant(static_cast<int>(region.width),static_cast<int>(region.height),value);
        }
        return std::make_shared<const RasterSnapshot>(Id::generate(),extent,pack(alternate ? Pixel{.25f,0,.5f,.5f}:Pixel{0,.5f,0,.5f}),tiles);
    };
    auto a=raster(false),b=raster(true);
    GpuLayerBlock block(context.physical,context.device,context.queue,context.family,16*TileResidency::slotBytes);
    Buffer output(context,256*256*sizeof(Pixel));
    struct Retirement {VkQueue queue;~Retirement(){vkQueueWaitIdle(queue);}} retirement{context.queue};
    auto finish=[&](GpuLayerBlock& renderer) {
        for(int attempts=0;attempts<100;++attempts) {
            auto state=renderer.advance();
            if(state!=GpuLayerBlock::State::Pending) return state;
            const auto serial=renderer.lastSubmitted();const auto timeline=renderer.timeline();
            VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};wait.semaphoreCount=1;wait.pSemaphores=&timeline;wait.pValues=&serial;
            check(vkWaitSemaphores(context.device,&wait,30000000000ULL),"Wait layer block test");
        }
        throw std::runtime_error("Layer block did not finish");
    };
    auto read=[&](GpuLayerBlock& renderer,std::size_t count) {
        check(vkResetCommandBuffer(context.command,0),"Reset block readback");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};check(vkBeginCommandBuffer(context.command,&begin),"Begin block readback");
        VkBufferCopy copy{0,0,count*sizeof(Pixel)};vkCmdCopyBuffer(context.command,renderer.output(),output.buffer,1,&copy);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(context.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        check(vkEndCommandBuffer(context.command),"End block readback");check(vkResetFences(context.device,1,&context.fence),"Reset block readback fence");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&context.command;
        check(vkQueueSubmit(context.queue,1,&submit,context.fence),"Submit block readback");
        check(vkWaitForFences(context.device,1,&context.fence,VK_TRUE,30000000000ULL),"Wait block readback");output.invalidate();
    };
    std::uint64_t compared=0;double maximum=0;
    for(int mode=0;mode<static_cast<int>(blendModeCount);++mode) {
        std::vector<LayerNode> nodes;
        nodes.push_back({Id::generate(),{},0,false,true,1,BlendMode::Normal,{},Sampling::Bilinear,a});
        const auto outer=Id::generate(),inner=Id::generate();
        nodes.push_back({outer,{},1,true,true,.5f});
        nodes.push_back({inner,outer,0,true,true,.75f});
        nodes.push_back({Id::generate(),inner,0,false,true,.625f,static_cast<BlendMode>(mode),{.8,.6,-.6,.8,11,-7},Sampling::Bilinear,b});
        nodes.push_back({Id::generate(),{},2,false,true,.25f,BlendMode::Screen,{-1,0,0,1,257,0},Sampling::Nearest,a});
        const auto folder=Id::generate();nodes.push_back({folder,{},3,true,false});
        nodes.push_back({Id::generate(),folder,0,false,true,1,BlendMode::Normal,{},Sampling::Bilinear,b});
        LayerStack stack(std::move(nodes));
        const Coordinate origin{-3.125,-2.25},step{4.5,4.75};
        block.start(stack,origin,step,65,63);
        expect(block.layerCount()==3,"GPU plan ignored inherited visibility");
        rejects([&]{block.start(stack,origin,step,65,63);});rejects([&]{block.output();});
        expect(finish(block)==GpuLayerBlock::State::Complete,"Layer block failed to complete");read(block,65*63);
        for(std::uint32_t y=0;y<63;++y) for(std::uint32_t x=0;x<65;++x) {
            const auto expected=stack.evaluate(origin.x+x*step.x,origin.y+y*step.y);
            Pixel actual;std::memcpy(&actual,static_cast<const char*>(output.mapped)+(y*65+x)*sizeof(Pixel),sizeof(Pixel));
            const std::array<float,4> values{actual.r,actual.g,actual.b,actual.a},reference{expected.r,expected.g,expected.b,expected.a};
            for(std::size_t channel=0;channel<4;++channel) {
                const auto error=std::abs(double(values[channel])-reference[channel]);maximum=std::max(maximum,error);
                expect(std::isfinite(values[channel]) && error<=std::max(.002,.002*std::abs(double(reference[channel]))),"Layered GPU/CPU mismatch");
            }
            ++compared;
        }
    }
    LayerStack empty({});block.start(empty,{0,0},{1,1},65,63);
    expect(finish(block)==GpuLayerBlock::State::Complete,"Empty stack failed");read(block,65*63);
    for(std::size_t i=0;i<65*63*sizeof(Pixel);++i) expect(static_cast<const std::uint8_t*>(output.mapped)[i]==0,"Empty stack not transparent");
    LayerStack single({LayerNode{Id::generate(),{},0,false,true,1,BlendMode::Normal,{},Sampling::Bilinear,a}});
    block.start(single,{0,0},{1,1},65,63);block.cancel();
    expect(finish(block)==GpuLayerBlock::State::Cancelled,"Pre-submit cancellation failed");rejects([&]{block.output();});
    block.start(single,{0,0},{1,1},65,63);block.advance();block.cancel();
    expect(finish(block)==GpuLayerBlock::State::Cancelled,"In-flight cancellation published output");
    // Artificially tiny cache must request subdivision, not silently lose tiles.
    GpuLayerBlock small(context.physical,context.device,context.queue,context.family,TileResidency::slotBytes);
    struct SmallRetirement {VkQueue queue;~SmallRetirement(){vkQueueWaitIdle(queue);}} smallRetirement{context.queue};
    small.start(single,{-1,-1},{3,3},256,256);
    expect(finish(small)==GpuLayerBlock::State::NeedsSplit,"Low-budget block did not request splitting");rejects([&]{small.output();});
    small.start(single,{20,20},{.1,.1},8,8);
    expect(finish(small)==GpuLayerBlock::State::Complete,"Small block did not recover after pressure");
    read(small,64);
    for(std::size_t i=0;i<64;++i) {
        Pixel pixel;std::memcpy(&pixel,static_cast<const char*>(output.mapped)+i*sizeof(Pixel),sizeof(Pixel));
        expect(pixel==single.evaluate(20+double(i%8)*.1,20+double(i/8)*.1),"Pressure recovery produced incorrect pixels");
    }
    small.cancel();rejects([&]{small.output();});
    // Submit two layers behind a closed gate. A third must not reuse either
    // descriptor/host table; cancellation must retain both until retirement.
    LayerStack triple({
        LayerNode{Id::generate(),{},0,false,true,1,BlendMode::Normal,{},Sampling::Bilinear,a},
        LayerNode{Id::generate(),{},1,false,true,.5f,BlendMode::Multiply,{},Sampling::Bilinear,b},
        LayerNode{Id::generate(),{},2,false,true,.25f,BlendMode::Screen,{},Sampling::Bilinear,a}});
    {
        QueueGate gate(context);gate.block();
        block.start(triple,{20,20},{.1,.1},8,8);const auto serial=block.lastSubmitted();
        expect(block.advance()==GpuLayerBlock::State::Pending,"Blocked layered work completed");
        expect(block.lastSubmitted()==serial+2,"Two layer slots did not fill concurrently");
        for(int i=0;i<8;++i) block.advance();
        expect(block.lastSubmitted()==serial+2,"Busy descriptor slots were reused");
        block.cancel();expect(block.advance()==GpuLayerBlock::State::Pending,"Cancellation retired blocked GPU work");
        gate.open();expect(finish(block)==GpuLayerBlock::State::Cancelled,"Two-slot cancellation failed");
    }
    block.start(triple,{20,20},{.1,.1},8,8);
    expect(finish(block)==GpuLayerBlock::State::Complete,"Roomy pipelined reference failed");read(block,64);
    std::array<Pixel,64> roomy{};std::memcpy(roomy.data(),output.mapped,sizeof(roomy));
    double pressureError=0;
    {
        QueueGate gate(context);gate.block();
        small.start(triple,{20,20},{.1,.1},8,8);const auto serial=small.lastSubmitted();
        expect(small.advance()==GpuLayerBlock::State::Pending,"In-flight tile pressure requested a split");
        expect(small.lastSubmitted()==serial+1,"One-tile cache overwrote in-flight source");
        expect(small.advance()==GpuLayerBlock::State::Pending,"Temporary tile pressure became permanent");
        gate.open();expect(finish(small)==GpuLayerBlock::State::Complete,"Retired tile pressure failed to recover");
        read(small,64);
        for(std::size_t i=0;i<64;++i) {
            Pixel pixel;std::memcpy(&pixel,static_cast<const char*>(output.mapped)+i*sizeof(Pixel),sizeof(Pixel));
            expect(pixel==roomy[i],"Pipelined pressure recovery changed GPU pixels");
            const auto expected=triple.evaluate(20+double(i%8)*.1,20+double(i/8)*.1);
            const std::array<float,4> actual{pixel.r,pixel.g,pixel.b,pixel.a},reference{expected.r,expected.g,expected.b,expected.a};
            for(std::size_t channel=0;channel<4;++channel) {
                const auto error=std::abs(double(actual[channel])-reference[channel]);pressureError=std::max(pressureError,error);
                expect(std::isfinite(actual[channel]) && error<=std::max(.002,.002*std::abs(double(reference[channel]))),"Pipelined pressure CPU comparison failed");
            }
        }
    }
    std::cout<<"Two-slot blocked-queue cancellation and pressure passed; roomy/tiny output bit-identical; CPU max error="<<pressureError<<"\n";
    std::cout<<compared<<" layered GPU/CPU pixels across "<<blendModeCount<<" modes; max absolute error="<<maximum<<"; empty/cancel/pressure checks passed\n";
}
void runLayerFrames(Context& context) {
    if(!context.float64) return;
    TileStore store(16*1024*1024);const Extent extent{0,0,769,513};TileMap tiles;
    for(std::int64_t y=0;y<=2;++y) for(std::int64_t x=0;x<=3;++x) {
        const auto region=tileExtent(extent,{x,y});
        tiles[{x,y}]=store.constant(static_cast<int>(region.width),static_cast<int>(region.height),{float(x+1)/8,float(y+1)/8,.125f,.75f});
    }
    auto raster=std::make_shared<const RasterSnapshot>(Id::generate(),extent,PackedPixel{},tiles);
    auto makeDocument=[&](bool overlay) {
        std::vector<LayerNode> nodes{{Id::generate(),{},0,false,true,1,BlendMode::Normal,{},Sampling::Bilinear,raster}};
        if(overlay) nodes.push_back({Id::generate(),{},1,false,true,.5f,BlendMode::Multiply,{.8,.6,-.6,.8,17,-11},Sampling::Bilinear,raster});
        return std::make_shared<const DocumentSnapshot>(Id::generate(),769,513,96,std::move(nodes),overlay ? 2 : 1);
    };
    auto document=makeDocument(true);
    constexpr std::uint32_t width=273,height=67;constexpr auto count=width*height;
    const Coordinate origin{-1.125,-.875},step{2.75,8.25};
    Buffer output(context,count*sizeof(Pixel));
    Buffer sdr(context,DisplayKernel::paddedPixels(count)*4);DisplayKernel display(context.device);
    std::vector<Pixel> roomy;
    std::uint64_t compared=0;
    for(std::size_t slots:{16u,4u}) {
        GpuLayerFrame frame(context.physical,context.device,context.queue,context.family,slots*TileResidency::slotBytes,4*1024*1024);
        struct Retirement {VkQueue queue;~Retirement(){vkQueueWaitIdle(queue);}} retirement{context.queue};
        auto wait=[&](VkSemaphore timeline,std::uint64_t value) {
            VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};info.semaphoreCount=1;info.pSemaphores=&timeline;info.pValues=&value;
            check(vkWaitSemaphores(context.device,&info,30000000000ULL),"Wait frame test timeline");
        };
        auto finish=[&]() {
            for(int attempt=0;attempt<10000;++attempt) {
                const auto state=frame.advance();
                if(state!=GpuLayerFrame::State::Pending) return state;
                wait(frame.blockTimeline(),frame.lastBlockSubmitted());wait(frame.timeline(),frame.lastSubmitted());
            }
            throw std::runtime_error("Frame scheduler made no progress");
        };
        auto read=[&](std::size_t pixelCount) {
            check(vkResetCommandBuffer(context.command,0),"Reset frame readback");
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};check(vkBeginCommandBuffer(context.command,&begin),"Begin frame readback");
            const VkBufferCopy copy{0,0,pixelCount*sizeof(Pixel)};vkCmdCopyBuffer(context.command,frame.output(),output.buffer,1,&copy);
            if(pixelCount==count) {
                VkMemoryBarrier ready{VK_STRUCTURE_TYPE_MEMORY_BARRIER};ready.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
                ready.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
                vkCmdPipelineBarrier(context.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&ready,0,nullptr,0,nullptr);
                DisplayParameters parameters{};parameters.origin=origin;parameters.step=step;parameters.size={width,height};parameters.canvas={769,513};
                parameters.bgra=1;parameters.background={decodeSrgb(.85f),decodeSrgb(.7f)};
                display.record(context.command,0,{frame.output(),frame.outputBytes()},{sdr.buffer,sdr.bytes},parameters);
            }
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(context.command,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
            check(vkEndCommandBuffer(context.command),"End frame readback");check(vkResetFences(context.device,1,&context.fence),"Reset frame readback fence");
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&context.command;
            check(vkQueueSubmit(context.queue,1,&submit,context.fence),"Submit frame readback");check(vkWaitForFences(context.device,1,&context.fence,VK_TRUE,30000000000ULL),"Wait frame readback");output.invalidate();sdr.invalidate();
        };
        std::stop_source originalToken;
        frame.start(document,origin,step,width,height,originalToken.get_token());
        expect(frame.document()==document,"Frame lost captured document revision");
        rejects([&]{frame.output();});rejects([&]{frame.start(document,origin,step,width,height);});
        expect(finish()==GpuLayerFrame::State::Complete,"Frame failed under cache pressure");
        expect(slots==16 || frame.splitCount()>0,"Low cache budget did not exercise subdivision");
        expect(frame.blockCount()>=2,"Partial viewport edge block was omitted");
        const auto fullFrameBlocks=frame.blockCount();read(count);
        std::vector<Pixel> actual(count);std::memcpy(actual.data(),output.mapped,count*sizeof(Pixel));
        for(std::uint32_t y=0;y<height;++y) for(std::uint32_t x=0;x<width;++x) {
            const auto expected=document->stack.evaluate(origin.x+x*step.x,origin.y+y*step.y),value=actual[y*width+x];
            const std::array<float,4> a{value.r,value.g,value.b,value.a},b{expected.r,expected.g,expected.b,expected.a};
            for(std::size_t channel=0;channel<4;++channel) expect(std::isfinite(a[channel]) && std::abs(a[channel]-b[channel])<=.002f,"Viewport GPU/CPU mismatch");
            const Coordinate position{origin.x+x*step.x,origin.y+y*step.y};
            const auto code=position.x<0 || position.y<0 || position.x>=769 || position.y>=513 ? 0xff1f1f1fu :
                displayArgb(expected,decodeSrgb(((x/16+y/16)&1) ? .7f : .85f),static_cast<int>(std::floor(position.x)),static_cast<int>(std::floor(position.y)));
            std::uint32_t actualCode;std::memcpy(&actualCode,static_cast<const char*>(sdr.mapped)+(y*width+x)*4,4);
            for(int shift:{0,8,16}) expect(std::abs(int((actualCode>>shift)&255)-int((code>>shift)&255))<=1,"Layered frame-to-display pipeline mismatch");
            ++compared;
        }
        if(slots==16) roomy=actual;
        else expect(actual==roomy,"Subdivision changed viewport sampling or ownership");
        const auto completedCopy=frame.lastSubmitted(),completedBlock=frame.lastBlockSubmitted();
        const auto completedOutput=frame.output();
        originalToken.request_stop();
        frame.start(document,origin,step,width,height);
        expect(frame.state()==GpuLayerFrame::State::Complete && frame.output()==completedOutput,
               "Unchanged completed frame was not reused");
        expect(frame.lastSubmitted()==completedCopy && frame.lastBlockSubmitted()==completedBlock,
               "Unchanged completed frame submitted GPU work");
        read(count);
        expect(std::memcmp(actual.data(),output.mapped,count*sizeof(Pixel))==0,"Reused frame pixels changed");
        // A small new layer touches only the final viewport block. Preserve
        // the completed left block, including under the low upload budget.
        auto editedNodes=document->layers();
        auto patch=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,8,8},pack({1,0,0,1}));
        editedNodes.push_back({Id::generate(),{},2,false,true,1,BlendMode::Normal,{1,0,0,1,725,20},Sampling::Bilinear,patch});
        auto edited=std::make_shared<const DocumentSnapshot>(document->id,769,513,96,editedNodes,3);
        frame.start(edited,origin,step,width,height);
        expect(finish()==GpuLayerFrame::State::Complete,"Dirty frame failed");
        const auto dirtyBlocks=frame.blockCount();
        expect(dirtyBlocks>0 && dirtyBlocks<fullFrameBlocks && (slots!=16 || dirtyBlocks==1),"Unchanged viewport block was recomputed");
        read(count);
        for(std::uint32_t y=0;y<height;++y) for(std::uint32_t x=0;x<width;++x) {
            Pixel pixel;std::memcpy(&pixel,static_cast<const char*>(output.mapped)+(y*width+x)*sizeof(Pixel),sizeof(Pixel));
            const auto expected=edited->stack.evaluate(origin.x+x*step.x,origin.y+y*step.y);
            for(auto delta:{pixel.r-expected.r,pixel.g-expected.g,pixel.b-expected.b,pixel.a-expected.a})
                expect(std::abs(delta)<=.002f,"Dirty-region GPU output differs from full CPU render");
        }
        editedNodes.back().name="Rename only";
        auto renamed=std::make_shared<const DocumentSnapshot>(document->id,769,513,96,editedNodes,4);
        const auto beforeRename=frame.lastSubmitted();
        frame.start(renamed,origin,step,width,height);
        expect(finish()==GpuLayerFrame::State::Complete && frame.blockCount()==0 && frame.lastSubmitted()==beforeRename,
               "Metadata-only edit submitted GPU work");
        frame.start(document,origin,step,width,height);
        expect(finish()==GpuLayerFrame::State::Complete && frame.blockCount()==dirtyBlocks,"Removed layer did not invalidate only old coverage");
        read(count);
        expect(std::memcmp(actual.data(),output.mapped,count*sizeof(Pixel))==0,"Removing patch left stale pixels");
        // Every part of the cache key matters, independently. Return to the
        // original key between misses so no second field can hide an omission.
        auto miss=[&](DocumentPtr snapshot,Coordinate position,Coordinate spacing,std::uint32_t w,std::uint32_t h) {
            frame.start(snapshot,position,spacing,w,h);
            expect(frame.state()==GpuLayerFrame::State::Pending,"Changed frame key reused stale output");
            expect(finish()==GpuLayerFrame::State::Complete,"Changed frame did not finish");
            frame.start(document,origin,step,width,height);
            expect(finish()==GpuLayerFrame::State::Complete,"Original frame did not finish");
        };
        miss(makeDocument(true),origin,step,width,height);
        miss(document,{origin.x+1,origin.y},step,width,height);
        miss(document,{origin.x,origin.y+1},step,width,height);
        miss(document,origin,{step.x+.25,step.y},width,height);
        miss(document,origin,{step.x,step.y+.25},width,height);
        miss(document,origin,step,width-1,height);
        miss(document,origin,step,width,height-1);
        frame.cancel();rejects([&]{frame.output();});
        frame.start(document,origin,step,width,height);frame.cancel();
        expect(finish()==GpuLayerFrame::State::Cancelled,"Cancelled frame was published");
        frame.start(document,origin,step,width,height);frame.advance();frame.cancel();
        expect(finish()==GpuLayerFrame::State::Cancelled,"In-flight cancelled frame was published");
        std::stop_source pendingToken;
        frame.start(document,origin,step,width,height,pendingToken.get_token());
        pendingToken.request_stop();
        expect(finish()==GpuLayerFrame::State::Cancelled,"Stopped pending frame was published");
        frame.start(document,origin,step,width,height);
        expect(finish()==GpuLayerFrame::State::Complete,"Fresh token did not recover cancelled pending frame");
        auto replacement=makeDocument(false);frame.start(replacement,{10,10},step,1,1);
        expect(finish()==GpuLayerFrame::State::Complete && frame.document()==replacement,"Replacement frame revision was not retained");
        read(1);Pixel replacementPixel;std::memcpy(&replacementPixel,output.mapped,sizeof(Pixel));
        expect(replacementPixel==replacement->stack.evaluate(10,10),"Replacement frame contains stale pixels");
        frame.start(replacement,{11,10},step,1,1);
        const auto previousCopy=frame.lastSubmitted();
        for(int attempt=0;attempt<10 && frame.lastSubmitted()==previousCopy;++attempt) {
            frame.advance();wait(frame.blockTimeline(),frame.lastBlockSubmitted());
        }
        expect(frame.lastSubmitted()>previousCopy,"Cancellation fixture did not enqueue a frame copy");
        frame.cancel();expect(finish()==GpuLayerFrame::State::Cancelled,"Cancellation during frame copy published output");
        rejects([&]{frame.output();});
        rejects([&]{frame.start(document,origin,step,32768,32768);});
        std::cout<<"GPU frame slots="<<slots<<" passed full coverage, splitting, cancellation and replacement checks\n";
    }
    {
        GpuLayerFrame tiny(context.physical,context.device,context.queue,context.family,TileResidency::slotBytes,1024*1024);
        struct Retirement {VkQueue queue;~Retirement(){vkQueueWaitIdle(queue);}} retirement{context.queue};
        tiny.start(makeDocument(false),{256,256},{1,1},1,1);
        expect(tiny.advance()==GpuLayerFrame::State::NeedsCpu,"Unsplittable four-tap pixel did not request CPU fallback");
        rejects([&]{tiny.output();});
        auto highNodes=makeDocument(false)->layers();highNodes[0].sampling=Sampling::Lanczos;highNodes[0].localToDocument={.01,0,0,1,0,0};
        auto high=std::make_shared<const DocumentSnapshot>(Id::generate(),769,513,96,std::move(highNodes),3);
        tiny.start(high,{2,2},{1,1},1,1);
        expect(tiny.advance()==GpuLayerFrame::State::NeedsCpu,"Oversized anisotropic footprint did not request CPU");
        rejects([&]{tiny.output();});
    }
    std::cout<<compared<<" viewport GPU/CPU pixels; roomy and four-tile cache outputs agree exactly\n";
}
void runFilteredFrames(Context& context) {
    if(!context.float64) return;
    TileStore store(8*1024*1024);const Extent extent{-2,-3,2305,65};TileMap tiles;
    for(auto y=floorTile(extent.y);y<=floorTile(extent.y+extent.height-1);++y)
        for(auto x=floorTile(extent.x);x<=floorTile(extent.x+extent.width-1);++x) {
            if(x==3) continue;
            const auto area=tileExtent(extent,{x,y});
            tiles[{x,y}]=store.constant(static_cast<int>(area.width),static_cast<int>(area.height),{float(x+1)/16,.125f,-.0625f,.75f});
        }
    auto raster=std::make_shared<const RasterSnapshot>(Id::generate(),extent,pack({.25f,0,.5f,.5f}),tiles);
    auto document=[&](const std::shared_ptr<const RasterSnapshot>& source,BlendMode mode) {
        std::vector<LayerNode> layers{{Id::generate(),{},0,false,true,.875f,BlendMode::Normal,{},Sampling::Lanczos,source},
            {Id::generate(),{},1,false,true,.625f,mode,{.9,.08,.2,1,7,-2},Sampling::Lanczos,source}};
        return std::make_shared<const DocumentSnapshot>(Id::generate(),2305,65,96,std::move(layers),1);
    };
    const auto original=document(raster,BlendMode::Screen);constexpr std::uint32_t width=273,height=5;
    const Coordinate origin{40.125,10.75},step{7.5,7.5};
    Buffer readback(context,width*height*sizeof(Pixel));MipCache cpuCache(16*1024*1024);
    double maximum=0;std::size_t compared=0;std::vector<Pixel> roomy;
    for(std::size_t slots:{16u,4u}) {
        GpuLayerFrame frame(context.physical,context.device,context.queue,context.family,
            (slots==16 ? 32 : 4)*TileResidency::slotBytes,4*1024*1024,slots*GpuMipCache::slotBytes);
        struct Retirement {VkQueue queue;~Retirement(){vkQueueWaitIdle(queue);}} retirement{context.queue};
        auto wait=[&](VkSemaphore timeline,std::uint64_t value) {
            VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};info.semaphoreCount=1;info.pSemaphores=&timeline;info.pValues=&value;
            check(vkWaitSemaphores(context.device,&info,30000000000ULL),"Wait filtered frame");
        };
        auto finish=[&] {
            for(int i=0;i<100000;++i) {
                const auto state=frame.advance();if(state!=GpuLayerFrame::State::Pending) return state;
                wait(frame.blockTimeline(),frame.lastBlockSubmitted());wait(frame.timeline(),frame.lastSubmitted());
            }
            throw std::runtime_error("Filtered frame scheduler made no progress");
        };
        auto read=[&](std::uint32_t w,std::uint32_t h) {
            check(vkResetCommandBuffer(context.command,0),"Reset filtered readback");
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};check(vkBeginCommandBuffer(context.command,&begin),"Begin filtered readback");
            const VkBufferCopy copy{0,0,VkDeviceSize(w)*h*sizeof(Pixel)};vkCmdCopyBuffer(context.command,frame.output(),readback.buffer,1,&copy);
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(context.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
            check(vkEndCommandBuffer(context.command),"End filtered readback");check(vkResetFences(context.device,1,&context.fence),"Reset filtered fence");
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&context.command;
            check(vkQueueSubmit(context.queue,1,&submit,context.fence),"Submit filtered readback");
            check(vkWaitForFences(context.device,1,&context.fence,VK_TRUE,30000000000ULL),"Wait filtered readback");readback.invalidate();
            std::vector<Pixel> values(static_cast<std::size_t>(w)*h);std::memcpy(values.data(),readback.mapped,values.size()*sizeof(Pixel));return values;
        };
        auto compare=[&](DocumentPtr snapshot,Coordinate start,std::uint32_t w,std::uint32_t h) {
            frame.start(snapshot,start,step,w,h);expect(finish()==GpuLayerFrame::State::Complete,"High-quality frame fell back or failed");
            auto actual=read(w,h);CpuLayerSampler sampler(snapshot->stack,step,cpuCache);const auto all=sampler.all();
            for(std::uint32_t y=0;y<h;++y) for(std::uint32_t x=0;x<w;++x) {
                const auto value=actual[y*w+x],expected=*sampler.evaluate(all,start.x+x*step.x,start.y+y*step.y);
                const std::array<float,4> a{value.r,value.g,value.b,value.a},b{expected.r,expected.g,expected.b,expected.a};
                for(std::size_t c=0;c<4;++c) {
                    maximum=std::max(maximum,std::abs(double(a[c])-b[c]));
                    expect(std::isfinite(a[c]) && std::abs(a[c]-b[c])<=.003f,"Layered high-quality GPU/CPU mismatch");
                }
                ++compared;
            }
            return actual;
        };
        const auto pixels=compare(original,origin,width,height);
        if(slots==16) roomy=pixels;
        else {
            expect(frame.splitCount()>0,"Lowered high-quality budgets did not exercise subdivision");
            expect(pixels==roomy,"High-quality subdivision changed pixel coordinates or ownership");
        }
        const auto fullBlocks=frame.blockCount();
        auto editedTiles=tiles;
        const auto editExtent=tileExtent(extent,{0,0});
        editedTiles[{0,0}]=store.constant(static_cast<int>(editExtent.width),static_cast<int>(editExtent.height),{.125f,.5f,0,.75f});
        auto changedRaster=std::make_shared<const RasterSnapshot>(raster->id,extent,raster->defaultValue,editedTiles,raster->revision);
        compare(document(changedRaster,BlendMode::Screen),origin,width,height);
        expect(frame.blockCount()>0 && frame.blockCount()<fullBlocks,"Filtered pixel edit did not preserve unaffected blocks");
        expect(compare(original,origin,width,height)==pixels,"Filtered undo failed to restore exact pixels");
        for(int mode=0;mode<static_cast<int>(blendModeCount);++mode) compare(document(raster,static_cast<BlendMode>(mode)),{64.25,16.125},7,3);
        auto replacement=std::make_shared<const RasterSnapshot>(raster->id,extent,raster->defaultValue,tiles,raster->revision);
        frame.start(document(replacement,BlendMode::Normal),origin,step,width,height);
        const auto serial=frame.lastBlockSubmitted();
        for(int i=0;i<1000 && frame.lastBlockSubmitted()==serial;++i) frame.advance();
        expect(frame.lastBlockSubmitted()>serial,"Filtered cancellation fixture did not submit work");
        frame.cancel();expect(finish()==GpuLayerFrame::State::Cancelled,"Cancelled mip frame published pixels");rejects([&]{frame.output();});
        compare(original,{64.25,16.125},7,3);
    }
    {
        GpuLayerFrame tiny(context.physical,context.device,context.queue,context.family,4*TileResidency::slotBytes,1024*1024,GpuMipCache::slotBytes);
        struct Retirement {VkQueue queue;~Retirement(){vkQueueWaitIdle(queue);}} retirement{context.queue};
        tiny.start(original,origin,step,1,1);
        expect(tiny.advance()==GpuLayerFrame::State::NeedsCpu,"Unsatisfiable mip dependency did not request CPU fallback");
        rejects([&]{tiny.output();});
    }
    std::cout<<compared<<" layered high-quality GPU/CPU pixels across "<<blendModeCount<<" modes; max error="<<maximum<<"; budget splitting and cancellation passed\n";
}
void runDisplay(Context& context) {
    if(!context.float64) return;
    DisplayKernel kernel(context.device);std::uint64_t compared=0;int maximum=0;
    for(const auto size:std::array<std::array<std::uint32_t,2>,3>{{{1,1},{257,257},{1025,67}}}) {
        const auto count=size[0]*size[1];const auto padded=DisplayKernel::paddedPixels(count);
        Buffer source(context,padded*16),output(context,padded*4);
        struct Retirement {VkQueue queue;~Retirement(){vkQueueWaitIdle(queue);}} retirement{context.queue};
        std::vector<Pixel> pixels(count);
        const std::array<Pixel,7> special{{{0,0,0,0},{65504,-65504,2,1},{.000000059604645f,0,0,.000000059604645f},
            {1,0,0,1},{0,1,0,.5f},{0,0,1,.5f},{.0031308f,.5f,.99f,1}}};
        for(std::size_t i=0;i<pixels.size();++i) {
            const float alpha=float(i%257)/256;
            pixels[i]=i%3==0 ? special[i%special.size()] : unpack(pack({(float(i%4096)/2048-.25f)*alpha,.35f*alpha,.85f*alpha,alpha}));
        }
        std::memcpy(source.mapped,pixels.data(),count*sizeof(Pixel));source.flush();
        for(std::uint32_t packing:{0u,1u}) for(std::uint32_t cell:{1u,25u}) {
            DisplayParameters p{};p.origin={-.25,-.125};p.step={.50125,.50125};p.size=size;p.canvas={512,128};p.cell=cell;p.bgra=packing;
            if(count==1) p.origin={.5,.5};
            p.background={decodeSrgb(.85f),decodeSrgb(.7f)};
            std::memset(output.mapped,0xcd,static_cast<std::size_t>(output.bytes));output.flush();
            check(vkResetCommandBuffer(context.command,0),"Reset display command");
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};check(vkBeginCommandBuffer(context.command,&begin),"Begin display command");
            auto invalid=p;invalid.cell=0;rejects([&]{invalid.validate();});
            rejects([&]{kernel.record(context.command,2,{source.buffer,source.bytes},{output.buffer,output.bytes},p);});
            rejects([&]{kernel.record(context.command,0,{source.buffer,source.bytes-1},{output.buffer,output.bytes},p);});
            kernel.record(context.command,packing,{source.buffer,source.bytes},{output.buffer,output.bytes},p);
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(context.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
            check(vkEndCommandBuffer(context.command),"End display command");check(vkResetFences(context.device,1,&context.fence),"Reset display fence");
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&context.command;
            check(vkQueueSubmit(context.queue,1,&submit,context.fence),"Submit display");check(vkWaitForFences(context.device,1,&context.fence,VK_TRUE,30000000000ULL),"Wait display");output.invalidate();
            for(std::uint32_t y=0;y<size[1];++y) for(std::uint32_t x=0;x<size[0];++x) {
                const Coordinate position{p.origin.x+x*p.step.x,p.origin.y+y*p.step.y};
                const auto expected=position.x<0 || position.y<0 || position.x>=p.canvas[0] || position.y>=p.canvas[1] ? 0xff1f1f1fu :
                    displayArgb(pixels[y*size[0]+x],p.background[(x/cell+y/cell)&1],static_cast<int>(std::floor(position.x)),static_cast<int>(std::floor(position.y)));
                std::uint32_t actual;std::memcpy(&actual,static_cast<const char*>(output.mapped)+(y*size[0]+x)*4,4);
                if(!packing) actual=(actual&0xff00ff00u)|((actual&255u)<<16)|((actual>>16)&255u);
                expect((actual>>24)==255,"Display alpha is not opaque");
                for(int shift:{0,8,16}) {const int error=std::abs(int((actual>>shift)&255)-int((expected>>shift)&255));maximum=std::max(maximum,error);expect(error<=1,"GPU SDR display mismatch");}
                ++compared;
            }
            for(std::size_t i=count*4;i<output.bytes;++i) expect(static_cast<const std::uint8_t*>(output.mapped)[i]==0xcd,"Display dispatch overwrote padding");
        }
    }
    std::cout<<compared<<" GPU/CPU SDR display pixels; max code error="<<maximum<<"; RGBA/BGRA and dynamic-range tails passed\n";
}
void run(Context& context) {
    std::vector<Pixel> sources,backdrops;
    const std::array<Pixel,9> colors{{{0,0,0,1},{1,1,1,1},{1,0,0,1},{0,1,0,1},{0,0,1,1},{.5f,.5f,.5f,1},{.2f,.6f,.8f,1},{.6f,.6f,.2f,1},{.4f,.1f,.4f,1}}};
    for(float sa:{0.f,0.00006103515625f,.25f,.5f,1.f}) for(float ba:{0.f,.25f,.5f,1.f}) for(auto s:colors) for(auto b:colors) {
        s.a=sa;b.a=ba;sources.push_back(unpack(pack(fromStraightSrgb(s))));backdrops.push_back(unpack(pack(fromStraightSrgb(b))));
    }
    for(auto s:std::array<Pixel,4>{{{2,-1,0,.5f},{65504,-65504,0,.5f},{.000000059604645f,0,0,.000000059604645f},{0,0,0,0}}})
        for(auto b:std::array<Pixel,3>{{{1,0,0,1},{0,2,-1,.5f},{0,0,0,0}}}) {sources.push_back(s);backdrops.push_back(b);}
    // Explicit encoded half-way and endpoint neighborhoods exercise Soft Light,
    // Vivid Light, Hard Mix and Divide branch choices. Preserve FP32 here:
    // quantizing these boundaries to half would erase nextafter distinctions.
    const std::array<float,9> codes{0.f,std::nextafter(0.f,1.f),.25f,
        std::nextafter(.5f,0.f),.5f,std::nextafter(.5f,1.f),.75f,
        std::nextafter(1.f,0.f),1.f};
    for(float s:codes) for(float b:codes) for(float sa:{.25f,1.f}) for(float ba:{.5f,1.f}) {
        sources.push_back(fromStraightSrgb({s,s,s,sa}));
        backdrops.push_back(fromStraightSrgb({b,b,b,ba}));
    }
    // Frozen nearest-code boundaries generated from the same threshold recipe.
    // Backdrop's centered code is 4095-q; source advances from q to q+1 at
    // the threshold, so the analytical integer sum changes 4095 -> 4096.
    const std::array<std::array<std::uint32_t,2>,8> hardMixBoundaries{{
        {0x371e8391u,0x3f7fdb9cu},
        {0x37edc55au,0x3f7fb73au},
        {0x3ba88520u,0x3f5d1969u},
        {0x3c6ab9d2u,0x3f3d24edu},
        {0x3d503037u,0x3f05c3f9u},
        {0x3e5b0ffdu,0x3e5b2d9au},
        {0x3f05b783u,0x3d506372u},
        {0x3f7fc96bu,0x379e8391u}}};
    const auto hardMixBoundaryStart=sources.size();
    for(const auto& bits:hardMixBoundaries) {
        const auto boundary=std::bit_cast<float>(bits[0])*.5f;
        const auto backdrop=std::bit_cast<float>(bits[1])*.5f;
        for(float channel:{std::nextafter(boundary,0.f),boundary,std::nextafter(boundary,1.f)}) {
            sources.push_back({channel,channel,channel,.5f});
            backdrops.push_back({backdrop,backdrop,backdrop,.5f});
        }
    }
    // Deterministic, canonical half inputs across a full 256² tile, including
    // extended RGB and soft alpha. No platform RNG or image-codec dependence.
    std::uint32_t random=0x914b76a1;
    const auto next=[&]() {random=random*1664525u+1013904223u;return float(random>>8)/16777215.f;};
    while(sources.size()<256*256) {
        const auto pixel=[&]() {const float a=next();return unpack(pack({(next()*3-.5f)*a,(next()*3-.5f)*a,(next()*3-.5f)*a,a}));};
        sources.push_back(pixel());backdrops.push_back(pixel());
    }
    const VkDeviceSize bytes=(sources.size()+1)*sizeof(Pixel);
    Buffer source(context,bytes),backdrop(context,bytes),output(context,bytes); BlendKernel kernel(context.physical,context.device);
    // This synchronous test owns the queue. On a failed wait/compare, retire
    // any submitted work before stack unwinding destroys its bound resources.
    struct Retirement {VkQueue queue;~Retirement(){vkQueueWaitIdle(queue);}} retirement{context.queue};
    std::memcpy(source.mapped,sources.data(),sources.size()*sizeof(Pixel));std::memcpy(backdrop.mapped,backdrops.data(),backdrops.size()*sizeof(Pixel));source.flush();backdrop.flush();
    double maxAbsolute=0,maxRelative=0;std::uint64_t compared=0;
    for(std::uint32_t count:{1u,63u,64u,65u,65536u}) for(int mode=0;mode<static_cast<int>(blendModeCount);++mode) {
        const auto blend=static_cast<BlendMode>(mode);
        std::memset(output.mapped,0xcd,static_cast<std::size_t>(bytes));output.flush();
        check(vkResetCommandBuffer(context.command,0),"Reset command");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(context.command,&begin),"Begin command");
        // Submission makes flushed host input writes available. Explicitly
        // make compute writes visible to host before fence completion/readback.
        kernel.record(context.command,static_cast<std::size_t>(mode%2),source.binding(),backdrop.binding(),output.binding(),count,blend);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(context.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        check(vkEndCommandBuffer(context.command),"End command");check(vkResetFences(context.device,1,&context.fence),"Reset fence");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&context.command;
        check(vkQueueSubmit(context.queue,1,&submit,context.fence),"Submit compute");
        check(vkWaitForFences(context.device,1,&context.fence,VK_TRUE,30000000000ULL),"Wait compute");output.invalidate();
        for(std::size_t i=0;i<count;++i) {
            Pixel actual;std::memcpy(&actual,static_cast<const char*>(output.mapped)+i*sizeof(Pixel),sizeof(Pixel));
            Pixel expected;
            try {expected=composite(sources[i],backdrops[i],blend);} catch(const std::exception& error) {
                throw std::runtime_error("CPU oracle failed mode="+std::string(blendIdentifier(blend))+" pixel="+std::to_string(i)+" source="+std::to_string(sources[i].r)+","+std::to_string(sources[i].g)+","+std::to_string(sources[i].b)+","+std::to_string(sources[i].a)+" backdrop="+std::to_string(backdrops[i].r)+","+std::to_string(backdrops[i].g)+","+std::to_string(backdrops[i].b)+","+std::to_string(backdrops[i].a)+": "+error.what());
            }
            if(blend==BlendMode::HardMix && i>=hardMixBoundaryStart &&
               i<hardMixBoundaryStart+hardMixBoundaries.size()*3) {
                const bool white=(i-hardMixBoundaryStart)%3!=0;
                const float channel=sources[i].r*.5f+(white ? .25f : 0.f)+backdrops[i].r*.5f;
                expect(expected==Pixel{channel,channel,channel,.75f},"CPU Hard Mix violated analytical frozen boundary classification");
                expect(actual==Pixel{channel,channel,channel,.75f},"GPU Hard Mix violated analytical frozen boundary classification");
            }
            const std::array<float,4> a{actual.r,actual.g,actual.b,actual.a},b{expected.r,expected.g,expected.b,expected.a};
            for(std::size_t c=0;c<4;++c) {
                const double error=std::abs(double(a[c])-b[c]);const double tolerance=c==3 ? .002 : std::max(.002,.002*std::abs(double(b[c])));
                if(!std::isfinite(a[c]) || error>tolerance) throw std::runtime_error("GPU mismatch mode="+std::string(blendIdentifier(blend))+" pixel="+std::to_string(i)+" channel="+std::to_string(c)+" expected="+std::to_string(b[c])+" actual="+std::to_string(a[c]));
                maxAbsolute=std::max(maxAbsolute,error);maxRelative=std::max(maxRelative,error/std::max(1.,std::abs(double(b[c]))));
                if(expected.a==0) expect(std::bit_cast<std::uint32_t>(a[c])==0,"Transparent GPU output must be canonical positive zero");
                if(c<3) {
                    const auto display=[](float channel,float alpha) {return std::lround(255*std::clamp(encodeSrgb(channel+.5f*(1-alpha)),0.f,1.f));};
                    expect(std::abs(display(a[c],actual.a)-display(b[c],expected.a))<=1,"Undithered SDR blend differs by more than one code step");
                }
            }
            ++compared;
        }
        const auto* guard=static_cast<const unsigned char*>(output.mapped)+count*sizeof(Pixel);
        for(std::size_t i=0;i<sizeof(Pixel);++i) expect(guard[i]==0xcd,"Dispatch wrote beyond pixel count");
    }
    rejects([&]{kernel.record(context.command,2,source.binding(),backdrop.binding(),output.binding(),1,BlendMode::Normal);});
    rejects([&]{kernel.record(context.command,0,source.binding(),backdrop.binding(),output.binding(),65537,BlendMode::Normal);});
    rejects([&]{kernel.record(context.command,0,source.binding(),backdrop.binding(),source.binding(),1,BlendMode::Normal);});
    rejects([&]{kernel.record(context.command,0,{source.buffer,8},backdrop.binding(),output.binding(),1,BlendMode::Normal);});
    rejects([&]{kernel.record(context.command,0,source.binding(),backdrop.binding(),output.binding(),1,static_cast<BlendMode>(blendModeCount));});
    // Two dependent passes stay on the device: second reads the first output
    // and overwrites a retired input buffer. Exercise RAW and WAR dependencies,
    // plus two distinct descriptor slots in one command buffer.
    constexpr std::size_t first=2048,chainCount=257;
    std::memcpy(source.mapped,sources.data()+first,chainCount*sizeof(Pixel));
    std::memcpy(backdrop.mapped,backdrops.data()+first,chainCount*sizeof(Pixel));source.flush();backdrop.flush();
    check(vkResetCommandBuffer(context.command,0),"Reset chain command");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(context.command,&begin),"Begin chain command");
    kernel.record(context.command,0,source.binding(),backdrop.binding(),output.binding(),chainCount,BlendMode::Screen);
    VkMemoryBarrier dependency{VK_STRUCTURE_TYPE_MEMORY_BARRIER};dependency.srcAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
    dependency.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(context.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&dependency,0,nullptr,0,nullptr);
    kernel.record(context.command,1,source.binding(),output.binding(),backdrop.binding(),chainCount,BlendMode::ColorBurn);
    dependency.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;dependency.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(context.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&dependency,0,nullptr,0,nullptr);
    check(vkEndCommandBuffer(context.command),"End chain command");check(vkResetFences(context.device,1,&context.fence),"Reset chain fence");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&context.command;
    check(vkQueueSubmit(context.queue,1,&submit,context.fence),"Submit chain");
    check(vkWaitForFences(context.device,1,&context.fence,VK_TRUE,30000000000ULL),"Wait chain");backdrop.invalidate();
    for(std::size_t i=0;i<chainCount;++i) {
        const auto intermediate=composite(sources[first+i],backdrops[first+i],BlendMode::Screen);
        const auto expected=composite(sources[first+i],intermediate,BlendMode::ColorBurn);
        Pixel actual;std::memcpy(&actual,static_cast<const char*>(backdrop.mapped)+i*sizeof(Pixel),sizeof(Pixel));
        const std::array<float,4> a{actual.r,actual.g,actual.b,actual.a},b{expected.r,expected.g,expected.b,expected.a};
        for(std::size_t c=0;c<4;++c) expect(std::isfinite(a[c]) && std::abs(a[c]-b[c])<=.002f,"GPU-to-GPU blend chain mismatch");
    }
    std::cout<<chainCount<<" dependent GPU blend pairs passed without intermediate readback\n";
    std::cout<<compared<<" GPU/CPU blend pairs; max absolute error="<<maxAbsolute<<"; max error/max(1,abs(reference))="<<maxRelative<<'\n';
}
}
int main(int argc,char** argv) {
    try {
        bool requireDevice=false,requireValidation=false;std::string preferred;
        auto storageDirectory=std::filesystem::current_path();
        for(int i=1;i<argc;++i) {
            const std::string argument=argv[i];
            if(argument=="--require-device") requireDevice=true;
            else if(argument=="--require-validation") requireValidation=true;
            else if(argument=="--device" && i+1<argc) preferred=argv[++i];
            else if(argument=="--storage-dir" && i+1<argc) storageDirectory=std::filesystem::absolute(argv[++i]);
            else throw std::invalid_argument("Unknown GPU test option");
        }
        Context context;
        if(!context.initialize(requireValidation,preferred)) {expect(!requireDevice,"Required Vulkan compute device unavailable");std::cout<<"SKIP: Vulkan compute device unavailable\n";return 77;}
        runMemoryAdmission(context);runSpilledUploads(context,storageDirectory);runUploads(context);runCache(context);runSampling(context);runMipSampling(context);runMipCache(context);runLayerBlocks(context);runLayerFrames(context);runFilteredFrames(context);runDisplay(context);run(context);context.releaseDevice();expect(context.errors==0,"Vulkan validation reported errors");
        std::cout<<"All "<<blendModeCount<<" GPU blend modes and dispatch boundaries passed\n";
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
