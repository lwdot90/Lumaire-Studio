#include "rendering/mip_cache.h"
#include "core/document.h"
#include <unordered_set>

namespace compositor {
std::size_t MipCache::Hash::operator()(const Key& key) const {
    auto hash=std::hash<const void*>{}(key.source);
    for(auto value:{std::size_t(key.level),std::size_t(key.x),std::size_t(key.y)})
        hash^=value+std::size_t(0x9e3779b9)+(hash<<6)+(hash>>2);
    return hash;
}
MipCache::MipCache(std::size_t payloadBudget,std::shared_ptr<MemoryAdmission> memory)
    :capacity_(payloadBudget/slotBytes),memory_(std::move(memory)) {
    // Metadata has a separate, explicit entry-count bound; no unbounded table
    // for missing/transparent source pieces. One region's filtering scratch per
    // recursion level is transient, bounded by 15 levels and side=32.
    if(capacity_==0 || capacity_>16384) throw std::invalid_argument("Invalid mip cache budget");
}
void MipCache::clear() {index_.clear();entries_.clear();baseReader_.reset();baseSource_.reset();}
void MipCache::prune(const engine::DocumentSnapshot& document) {
    std::unordered_set<const engine::RasterSnapshot*> sources;
    for(const auto& node:document.layers()) if(node.raster) sources.insert(node.raster.get());
    if(baseSource_ && !sources.contains(baseSource_.get())) {baseReader_.reset();baseSource_.reset();}
    for(auto it=entries_.begin();it!=entries_.end();) {
        if(sources.contains(it->source.get())) ++it;
        else {index_.erase(it->key);it=entries_.erase(it);}
    }
}
std::optional<engine::Pixel> MipCache::pixel(const std::shared_ptr<const engine::RasterSnapshot>& source,
    unsigned level,std::int64_t x,std::int64_t y,const std::function<bool()>& cancelled,std::stop_token token) {
    if(!source) throw std::invalid_argument("Missing mip source");
    int width=static_cast<int>(source->extent.width),height=static_cast<int>(source->extent.height);
    unsigned available=0;
    for(int dimension=std::max(width,height);dimension>1;dimension=(dimension+1)/2) ++available;
    if(level>available) throw std::invalid_argument("Mip level beyond 1x1");
    if(token.stop_requested() || (cancelled && cancelled())) {releaseSourceLease();return std::nullopt;}
    int parentWidth=width,parentHeight=height;
    for(unsigned i=0;i<level;++i) {parentWidth=width;parentHeight=height;width=(width+1)/2;height=(height+1)/2;}
    if(x<0 || y<0 || x>=width || y>=height) return engine::Pixel{};
    // Local mip coordinates are relative to the immutable raster's origin.
    // Addition is safe: validated raster endpoints bound all in-range inputs.
    if(level==0) {
        if(baseSource_.get()!=source.get()) {
            baseReader_.reset();baseSource_=source;
            baseReader_.emplace(source);
        }
        try {return baseReader_->pixel(source->extent.x+x,source->extent.y+y,cancelled,token);}
        catch(const RasterReadCancelled&) {return std::nullopt;}
    }
    const Key key{source.get(),level,x/side,y/side};
    if(const auto found=index_.find(key);found!=index_.end()) {
        entries_.splice(entries_.begin(),entries_,found->second);
        const auto& region=found->second->region;
        return region.pixels[static_cast<std::size_t>((y-region.extent.y)*region.extent.width+x-region.extent.x)];
    }
    std::optional<MemoryAdmission::Reservation> memory;
    if(memory_) {
        memory=memory_->reserve(slotBytes+sizeof(Entry)+64);
        if(!memory) {clear();memory=memory_->require(slotBytes+sizeof(Entry)+64);}
    }
    bool aborted=false;
    const auto stop=[&]{return aborted || token.stop_requested() || (cancelled && cancelled());};
    const engine::PixelReader parent=[&](auto px,auto py) {
        const auto value=pixel(source,level-1,px,py,cancelled,token);
        if(!value) {aborted=true;return engine::Pixel{};}
        return *value;
    };
    const engine::Extent area{key.x*side,key.y*side,std::min(side,std::int64_t(width)-key.x*side),std::min(side,std::int64_t(height)-key.y*side)};
    auto region=engine::halveRegion(parent,parentWidth,parentHeight,area,stop);
    releaseSourceLease();
    if(!region || stop()) return std::nullopt;
    const auto result=region->pixels[static_cast<std::size_t>((y-area.y)*area.width+x-area.x)];
    // Copy the requested pixel before publication: callers never retain a view
    // into entries that recursion or a subsequent request could evict.
    while(entries_.size()>=capacity_) {index_.erase(entries_.back().key);entries_.pop_back();}
    if(memory) memory->commit();
    entries_.push_front({key,source,std::move(memory),std::move(*region)});
    try {index_.emplace(key,entries_.begin());}
    catch(...) {entries_.pop_front();throw;}
    ++built_;
    return result;
}
}
