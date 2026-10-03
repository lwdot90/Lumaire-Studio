#pragma once
#include "core/raster.h"
#include "core/resampling.h"
#include "rendering/raster_reader.h"
#include <list>
#include <unordered_map>

namespace compositor {
namespace engine {struct DocumentSnapshot;}
// Worker-owned, demand-built FP32 mip pieces. Adapted from DownsampleCache's
// immutable source identity, successive stable halvings and bounded LRU policy.
// No locks: each renderer/worker owns its cache; never share it across workers.
class MipCache {
public:
    static constexpr std::int64_t side=32;
    static constexpr std::size_t slotBytes=side*side*sizeof(engine::Pixel);
    explicit MipCache(std::size_t payloadBudget,std::shared_ptr<MemoryAdmission> memory={});
    MipCache(const MipCache&)=delete;
    MipCache& operator=(const MipCache&)=delete;
    std::optional<engine::Pixel> pixel(const std::shared_ptr<const engine::RasterSnapshot>& source,
        unsigned level,std::int64_t x,std::int64_t y,const std::function<bool()>& cancelled={},std::stop_token stop={});
    void releaseSourceLease() {if(baseReader_) baseReader_->release();}
    void clear();
    void prune(const engine::DocumentSnapshot& document);
    std::size_t residentBytes() const {return entries_.size()*slotBytes;}
    std::size_t residentPieces() const {return entries_.size();}
    std::size_t builtPieces() const {return built_;}
private:
    struct Key {
        const engine::RasterSnapshot* source;
        unsigned level;
        std::int64_t x,y;
        bool operator==(const Key&) const = default;
    };
    struct Hash {std::size_t operator()(const Key& key) const;};
    struct Entry {
        Key key;
        std::shared_ptr<const engine::RasterSnapshot> source;
        std::optional<MemoryAdmission::Reservation> memory;
        engine::FilteredRegion region;
    };
    using List=std::list<Entry>;
    const std::size_t capacity_;
    std::shared_ptr<MemoryAdmission> memory_;
    std::size_t built_=0;
    std::shared_ptr<const engine::RasterSnapshot> baseSource_;
    std::optional<RasterReader> baseReader_;
    List entries_;
    std::unordered_map<Key,List::iterator,Hash> index_;
};
}
