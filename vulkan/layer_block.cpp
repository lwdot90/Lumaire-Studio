#include "vulkan/layer_block.h"
#include "io/spill_store.h"
#include <cstring>
#include <stdexcept>
#include <utility>

namespace compositor {
namespace {
constexpr VkDeviceSize blockBytes=256*256*sizeof(engine::Pixel);
constexpr VkBufferUsageFlags usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
}
GpuLayerBlock::GpuLayerBlock(VkPhysicalDevice physical,VkDevice device,VkQueue queue,std::uint32_t family,std::size_t tileBudget,VkDeviceSize mipBudget,
                           std::shared_ptr<MemoryAdmission> memory):
    physical_(physical),device_(device),mipBudget_(mipBudget),memory_(std::move(memory)),cache_(physical,device,queue,family,tileBudget,memory_),sampler_(device),blender_(physical,device,memory_),
    sample_(physical,device,blockBytes,usage,VulkanBuffer::Memory::Device,memory_),
    first_(physical,device,blockBytes,usage,VulkanBuffer::Memory::Device,memory_),
    second_(physical,device,blockBytes,usage,VulkanBuffer::Memory::Device,memory_) {
    for(std::size_t i=0;i<parameters_.size();++i) {
        parameters_[i]=std::make_unique<VulkanBuffer>(physical,device,sizeof(SampleParameters),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VulkanBuffer::Memory::Host,memory_);
        table_[i]=std::make_unique<VulkanBuffer>(physical,device,120*120*16,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VulkanBuffer::Memory::Host,memory_);
    }
}
bool GpuLayerBlock::retired() const {
    std::uint64_t completed=0;
    if(vkGetSemaphoreCounterValue(device_,cache_.timeline(),&completed)!=VK_SUCCESS) throw std::runtime_error("Poll layered render timeline failed");
    return completed>=cache_.lastSubmitted();
}
void GpuLayerBlock::start(const engine::LayerStack& stack,engine::Coordinate origin,engine::Coordinate step,std::uint32_t width,std::uint32_t height,std::uint32_t offsetX,std::uint32_t offsetY,std::stop_token stop) {
    if(state_==State::Pending || !retired()) throw std::logic_error("Previous layered block is still active");
    if(width==0 || height==0 || width>256 || height>256 || !std::isfinite(step.x) || !std::isfinite(step.y) || step.x<=0 || step.y<=0)
        throw std::invalid_argument("Invalid layered block dimensions or step");
    if(offsetX>32768-width || offsetY>32768-height) throw std::invalid_argument("Invalid block offset");
    engine::SampleRegion bounds{origin.x+offsetX*step.x,origin.y+offsetY*step.y,origin.x+(offsetX+width-1)*step.x,origin.y+(offsetY+height-1)*step.y};bounds.validate();
    std::vector<engine::LayerStack::Prepared> layers;
    std::vector<engine::ReductionPlan> reductions;
    for(const auto& layer:stack.prepared()) {
        engine::ReductionPlan reduction;engine::Coordinate support;
        if(layer.sampling==engine::Sampling::Lanczos) {
            reduction=engine::reductionPlan(layer.inverse,step,static_cast<int>(layer.raster->extent.width),static_cast<int>(layer.raster->extent.height));
            support=engine::reductionSupport(reduction);
        } else support=layer.sampling==engine::Sampling::Bilinear ? engine::Coordinate{.5,.5} : engine::Coordinate{};
        if(engine::intersectsSource(layer.raster->extent,layer.inverse,bounds,support)) {layers.push_back(layer);reductions.push_back(reduction);}
    }
    reductions_=std::move(reductions);mipLayer_.reset();
    layers_=std::move(layers);bounds_=bounds;origin_=origin;step_=step;width_=width;height_=height;
    offset_={offsetX,offsetY};
    next_=0;uploads_=0;initialized_=false;current_=false;stop_=stop;cancelled_=stop.stop_requested();state_=State::Pending;
}
VkBuffer GpuLayerBlock::output() const {
    if(state_!=State::Complete) throw std::logic_error("Layered block has no complete output");
    return current_ ? second_.handle() : first_.handle();
}
void GpuLayerBlock::cancel() {
    cancelled_=true;
    if(mips_) mips_->cancel();
    if(state_!=State::Pending) {state_=State::Cancelled;layers_.clear();}
}
GpuLayerBlock::State GpuLayerBlock::advance() {
    // Bounded recording quantum; do not spin while the GPU owns both slots.
    for(unsigned i=0;i<2 && state_==State::Pending;++i) {
        const auto before=cache_.lastSubmitted();
        advanceOne();
        if(cache_.lastSubmitted()==before) break;
    }
    return state_;
}
GpuLayerBlock::State GpuLayerBlock::advanceOne() {
    if(state_!=State::Pending) return state_;
    if(stop_.stop_requested()) cancel();
    if(cancelled_) {
        if(!retired()) return state_;
        if(mips_ && mips_->advance()==GpuMipCache::State::Pending) return state_;
        state_=State::Cancelled;layers_.clear();return state_;
    }
    if(initialized_ && next_==layers_.size()) {
        if(retired()) state_=State::Complete;
        return state_;
    }
    const auto* layer=next_<layers_.size() ? &layers_[next_] : nullptr;
    // Mip planning may replace derived storage and descriptor resources. Keep
    // it serialized with prior consumers; direct sampling can overlap safely.
    if(layer && layer->sampling==engine::Sampling::Lanczos && !retired()) return state_;
    std::uint64_t completed=0;
    if(vkGetSemaphoreCounterValue(device_,cache_.timeline(),&completed)!=VK_SUCCESS)
        throw std::runtime_error("Poll layered descriptor retirement failed");
    std::size_t slot=0;
    while(slot<retirement_.size() && retirement_[slot]>completed) ++slot;
    if(slot==retirement_.size()) return state_;
    auto& table=*table_[slot];auto& parameters=*parameters_[slot];
    SampleParameters p{};engine::TileMap required;
    SampleKernel::Buffer sourcePool{cache_.buffer(),cache_.bytes()};
    std::vector<std::array<std::int32_t,4>> entries;
    if(layer) {
        const auto& raster=*layer->raster;auto extent=raster.extent;
        if(extent.x < -1073741824 || extent.y < -1073741824 || extent.x>1073741824 || extent.y>1073741824) {
            if(!retired()) return state_;
            throw std::out_of_range("Layer source geometry requires CPU fallback");
        }
        p.inverse=layer->inverse;p.origin=origin_;p.step=step_;p.outputOffset=offset_;
        const auto& reduction=reductions_[next_];
        if(layer->sampling==engine::Sampling::Lanczos) {
            p.footprint=reduction.residualFootprint;
            if(p.footprint.x>16 || p.footprint.y>16) throw std::out_of_range("Direct GPU filter footprint requires CPU fallback");
            p.inputOrigin={static_cast<double>(extent.x),static_cast<double>(extent.y)};
            if(reduction.level) {
                p.inputScale=std::ldexp(1.,static_cast<int>(reduction.level));p.fp32Source=1;extent.x=extent.y=0;
                for(unsigned i=0;i<reduction.level;++i) {extent.width=(extent.width+1)/2;extent.height=(extent.height+1)/2;}
            } else p.tapOrigin=p.inputOrigin;
        }
        p.extent={static_cast<std::int32_t>(extent.x),static_cast<std::int32_t>(extent.y),static_cast<std::int32_t>(extent.width),static_cast<std::int32_t>(extent.height)};
        const auto gx=engine::floorTile(extent.x),gy=engine::floorTile(extent.y);
        p.grid={static_cast<std::int32_t>(gx),static_cast<std::int32_t>(gy),static_cast<std::int32_t>(engine::floorTile(extent.x+extent.width-1)-gx+1),static_cast<std::int32_t>(engine::floorTile(extent.y+extent.height-1)-gy+1)};
        p.defaultPixel=p.fp32Source ? engine::Pixel{} : engine::unpack(raster.defaultValue);
        p.width=width_;p.height=height_;p.sampling=static_cast<std::uint32_t>(layer->sampling);p.opacity=layer->opacity;p.validate();
        entries.resize(static_cast<std::size_t>(p.grid[2]*p.grid[3]),{p.fp32Source ? -2 : -1,0,0,0});
        if(p.fp32Source) {
            if(!mips_) mips_=std::make_unique<GpuMipCache>(physical_,device_,cache_,mipBudget_,memory_);
            std::vector<engine::TileCoord> pieces;
            for(auto y=gy;y<gy+p.grid[3];++y) for(auto x=gx;x<gx+p.grid[2];++x) {
                const engine::TileCoord coordinate{x,y};const auto area=engine::tileExtent(extent,coordinate);
                if(engine::intersectsSource(area,layer->inverse,bounds_,{3*p.footprint.x,3*p.footprint.y},p.inputOrigin,p.inputScale)) pieces.push_back(coordinate);
            }
            if(mipLayer_!=next_) {mips_->start(layer->raster,reduction.level,pieces,stop_);mipLayer_=next_;}
            const auto status=mips_->advance();
            if(status==GpuMipCache::State::Pending) return state_;
            if(status==GpuMipCache::State::Cancelled) {cancel();return state_;}
            if(status==GpuMipCache::State::NeedsSpace) return state_=State::NeedsSplit;
            if(status!=GpuMipCache::State::Complete) throw std::logic_error("Unexpected mip generation state");
            sourcePool={mips_->buffer(),mips_->bytes()};
            for(auto coordinate:pieces) entries[static_cast<std::size_t>((coordinate.y-gy)*p.grid[2]+coordinate.x-gx)]=
                {static_cast<std::int32_t>(mips_->lease(coordinate)),static_cast<std::int32_t>(coordinate.x*256),static_cast<std::int32_t>(coordinate.y*256),0};
        } else {
            for(const auto& [coordinate,tile]:raster.tiles) {
                entries[static_cast<std::size_t>((coordinate.y-gy)*p.grid[2]+coordinate.x-gx)][0]=-2;
                if(layer->sampling==engine::Sampling::Lanczos && engine::intersectsSource(engine::tileExtent(extent,coordinate),layer->inverse,bounds_,{3*p.footprint.x,3*p.footprint.y}))
                    required.emplace(coordinate,tile);
            }
            if(layer->sampling!=engine::Sampling::Lanczos) required=engine::requiredSourceTiles(raster,layer->inverse,layer->sampling,bounds_);
        }
    }
    if(!cache_.begin()) return state_;
    bool recording=true;
    try {
        if(layer) {
            for(const auto& [coordinate,tile]:required) {
                auto lease=cache_.acquire(tile,stop_);
                if(!lease) {
                    cache_.cancel();recording=false;
                    // Temporary pressure from an earlier layer is not proof
                    // that this block exceeds the budget. Retry after it retires.
                    if(completed>=cache_.lastSubmitted()) state_=State::NeedsSplit;
                    return state_;
                }
                const auto extent=engine::tileExtent(layer->raster->extent,coordinate);
                entries[static_cast<std::size_t>((coordinate.y-p.grid[1])*p.grid[2]+coordinate.x-p.grid[0])]={static_cast<std::int32_t>(lease->slot),static_cast<std::int32_t>(extent.x),static_cast<std::int32_t>(extent.y),0};
            }
            std::memcpy(table.mapped(),entries.data(),entries.size()*16);table.flush();
            std::memcpy(parameters.mapped(),&p,sizeof(p));parameters.flush();
        }
        const auto command=cache_.consumers();
        VkMemoryBarrier dependency{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        // Include previous sampled/output accesses before reusing scratch and
        // ping-pong storage, even when the preceding submission has retired.
        dependency.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        dependency.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&dependency,0,nullptr,0,nullptr);
        if(!initialized_) {
            vkCmdFillBuffer(command,first_.handle(),0,blockBytes,0);
            dependency.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;dependency.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&dependency,0,nullptr,0,nullptr);
        }
        if(layer) {
            sampler_.record(command,slot,sourcePool,{table.handle(),entries.size()*16},
                {parameters.handle(),parameters.bytes()},{sample_.handle(),sample_.bytes()},p);
            dependency.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;dependency.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&dependency,0,nullptr,0,nullptr);
            auto& backdrop=current_ ? second_ : first_;auto& output=current_ ? first_ : second_;
            blender_.record(command,slot,{sample_.handle(),sample_.bytes()},{backdrop.handle(),backdrop.bytes()},
                {output.handle(),output.bytes()},width_*height_,layer->blend);
        }
        dependency.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;dependency.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&dependency,0,nullptr,0,nullptr);
        if(stop_.stop_requested()) throw io::SpillError(io::SpillErrorCode::Cancelled,"GPU recording cancelled");
        const auto uploadCount=cache_.uploadCount();
        recording=false;serial_=cache_.submit();uploads_+=uploadCount;
        retirement_[slot]=serial_;
        if(layer) {++next_;current_=!current_;}
        initialized_=true;
    } catch(const io::SpillError& error) {
        if(recording) cache_.cancel();
        if(error.code()!=io::SpillErrorCode::Cancelled) throw;
        cancel();
    } catch(...) {if(recording) cache_.cancel();throw;}
    return state_;
}
}
