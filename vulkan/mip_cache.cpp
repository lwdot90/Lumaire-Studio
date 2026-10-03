#include "vulkan/mip_cache.h"
#include "io/spill_store.h"
#include <cstring>
#include <limits>

namespace compositor {
using namespace engine;
namespace {
constexpr auto storage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
std::vector<TileCoord> parentPieces(Extent area,Extent parent) {
    // ceil(center-6-.5) .. floor(center+6-.5), for center=origin+2*i+1.
    const auto left=std::max(parent.x,parent.x+2*area.x-5),top=std::max(parent.y,parent.y+2*area.y-5);
    const auto right=std::min(parent.x+parent.width-1,parent.x+2*(area.x+area.width-1)+6);
    const auto bottom=std::min(parent.y+parent.height-1,parent.y+2*(area.y+area.height-1)+6);
    std::vector<TileCoord> result;
    for(auto y=floorTile(top);y<=floorTile(bottom);++y) for(auto x=floorTile(left);x<=floorTile(right);++x) result.push_back({x,y});
    return result;
}
}
VkDeviceSize GpuMipCache::checkedBudget(VkPhysicalDevice physical,VkDeviceSize budget) {
    if(!physical) throw std::invalid_argument("Missing mip device");
    VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(physical,&properties);
    budget=budget/slotBytes*slotBytes;
    if(!budget || budget>properties.limits.maxStorageBufferRange) throw std::invalid_argument("Mip budget outside storage-buffer limits");
    return budget;
}
GpuMipCache::GpuMipCache(VkPhysicalDevice physical,VkDevice device,VulkanTileCache& uploads,VkDeviceSize budget,
                       std::shared_ptr<MemoryAdmission> memory):
    device_(device),uploads_(uploads),sampler_(device),
    pool_(physical,device,checkedBudget(physical,budget),storage|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VulkanBuffer::Memory::Device,memory),
    scratch_(physical,device,64*64*sizeof(Pixel),storage|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VulkanBuffer::Memory::Device,memory),
    parameters_(physical,device,sizeof(SampleParameters),storage,VulkanBuffer::Memory::Host,memory),
    table_(physical,device,120*120*16,storage,VulkanBuffer::Memory::Host,memory),entries_(static_cast<std::size_t>(pool_.bytes()/slotBytes)) {
    if(pool_.allocationBytes()>budget) throw std::length_error("Mip allocation padding exceeds budget");
}
bool GpuMipCache::retired() const {
    std::uint64_t completed=0;
    if(vkGetSemaphoreCounterValue(device_,uploads_.timeline(),&completed)!=VK_SUCCESS) throw std::runtime_error("Poll mip timeline failed");
    return completed>=uploads_.lastSubmitted();
}
Extent GpuMipCache::extent(unsigned level) const {
    if(level==0) return source_->extent;
    auto width=source_->extent.width,height=source_->extent.height;
    for(unsigned i=0;i<level;++i) {width=(width+1)/2;height=(height+1)/2;}
    return {0,0,width,height};
}
bool GpuMipCache::pinned(const Key& key) const {
    if(std::find(requested_.begin(),requested_.end(),key)!=requested_.end()) return true;
    for(const auto& task:tasks_)
        if(task.key==key || std::find(task.parents.begin(),task.parents.end(),key)!=task.parents.end()) return true;
    return false;
}
std::optional<std::size_t> GpuMipCache::find(const Key& key) const {
    for(std::size_t i=0;i<entries_.size();++i) if(entries_[i].complete && entries_[i].key==key) return i;
    return {};
}
std::optional<std::size_t> GpuMipCache::allocate(const Key& key) {
    std::optional<std::size_t> candidate;
    for(std::size_t i=0;i<entries_.size();++i) {
        if(!entries_[i].key) {candidate=i;break;}
        if(!pinned(*entries_[i].key) && (!candidate || entries_[i].lastUse<entries_[*candidate].lastUse)) candidate=i;
    }
    if(candidate) entries_[*candidate]={key,source_,++clock_,false};
    return candidate;
}
void GpuMipCache::discardPartial() {
    for(auto& entry:entries_) if(!entry.complete) entry={};
    tasks_.clear();requested_.clear();source_.reset();
}
void GpuMipCache::start(std::shared_ptr<const RasterSnapshot> source,unsigned level,std::vector<TileCoord> pieces,std::stop_token stop) {
    if(failed_ || state_==State::Pending || !retired()) throw std::logic_error("Mip cache still active or failed");
    if(!source || level==0 || level>15) throw std::invalid_argument("Invalid mip request");
    unsigned available=0;for(auto size=std::max(source->extent.width,source->extent.height);size>1;size=(size+1)/2) ++available;
    if(level>available) throw std::invalid_argument("Mip request beyond 1x1");
    if(source->extent.x < -1073741824 || source->extent.x>1073741824 || source->extent.y < -1073741824 || source->extent.y>1073741824)
        throw std::out_of_range("Mip source requires CPU geometry fallback");
    std::sort(pieces.begin(),pieces.end());pieces.erase(std::unique(pieces.begin(),pieces.end()),pieces.end());
    auto width=source->extent.width,height=source->extent.height;
    for(unsigned i=0;i<level;++i) {width=(width+1)/2;height=(height+1)/2;}
    std::vector<Key> requested;
    for(auto piece:pieces) {tileExtent({0,0,width,height},piece);requested.push_back({source.get(),level,piece});}
    discardPartial();source_=std::move(source);requested_=std::move(requested);stop_=stop;cancelled_=stop.stop_requested();
    if(requested_.size()>entries_.size()) {state_=State::NeedsSpace;discardPartial();return;}
    state_=State::Pending;
}
void GpuMipCache::cancel() {
    cancelled_=true;
    if(state_!=State::Pending) {state_=State::Cancelled;discardPartial();}
}
std::size_t GpuMipCache::lease(TileCoord coordinate) const {
    if(state_!=State::Complete) throw std::logic_error("No complete mip request");
    for(const auto& key:requested_) if(key.coordinate==coordinate) {
        const auto slot=find(key);if(slot) return *slot;
    }
    throw std::out_of_range("Mip piece was not requested or resident");
}
std::size_t GpuMipCache::residentPieces() const {
    return static_cast<std::size_t>(std::count_if(entries_.begin(),entries_.end(),[](const Entry& entry){return entry.complete;}));
}
void GpuMipCache::record(Task& task,Extent area,Extent parent) {
    bool recording=true;
    try {
    SampleParameters p{};p.origin={static_cast<double>(parent.x)+1,static_cast<double>(parent.y)+1};p.step={2,2};
    p.extent={static_cast<std::int32_t>(parent.x),static_cast<std::int32_t>(parent.y),static_cast<std::int32_t>(parent.width),static_cast<std::int32_t>(parent.height)};
    const auto gx=floorTile(parent.x),gy=floorTile(parent.y);
    p.grid={static_cast<std::int32_t>(gx),static_cast<std::int32_t>(gy),static_cast<std::int32_t>(floorTile(parent.x+parent.width-1)-gx+1),static_cast<std::int32_t>(floorTile(parent.y+parent.height-1)-gy+1)};
    p.width=static_cast<std::uint32_t>(area.width);p.height=static_cast<std::uint32_t>(area.height);p.sampling=2;p.opacity=1;p.footprint={2,2};
    p.outputOffset={static_cast<std::uint32_t>(area.x),static_cast<std::uint32_t>(area.y)};p.fp32Source=task.key.level>1;
    p.defaultPixel=p.fp32Source ? Pixel{} : unpack(source_->defaultValue);p.validate();
    std::vector<std::array<std::int32_t,4>> table(static_cast<std::size_t>(p.grid[2]*p.grid[3]),{p.fp32Source ? -2 : -1,0,0,0});
    const auto index=[&](TileCoord coord){return static_cast<std::size_t>((coord.y-gy)*p.grid[2]+coord.x-gx);};
        if(p.fp32Source) {
            for(const auto& key:task.parents) {
                const auto slot=find(key);if(!slot) throw std::logic_error("Mip parent missing after preparation");
                entries_[*slot].lastUse=++clock_;
                table[index(key.coordinate)]={static_cast<std::int32_t>(*slot),static_cast<std::int32_t>(key.coordinate.x*256),static_cast<std::int32_t>(key.coordinate.y*256),0};
            }
        } else {
            for(const auto& [coord,tile]:source_->tiles) {(void)tile;table[index(coord)][0]=-2;}
            for(auto coord:parentPieces(area,parent)) if(const auto found=source_->tiles.find(coord);found!=source_->tiles.end()) {
                const auto lease=uploads_.acquire(found->second,stop_);
                if(!lease) {uploads_.cancel();recording=false;state_=State::NeedsSpace;return;}
                const auto clipped=tileExtent(parent,coord);
                table[index(coord)]={static_cast<std::int32_t>(lease->slot),static_cast<std::int32_t>(clipped.x),static_cast<std::int32_t>(clipped.y),0};
            }
        }
        std::memcpy(parameters_.mapped(),&p,sizeof(p));parameters_.flush();std::memcpy(table_.mapped(),table.data(),table.size()*16);table_.flush();
        const auto command=uploads_.consumers();VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        sampler_.record(command,0,{p.fp32Source ? pool_.handle() : uploads_.buffer(),p.fp32Source ? pool_.bytes() : uploads_.bytes()},
            {table_.handle(),table.size()*16},{parameters_.handle(),parameters_.bytes()},{scratch_.handle(),scratch_.bytes()},p);
        barrier.srcAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        std::array<VkBufferCopy,64> rows{};
        const auto localX=area.x-task.key.coordinate.x*256,localY=area.y-task.key.coordinate.y*256;
        for(std::int64_t y=0;y<area.height;++y) rows[static_cast<std::size_t>(y)]={VkDeviceSize(y*area.width)*sizeof(Pixel),
            task.slot*slotBytes+VkDeviceSize((localY+y)*256+localX)*sizeof(Pixel),VkDeviceSize(area.width)*sizeof(Pixel)};
        vkCmdCopyBuffer(command,scratch_.handle(),pool_.handle(),p.height,rows.data());
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        if(stop_.stop_requested()) throw io::SpillError(io::SpillErrorCode::Cancelled,"GPU recording cancelled");
        recording=false;uploads_.submit();++task.next;
    } catch(const io::SpillError& error) {
        if(recording) uploads_.cancel();
        if(error.code()!=io::SpillErrorCode::Cancelled) throw;
        cancelled_=true;state_=State::Cancelled;
    } catch(...) {if(recording) uploads_.cancel();failed_=true;throw;}
}
GpuMipCache::State GpuMipCache::advance() {
    if(failed_) throw std::logic_error("Mip queue failed");
    if(state_!=State::Pending || !retired()) return state_;
    if(cancelled_ || stop_.stop_requested()) {discardPartial();state_=State::Cancelled;return state_;}
    if(clock_>std::numeric_limits<std::uint64_t>::max()-entries_.size()-1024) throw std::overflow_error("Mip LRU clock exhausted");
    if(tasks_.empty()) {
        for(const auto& key:requested_) {
            if(const auto slot=find(key)) {entries_[*slot].lastUse=++clock_;continue;}
            const auto slot=allocate(key);
            if(!slot) {discardPartial();return state_=State::NeedsSpace;}
            tasks_.push_back({key,*slot});break;
        }
        if(tasks_.empty()) return state_=State::Complete;
    }
    auto& task=tasks_.back();const auto piece=tileExtent(extent(task.key.level),task.key.coordinate);
    Extent area;
    while(task.next<16) {
        const auto x=piece.x+std::int64_t(task.next%4)*64,y=piece.y+std::int64_t(task.next/4)*64;
        if(x>=piece.x+piece.width || y>=piece.y+piece.height) {++task.next;continue;}
        area={x,y,std::min(std::int64_t(64),piece.x+piece.width-x),std::min(std::int64_t(64),piece.y+piece.height-y)};break;
    }
    if(task.next==16) {
        entries_[task.slot].complete=true;entries_[task.slot].lastUse=++clock_;++generated_;tasks_.pop_back();return state_;
    }
    const auto parent=extent(task.key.level-1);task.parents.clear();
    if(task.key.level>1) {
        for(auto coord:parentPieces(area,parent)) task.parents.push_back({source_.get(),task.key.level-1,coord});
        for(const auto& key:task.parents) if(!find(key)) {
            const auto slot=allocate(key);
            if(!slot) {discardPartial();return state_=State::NeedsSpace;}
            const Key captured=key;tasks_.push_back({captured,*slot});return state_;
        }
    }
    if(!uploads_.begin()) return state_;
    record(task,area,parent);
    if(state_==State::NeedsSpace || state_==State::Cancelled) discardPartial();
    return state_;
}
}
