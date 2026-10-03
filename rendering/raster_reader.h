#pragma once
#include "core/render_source.h"
#include <optional>
#include <stdexcept>
#include <stop_token>

namespace compositor {
class RasterReadCancelled final:public std::runtime_error {
public:
    RasterReadCancelled():std::runtime_error("Raster read cancelled") {}
};
// Worker-local, one-entry lookup cache and one read-only resident lease.
// Shared sources use the production typed adapter; borrowed sources require
// the caller to retain the immutable raster. No shared mutable state.
class RasterReader {
public:
    explicit RasterReader(const engine::RasterSnapshot& raster):raster_(raster),default_(engine::unpack(raster.defaultValue)) {}
    explicit RasterReader(std::shared_ptr<const engine::RasterSnapshot> raster)
        :raster_(checked(raster)),default_(engine::unpack(raster_.defaultValue)),
         sourceReader_(std::in_place,std::make_shared<const engine::RenderSource>(std::move(raster))) {}
    engine::Pixel pixel(std::int64_t x,std::int64_t y,const std::function<bool()>& cancelled={},std::stop_token stop={}) {
        if(stop.stop_requested() || (cancelled && cancelled())) {release();throw RasterReadCancelled{};}
        if(sourceReader_) {
            engine::Pixel value;
            try {value=sourceReader_->pixel(x,y,stop);}
            catch(...) {if(stop.stop_requested()) {release();throw RasterReadCancelled{};}throw;}
            if(stop.stop_requested() || (cancelled && cancelled())) {release();throw RasterReadCancelled{};}
            return value;
        }
        const auto& extent=raster_.extent;
        if(x<extent.x || y<extent.y || x>=extent.x+extent.width || y>=extent.y+extent.height) return {};
        if(!valid_ || x<left_ || y<top_ || x>=right_ || y>=bottom_) {
            lease_.reset();
            const engine::TileCoord coordinate{engine::floorTile(x),engine::floorTile(y)};
            const auto found=raster_.tiles.find(coordinate);
            tile_=found==raster_.tiles.end() ? nullptr : found->second.get();
            const auto region=engine::tileExtent(extent,coordinate);
            left_=region.x;top_=region.y;right_=left_+region.width;bottom_=top_+region.height;
            valid_=true;++lookups_;
        }
        if(!tile_) return default_;
        if(!lease_) {
            try {lease_.emplace(tile_->read(stop));}
            catch(...) {if(stop.stop_requested()) throw RasterReadCancelled{};throw;}
        }
        if(stop.stop_requested() || (cancelled && cancelled())) {release();throw RasterReadCancelled{};}
        return lease_->linearPixel(static_cast<int>(x-left_),static_cast<int>(y-top_));
    }
    void release() {lease_.reset();if(sourceReader_) sourceReader_->release();}
    bool leased() const {return sourceReader_ ? sourceReader_->hasLease() : lease_.has_value();}
    std::size_t lookups() const {return sourceReader_ ? sourceReader_->lookups() : lookups_;}
private:
    static const engine::RasterSnapshot& checked(const std::shared_ptr<const engine::RasterSnapshot>& raster) {
        if(!raster) throw std::invalid_argument("Raster reader requires a source");
        return *raster;
    }
    const engine::RasterSnapshot& raster_;
    engine::Pixel default_;
    std::optional<engine::SourceReadContext> sourceReader_;
    const engine::Tile* tile_=nullptr;
    std::optional<engine::Tile::ReadLease> lease_;
    std::int64_t left_=0,top_=0,right_=0,bottom_=0;
    bool valid_=false;
    std::size_t lookups_=0;
};
}
