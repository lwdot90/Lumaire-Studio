#pragma once
#include "core/pixels.h"
#include "core/memory_admission.h"
#include "core/spill_tile_payload.h"
#include <mutex>
#include <stop_token>
#include <stdexcept>
#include <atomic>
#include <compare>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <vector>

namespace compositor::engine {
inline constexpr std::int64_t tileSide=256;
struct TileCoord {
    std::int64_t x=0, y=0;
    auto operator<=>(const TileCoord&) const = default;
};
struct Extent {
    std::int64_t x=0, y=0, width=1, height=1;
    bool operator==(const Extent&) const = default;
    void validate() const;
    bool contains(std::int64_t px, std::int64_t py) const;
};
std::int64_t floorTile(std::int64_t coordinate);
Extent tileExtent(const Extent& asset, TileCoord coordinate);

// Accounts immutable metadata, including versions held only by history/jobs.
// Resident-only tiles also charge canonical payload; disk-enabled payload uses
// MemoryAdmission independently. Container nodes and caller scratch are separate.
class TileBudget {
public:
    explicit TileBudget(std::size_t limit):limit_(limit) {}
    std::size_t used() const { return used_.load(); }
    void reserve(std::size_t bytes);
    void release(std::size_t bytes) noexcept { used_.fetch_sub(bytes); }
private:
    const std::size_t limit_;
    std::atomic<std::size_t> used_{0};
};
class Tile final : public std::enable_shared_from_this<Tile> {
public:
    class ReadLease {
    public:
        ReadLease()=default;
        explicit operator bool() const {return width_>0 && height_>0;}
        int width() const {return width_;}
        int height() const {return height_;}
        bool uniform() const {return uniform_;}
        PackedPixel pixel(int x,int y) const;
        Pixel linearPixel(int x,int y) const;
        void copyCanonicalBytes(std::span<std::uint8_t> destination,std::stop_token stop={}) const;
        std::vector<std::uint8_t> canonicalBytes(std::stop_token stop={}) const;
    private:
        friend class Tile;
        std::shared_ptr<const Tile> owner_;
        SpillTileReadLease backing_;
        int width_=0,height_=0;
        bool uniform_=true;
        PackedPixel constant_{};
    };
    ReadLease read(std::stop_token stop={}) const;
    bool spill(std::stop_token stop={}) const;
    bool trySpill(std::stop_token stop={}) const;
    bool spillable() const {return bool(backing_);}
    io::SpillBackingStatus spillStatus() const {return backing_ ? backing_.status() : io::SpillBackingStatus{};}
    ~Tile();
    Tile(const Tile&)=delete;
    Tile& operator=(const Tile&)=delete;
    std::int64_t version() const { return version_; }
    int width() const { return width_; }
    int height() const { return height_; }
    bool uniform() const { return uniform_; }
    std::size_t retainedBytes() const { return charge_; }
    PackedPixel pixel(int x, int y) const;
    Pixel linearPixel(int x,int y) const;
    bool samePixels(const Tile& other) const;
    // Explicit little-endian encoding, independent of native byte order.
    std::vector<std::uint8_t> canonicalBytes() const;
private:
    friend class TileStore;
    Tile(std::shared_ptr<TileBudget> budget, std::size_t charge, std::size_t budgetCharge, std::int64_t version,
         int width, int height, bool uniform, PackedPixel constant, std::vector<PackedPixel> data,
         std::optional<MemoryAdmission::Reservation> memory, SpillTilePayload backing={});
    std::shared_ptr<TileBudget> budget_;
    std::size_t charge_,budgetCharge_;
    std::int64_t version_;
    int width_, height_;
    // Representation identity must survive future resident-payload eviction.
    const bool uniform_;
    PackedPixel constant_;
    Pixel linearConstant_;
    std::optional<MemoryAdmission::Reservation> memory_;
    std::vector<PackedPixel> data_;
    SpillTilePayload backing_;
};
using TilePtr=std::shared_ptr<const Tile>;
using TileMap=std::map<TileCoord,TilePtr>;
class TileStore {
public:
    explicit TileStore(std::size_t limit,std::shared_ptr<MemoryAdmission> memory={},std::shared_ptr<io::SpillStore> spill={})
        :budget_(std::make_shared<TileBudget>(limit)),memory_(std::move(memory)),spill_(std::move(spill)) {
        if(spill_ && !memory_) throw std::invalid_argument("Spill-enabled tiles require memory admission");
    }
    using TileDeleter=std::function<void(const Tile*)>;
    void setTileDeleterFactory(std::function<TileDeleter()> factory);
    void setSpillCallbacks(std::function<void(std::size_t)> beforeAllocation,
                           std::function<void(const TilePtr&)> observer);
    TilePtr constant(int width, int height, Pixel value);
    TilePtr create(int width, int height, std::span<const Pixel> pixels);
    TilePtr fromCanonical(int width, int height, std::span<const std::uint8_t> bytes);
    std::size_t usedBytes() const { return budget_->used(); }
    const std::shared_ptr<MemoryAdmission>& memoryAdmission() const {return memory_;}
private:
    TilePtr build(int width, int height, PackedPixel first, bool uniform,
                  const std::function<PackedPixel(std::size_t)>& read);
    std::shared_ptr<TileBudget> budget_;
    std::shared_ptr<MemoryAdmission> memory_;
    std::shared_ptr<io::SpillStore> spill_;
    std::mutex callbacksMutex_;
    std::function<void(std::size_t)> beforeAllocation_;
    std::function<void(const TilePtr&)> observer_;
    std::function<TileDeleter()> deleterFactory_;
    std::atomic<std::int64_t> nextVersion_{0};
};
}
