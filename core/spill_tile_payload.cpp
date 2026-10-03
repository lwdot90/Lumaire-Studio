#include "core/spill_tile_payload.h"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace compositor::engine {
namespace {
void cancelled(std::stop_token stop) {
    if(stop.stop_requested()) throw io::SpillError(io::SpillErrorCode::Cancelled,"Tile payload operation cancelled");
}
std::size_t storedSize(int width,int height,SpillTileKind kind) {
    if(width<1 || height<1 || width>256 || height>256) throw std::invalid_argument("Invalid spill tile dimensions");
    if(kind!=SpillTileKind::Dense && kind!=SpillTileKind::Constant) throw std::invalid_argument("Invalid spill tile kind");
    return kind==SpillTileKind::Constant ? 8 : static_cast<std::size_t>(width)*static_cast<std::size_t>(height)*8;
}
PackedPixel readPacked(std::span<const std::uint8_t> bytes,std::size_t offset) {
    PackedPixel pixel{};
    for(std::size_t channel=0;channel<4;++channel)
        pixel[channel]=static_cast<Half>(unsigned(bytes[offset+channel*2]) | (unsigned(bytes[offset+channel*2+1])<<8));
    return pixel;
}
void validateBytes(std::span<const std::uint8_t> bytes,std::size_t expected,std::stop_token stop) {
    cancelled(stop);
    if(bytes.size()!=expected) throw std::invalid_argument("Spill tile byte length does not match its kind and dimensions");
    for(std::size_t offset=0;offset<bytes.size();offset+=8) {
        if(offset%2048==0) cancelled(stop);
        validateCanonical(readPacked(bytes,offset));
    }
    cancelled(stop);
}
}
namespace detail {
struct SpillTilePayloadState {
    // The backing and metadata retire before their metadata admission charge.
    MemoryAdmission::Reservation reservation;
    const int width,height;
    const SpillTileKind kind;
    const io::SpillBacking backing;
    SpillTilePayloadState(MemoryAdmission::Reservation claim,int w,int h,SpillTileKind type,io::SpillBacking storage)
        :reservation(std::move(claim)),width(w),height(h),kind(type),backing(std::move(storage)) {reservation.commit();}
};
}
PackedPixel SpillTileReadLease::pixel(int x,int y) const {
    if(!resident_ || x<0 || y<0 || x>=width_ || y>=height_) throw std::out_of_range("Pixel outside resident spill tile");
    const auto index=kind_==SpillTileKind::Constant ? 0 : static_cast<std::size_t>(y)*static_cast<std::size_t>(width_)+static_cast<std::size_t>(x);
    return readPacked(resident_.bytes(),index*8);
}
Pixel SpillTileReadLease::linearPixel(int x,int y) const {return unpack(pixel(x,y));}
std::size_t SpillTileReadLease::canonicalByteCount() const noexcept {
    return static_cast<std::size_t>(width_)*static_cast<std::size_t>(height_)*8;
}
void SpillTileReadLease::copyCanonicalBytes(std::span<std::uint8_t> destination,std::stop_token stop) const {
    cancelled(stop);
    if(!resident_ || destination.size()!=canonicalByteCount()) throw std::invalid_argument("Canonical copy requires a live tile lease and exact-sized destination");
    const auto source=resident_.bytes();
    if(kind_==SpillTileKind::Dense) {
        constexpr std::size_t chunk=64*1024;
        for(std::size_t offset=0;offset<source.size();) {
            cancelled(stop);
            const auto count=std::min(chunk,source.size()-offset);
            std::copy_n(source.data()+offset,count,destination.data()+offset); offset+=count;
        }
    } else for(std::size_t offset=0;offset<destination.size();offset+=8) {
        if(offset%2048==0) cancelled(stop);
        std::copy_n(source.data(),8,destination.data()+offset);
    }
    cancelled(stop);
}
std::uint64_t SpillTilePayload::metadataCharge() noexcept {return sizeof(detail::SpillTilePayloadState)+64;}
SpillTilePayload SpillTilePayload::create(int width,int height,SpillTileKind kind,
    std::span<const std::uint8_t> bytes,std::shared_ptr<MemoryAdmission> memory,
    std::shared_ptr<io::SpillStore> store,std::stop_token stop,io::SpillBackingFault fault) {
    cancelled(stop);
    const auto expected=storedSize(width,height,kind);
    if(bytes.size()!=expected || !memory) throw std::invalid_argument("Tile payload requires exact-sized bytes and memory admission");
    auto claim=memory->require(metadataCharge());
    auto backing=io::SpillBacking::create(bytes,std::move(memory),std::move(store),stop,std::move(fault));
    {
        const auto lease=backing.resident(stop);
        validateBytes(lease.bytes(),expected,stop);
    }
    auto state=std::make_shared<detail::SpillTilePayloadState>(std::move(claim),width,height,kind,std::move(backing));
    cancelled(stop);
    return SpillTilePayload(std::move(state));
}
SpillTilePayload SpillTilePayload::fromSpill(int width,int height,SpillTileKind kind,const io::SpillHandle& handle,
    std::shared_ptr<MemoryAdmission> memory,std::stop_token stop,io::SpillBackingFault fault) {
    cancelled(stop);
    const auto expected=storedSize(width,height,kind);
    if(!handle || handle.size()!=expected || !memory) throw std::invalid_argument("Tile payload requires an exact-sized spill handle and memory admission");
    auto claim=memory->require(metadataCharge());
    auto backing=io::SpillBacking::fromSpill(handle,std::move(memory),stop,std::move(fault));
    {
        const auto lease=backing.resident(stop);
        validateBytes(lease.bytes(),expected,stop);
    }
    auto state=std::make_shared<detail::SpillTilePayloadState>(std::move(claim),width,height,kind,std::move(backing));
    cancelled(stop);
    return SpillTilePayload(std::move(state));
}
int SpillTilePayload::width() const noexcept {return state_ ? state_->width : 0;}
int SpillTilePayload::height() const noexcept {return state_ ? state_->height : 0;}
SpillTileKind SpillTilePayload::kind() const {
    if(!state_) throw std::logic_error("Empty spill tile has no kind");
    return state_->kind;
}
std::uint64_t SpillTilePayload::size() const noexcept {return state_ ? state_->backing.size() : 0;}
bool SpillTilePayload::sameIdentity(const SpillTilePayload& other) const noexcept {return state_ && state_==other.state_;}
SpillTileReadLease SpillTilePayload::resident(std::stop_token stop) const {
    const auto state=state_; cancelled(stop);
    if(!state) throw std::logic_error("Cannot read an empty spill tile");
    auto lease=state->backing.resident(stop);
    // Factories validate immutable canonical bytes once; cache misses verify
    // their original opaque checksum before publication. Cache hits need no scan.
    return SpillTileReadLease(std::move(lease),state->width,state->height,state->kind);
}
bool SpillTilePayload::spill(std::stop_token stop) const {
    const auto state=state_; cancelled(stop);
    if(!state) throw std::logic_error("Cannot spill an empty tile payload");
    return state->backing.spill(stop);
}
bool SpillTilePayload::trySpill(std::stop_token stop) const {
    const auto state=state_; cancelled(stop);
    if(!state) throw std::logic_error("Cannot spill an empty tile payload");
    return state->backing.trySpill(stop);
}
io::SpillHandle SpillTilePayload::spillHandle() const {return state_ ? state_->backing.spillHandle() : io::SpillHandle{};}
io::SpillBackingStatus SpillTilePayload::status() const {return state_ ? state_->backing.status() : io::SpillBackingStatus{};}
}
