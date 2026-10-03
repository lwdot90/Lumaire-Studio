#pragma once
#include "core/pixels.h"
#include "io/spill_backing.h"
#include <memory>
#include <span>
#include <stop_token>
#include <string_view>
#include <utility>

namespace compositor::engine {
enum class SpillTileKind { Dense, Constant };
namespace detail { struct SpillTilePayloadState; }

// A fully validated resident view. Hold this lease for every use of its span.
class SpillTileReadLease final {
public:
    SpillTileReadLease()=default;
    explicit operator bool() const noexcept {return bool(resident_);}
    int width() const noexcept {return width_;}
    int height() const noexcept {return height_;}
    SpillTileKind kind() const noexcept {return kind_;}
    PackedPixel pixel(int x,int y) const;
    Pixel linearPixel(int x,int y) const;
    // Dense stores width*height*8 bytes; Constant stores one canonical pixel.
    std::span<const std::uint8_t> storedBytes() const noexcept {return resident_.bytes();}
    std::uint64_t chargedResidentBytes() const noexcept {return resident_.chargedBytes();}
    std::size_t canonicalByteCount() const noexcept;
    // Full row-major canonical bytes, expanding Constant without allocating.
    // Discard the destination if cancellation interrupts the copy.
    void copyCanonicalBytes(std::span<std::uint8_t> destination,std::stop_token stop={}) const;
private:
    friend class SpillTilePayload;
    SpillTileReadLease(io::ResidentLease resident,int width,int height,SpillTileKind kind)
        :resident_(std::move(resident)),width_(width),height_(height),kind_(kind) {}
    io::ResidentLease resident_;
    int width_=0,height_=0;
    SpillTileKind kind_=SpillTileKind::Dense;
};

// Session-local legacy working pixels. Copies share typed identity and residency.
// This does not change Tile, document/history, placement or native file storage.
class SpillTilePayload final {
public:
    static constexpr std::string_view encoding="rgba16f-le";
    SpillTilePayload()=default;
    static SpillTilePayload create(int width,int height,SpillTileKind kind,
        std::span<const std::uint8_t> bytes,std::shared_ptr<MemoryAdmission> memory,
        std::shared_ptr<io::SpillStore> store,std::stop_token stop={},io::SpillBackingFault fault={});
    // Eagerly admit/read/verify canonical pixels before returning a typed object.
    static SpillTilePayload fromSpill(int width,int height,SpillTileKind kind,const io::SpillHandle& handle,
        std::shared_ptr<MemoryAdmission> memory,std::stop_token stop={},io::SpillBackingFault fault={});
    explicit operator bool() const noexcept {return bool(state_);}
    int width() const noexcept;
    int height() const noexcept;
    SpillTileKind kind() const;
    std::uint64_t size() const noexcept;
    bool sameIdentity(const SpillTilePayload& other) const noexcept;
    SpillTileReadLease resident(std::stop_token stop={}) const;
    bool spill(std::stop_token stop={}) const;
    bool trySpill(std::stop_token stop={}) const;
    io::SpillHandle spillHandle() const;
    io::SpillBackingStatus status() const;
    static std::uint64_t metadataCharge() noexcept;
private:
    explicit SpillTilePayload(std::shared_ptr<const detail::SpillTilePayloadState> state):state_(std::move(state)) {}
    std::shared_ptr<const detail::SpillTilePayloadState> state_;
};
}
