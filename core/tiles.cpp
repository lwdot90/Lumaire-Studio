#include "core/tiles.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace compositor::engine {
namespace {
std::size_t pixelCount(int width, int height) {
    if(width<1 || width>256 || height<1 || height>256) throw std::invalid_argument("Invalid tile dimensions");
    return static_cast<std::size_t>(width)*static_cast<std::size_t>(height);
}
}
void Extent::validate() const {
    if(width<1 || height<1 || width>30000 || height>30000 || width*height>100000000)
        throw std::invalid_argument("Raster dimensions exceed document limits");
    const auto maximum=std::numeric_limits<std::int64_t>::max();
    if(x>maximum-width || y>maximum-height) throw std::overflow_error("Raster endpoint overflow");
}
bool Extent::contains(std::int64_t px, std::int64_t py) const {
    validate();
    return px>=x && py>=y && px<x+width && py<y+height;
}
std::int64_t floorTile(std::int64_t coordinate) {
    return coordinate/tileSide-(coordinate%tileSide<0 ? 1 : 0);
}
Extent tileExtent(const Extent& asset, TileCoord coordinate) {
    asset.validate();
    if(coordinate.x<floorTile(asset.x) || coordinate.x>floorTile(asset.x+asset.width-1) ||
       coordinate.y<floorTile(asset.y) || coordinate.y>floorTile(asset.y+asset.height-1))
        throw std::out_of_range("Tile outside asset extent");
    const auto cellX=coordinate.x*tileSide, cellY=coordinate.y*tileSide;
    const auto x=std::max(asset.x,cellX), y=std::max(asset.y,cellY);
    return {x,y,std::min(asset.x+asset.width-x,tileSide-(x-cellX)),
                std::min(asset.y+asset.height-y,tileSide-(y-cellY))};
}
void TileBudget::reserve(std::size_t bytes) {
    auto used=used_.load();
    do {
        if(bytes>limit_ || used>limit_-bytes) throw std::length_error("Tile memory budget exhausted");
    } while(!used_.compare_exchange_weak(used,used+bytes));
}
Tile::Tile(std::shared_ptr<TileBudget> budget, std::size_t charge, std::size_t budgetCharge, std::int64_t version,
           int width, int height, bool uniform, PackedPixel constant, std::vector<PackedPixel> data,
           std::optional<MemoryAdmission::Reservation> memory, SpillTilePayload backing)
    :budget_(std::move(budget)),charge_(charge),budgetCharge_(budgetCharge),version_(version),width_(width),height_(height),uniform_(uniform),
     constant_(constant),linearConstant_(unpack(constant)),memory_(std::move(memory)),data_(std::move(data)),backing_(std::move(backing)) {}
Tile::~Tile() { budget_->release(budgetCharge_); }
Tile::ReadLease Tile::read(std::stop_token stop) const {
    if(stop.stop_requested()) throw io::SpillError(io::SpillErrorCode::Cancelled,"Tile read cancelled");
    ReadLease lease;
    lease.width_=width_; lease.height_=height_; lease.uniform_=uniform_; lease.constant_=constant_;
    if(backing_) lease.backing_=backing_.resident(stop);
    else if(!uniform_) lease.owner_=shared_from_this();
    return lease;
}
bool Tile::spill(std::stop_token stop) const {
    if(stop.stop_requested()) throw io::SpillError(io::SpillErrorCode::Cancelled,"Tile spill cancelled");
    return backing_ ? backing_.spill(stop) : false;
}
bool Tile::trySpill(std::stop_token stop) const {
    if(stop.stop_requested()) throw io::SpillError(io::SpillErrorCode::Cancelled,"Tile spill cancelled");
    return backing_ ? backing_.trySpill(stop) : false;
}
PackedPixel Tile::ReadLease::pixel(int x,int y) const {
    if(x<0 || y<0 || x>=width_ || y>=height_) throw std::out_of_range("Pixel outside tile");
    if(backing_) return backing_.pixel(x,y);
    return uniform_ ? constant_ : owner_->data_[static_cast<std::size_t>(y*width_+x)];
}
Pixel Tile::ReadLease::linearPixel(int x,int y) const {return unpack(pixel(x,y));}
void Tile::ReadLease::copyCanonicalBytes(std::span<std::uint8_t> destination,std::stop_token stop) const {
    if(destination.size()!=static_cast<std::size_t>(width_)*static_cast<std::size_t>(height_)*8 || width_==0) throw std::invalid_argument("Incorrect canonical destination size");
    if(backing_) {backing_.copyCanonicalBytes(destination,stop); return;}
    std::size_t offset=0;
    for(int y=0;y<height_;++y) {
        if(stop.stop_requested()) throw io::SpillError(io::SpillErrorCode::Cancelled,"Tile copy cancelled");
        for(int x=0;x<width_;++x) for(auto value:pixel(x,y)) {
            destination[offset++]=static_cast<std::uint8_t>(value&255);
            destination[offset++]=static_cast<std::uint8_t>(value>>8);
        }
    }
    if(stop.stop_requested()) throw io::SpillError(io::SpillErrorCode::Cancelled,"Tile copy cancelled");
}
std::vector<std::uint8_t> Tile::ReadLease::canonicalBytes(std::stop_token stop) const {
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(width_)*static_cast<std::size_t>(height_)*8);
    copyCanonicalBytes(bytes,stop); return bytes;
}
PackedPixel Tile::pixel(int x,int y) const {
    if(backing_) return read().pixel(x,y);
    if(x<0 || y<0 || x>=width_ || y>=height_) throw std::out_of_range("Pixel outside tile");
    return uniform_ ? constant_ : data_[static_cast<std::size_t>(y*width_+x)];
}
Pixel Tile::linearPixel(int x,int y) const {
    if(backing_) return read().linearPixel(x,y);
    if(x<0 || y<0 || x>=width_ || y>=height_) throw std::out_of_range("Pixel outside tile");
    return uniform_ ? linearConstant_ : unpack(data_[static_cast<std::size_t>(y*width_+x)]);
}
bool Tile::samePixels(const Tile& other) const {
    if(this==&other) return true;
    if(width_!=other.width_ || height_!=other.height_ || uniform_!=other.uniform_ || constant_!=other.constant_) return false;
    if(!backing_ && !other.backing_) return data_==other.data_;
    const auto a=read(),b=other.read();
    for(int y=0;y<height_;++y) for(int x=0;x<width_;++x) if(a.pixel(x,y)!=b.pixel(x,y)) return false;
    return true;
}
std::vector<std::uint8_t> Tile::canonicalBytes() const {return read().canonicalBytes();}
void TileStore::setTileDeleterFactory(std::function<TileDeleter()> factory) {
    std::lock_guard lock(callbacksMutex_); deleterFactory_=std::move(factory);
}
void TileStore::setSpillCallbacks(std::function<void(std::size_t)> beforeAllocation,std::function<void(const TilePtr&)> observer) {
    std::lock_guard lock(callbacksMutex_); beforeAllocation_=std::move(beforeAllocation); observer_=std::move(observer);
}
TilePtr TileStore::build(int width, int height, PackedPixel first, bool uniform,
                         const std::function<PackedPixel(std::size_t)>& read) {
    const auto count=pixelCount(width,height);
    const auto charge=sizeof(Tile)+(uniform ? 0 : count*sizeof(PackedPixel));
    const bool disk=spill_ && !uniform;
    const auto budgetCharge=disk ? sizeof(Tile)+SpillTilePayload::metadataCharge()+io::SpillBacking::stateCharge() : charge;
    std::function<void(std::size_t)> beforeAllocation;
    std::function<void(const TilePtr&)> observer;
    std::function<TileDeleter()> deleterFactory;
    {std::lock_guard lock(callbacksMutex_); beforeAllocation=beforeAllocation_; observer=observer_; if(disk) deleterFactory=deleterFactory_;}
    TileDeleter deleter=deleterFactory ? deleterFactory() : TileDeleter{};
    if(deleterFactory && !deleter) throw std::logic_error("Tile retirement factory returned no deleter");
    if(!deleter) deleter=[](const Tile* value){delete value;};
    if(beforeAllocation) beforeAllocation(disk ? budgetCharge+count*8+io::SpillBacking::residentCharge(count*8) : charge);
    std::optional<MemoryAdmission::Reservation> memory;
    if(memory_) memory=memory_->require(disk ? sizeof(Tile) : charge);
    budget_->reserve(budgetCharge);
    bool owned=false;
    try {
        SpillTilePayload backing;
        std::vector<PackedPixel> data;
        if(disk) {
            auto scratch=memory_->require(count*8);
            std::vector<std::uint8_t> bytes(count*8); scratch.commit();
            for(std::size_t i=0;i<count;++i) {
                const auto packed=read(i);
                for(std::size_t c=0;c<4;++c) {
                    bytes[i*8+c*2]=static_cast<std::uint8_t>(packed[c]&255);
                    bytes[i*8+c*2+1]=static_cast<std::uint8_t>(packed[c]>>8);
                }
            }
            backing=SpillTilePayload::create(width,height,SpillTileKind::Dense,bytes,memory_,spill_);
        } else {
            data.resize(uniform ? 0 : count);
            for(std::size_t i=0;i<data.size();++i) data[i]=read(i);
        }
        auto version=nextVersion_.load();
        do {
            if(version==std::numeric_limits<std::int64_t>::max()) throw std::overflow_error("Tile version exhausted");
        } while(!nextVersion_.compare_exchange_weak(version,version+1));
        auto tile=std::unique_ptr<Tile,TileDeleter>(new Tile(budget_,charge,budgetCharge,version+1,width,height,uniform,first,std::move(data),std::move(memory),std::move(backing)),
            std::move(deleter));
        if(tile->memory_) tile->memory_->commit();
        owned=true;
        TilePtr result(std::move(tile));
        if(observer) observer(result);
        return result;
    } catch(...) {
        if(!owned) budget_->release(budgetCharge);
        throw;
    }
}
TilePtr TileStore::constant(int width, int height, Pixel value) {
    return build(width,height,pack(value),true,{});
}
TilePtr TileStore::create(int width, int height, std::span<const Pixel> pixels) {
    if(pixels.size()!=pixelCount(width,height)) throw std::invalid_argument("Incorrect pixel count");
    const auto first=pack(pixels.front());
    bool uniform=true;
    for(auto pixel:pixels) if(pack(pixel)!=first) uniform=false;
    return build(width,height,first,uniform,[&](std::size_t i){return pack(pixels[i]);});
}
TilePtr TileStore::fromCanonical(int width, int height, std::span<const std::uint8_t> bytes) {
    const auto count=pixelCount(width,height);
    if(bytes.size()!=count*8) throw std::invalid_argument("Incorrect decoded tile length");
    const auto read=[&](std::size_t i) {
        PackedPixel result{};
        for(std::size_t c=0;c<4;++c) result[c]=static_cast<Half>(bytes[i*8+c*2] | (static_cast<unsigned>(bytes[i*8+c*2+1])<<8));
        return result;
    };
    const auto first=read(0);
    bool uniform=true;
    for(std::size_t i=0;i<count;++i) {
        const auto p=read(i);
        validateCanonical(p);
        if(p!=first) uniform=false;
    }
    return build(width,height,first,uniform,read);
}
}
