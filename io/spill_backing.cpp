#include "io/spill_backing.h"
#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace compositor::io {
namespace {
constexpr std::uint64_t controlAllowance=64;
constexpr std::size_t copyChunk=64*1024;
void cancelled(std::stop_token stop) {
    if(stop.stop_requested()) throw SpillError(SpillErrorCode::Cancelled,"Spill backing operation cancelled");
}
void invalid(const char* message) { throw SpillError(SpillErrorCode::InvalidArgument,message); }
}
namespace detail {
struct SpillResidentBlock {
    // Destroy the array before releasing its admission charge.
    MemoryAdmission::Reservation reservation;
    const std::uint64_t size;
    std::unique_ptr<std::uint8_t[]> data;
    SpillResidentBlock(MemoryAdmission::Reservation claim, std::uint64_t length)
        :reservation(std::move(claim)),size(length),
         data(length ? std::make_unique_for_overwrite<std::uint8_t[]>(static_cast<std::size_t>(length)) : nullptr) {
        reservation.commit();
    }
};
struct SpillBackingState {
    // All cache/handle members retire before the state's metadata charge.
    MemoryAdmission::Reservation reservation;
    const std::uint64_t size;
    const std::shared_ptr<MemoryAdmission> memory;
    const std::shared_ptr<SpillStore> store;
    const SpillBackingFault fault;
    std::mutex mutex;
    std::condition_variable_any ready;
    bool busy=false;
    std::shared_ptr<const SpillResidentBlock> cached;
    SpillHandle disk;
    SpillBackingState(MemoryAdmission::Reservation claim, std::uint64_t length,
        std::shared_ptr<MemoryAdmission> admission, std::shared_ptr<SpillStore> spill, SpillBackingFault callback)
        :reservation(std::move(claim)),size(length),memory(std::move(admission)),store(std::move(spill)),fault(std::move(callback)) {
        reservation.commit();
    }
    void checkpoint(SpillBackingStage stage, std::stop_token stop) const {
        cancelled(stop);
        if(fault) fault(stage,size);
        cancelled(stop);
    }
    std::shared_ptr<SpillResidentBlock> allocateResident(std::stop_token stop) const {
        cancelled(stop);
        auto claim=memory->require(SpillBacking::residentCharge(size),MemoryAdmission::Kind::Cpu);
        checkpoint(SpillBackingStage::AllocateResident,stop);
        auto block=std::make_shared<SpillResidentBlock>(std::move(claim),size);
        cancelled(stop);
        return block;
    }
    void wait(std::unique_lock<std::mutex>& lock, std::stop_token stop, bool allowCache) {
        if(!ready.wait(lock,stop,[&]{return !busy || (allowCache && bool(cached));})) cancelled(stop);
        cancelled(stop);
    }
};
}
namespace {
std::shared_ptr<detail::SpillBackingState> createState(std::uint64_t size,
    std::shared_ptr<MemoryAdmission> memory, std::shared_ptr<SpillStore> store,
    SpillBackingFault fault, std::stop_token stop) {
    cancelled(stop);
    if(!memory) invalid("Spill backing requires memory admission");
    SpillBacking::residentCharge(size); // Check representable array/charge lengths before state allocation.
    auto claim=memory->require(SpillBacking::stateCharge(),MemoryAdmission::Kind::Cpu);
    cancelled(stop);
    auto state=std::make_shared<detail::SpillBackingState>(std::move(claim),size,
        std::move(memory),std::move(store),std::move(fault));
    cancelled(stop);
    return state;
}
class Transition {
public:
    explicit Transition(detail::SpillBackingState& state):state_(state) {}
    ~Transition() {
        { std::lock_guard lock(state_.mutex); state_.busy=false; }
        state_.ready.notify_all();
    }
    Transition(const Transition&)=delete;
    Transition& operator=(const Transition&)=delete;
private:
    detail::SpillBackingState& state_;
};
}
std::span<const std::uint8_t> ResidentLease::bytes() const noexcept {
    return block_ ? std::span<const std::uint8_t>(block_->data.get(),static_cast<std::size_t>(block_->size))
                  : std::span<const std::uint8_t>{};
}
std::uint64_t ResidentLease::size() const noexcept { return block_ ? block_->size : 0; }
std::uint64_t ResidentLease::chargedBytes() const noexcept { return block_ ? block_->reservation.bytes() : 0; }
std::uint64_t SpillBacking::stateCharge() noexcept { return sizeof(detail::SpillBackingState)+controlAllowance; }
std::uint64_t SpillBacking::residentCharge(std::uint64_t size) {
    constexpr auto overhead=sizeof(detail::SpillResidentBlock)+controlAllowance;
    if(size>std::numeric_limits<std::size_t>::max() || size>std::numeric_limits<std::uint64_t>::max()-overhead)
        throw std::length_error("Spill backing payload or memory charge overflow");
    return size+overhead;
}
SpillBacking SpillBacking::create(std::span<const std::uint8_t> bytes,
    std::shared_ptr<MemoryAdmission> memory, std::shared_ptr<SpillStore> store,
    std::stop_token stop, SpillBackingFault fault) {
    cancelled(stop);
    if(!store) invalid("Resident spill backing requires a spill store");
    auto state=createState(bytes.size(),std::move(memory),std::move(store),std::move(fault),stop);
    auto block=state->allocateResident(stop);
    for(std::size_t offset=0;offset<bytes.size();) {
        cancelled(stop);
        const auto count=std::min(bytes.size()-offset,copyChunk);
        std::copy_n(bytes.data()+offset,count,block->data.get()+offset);
        offset+=count;
    }
    state->checkpoint(SpillBackingStage::PublishResident,stop);
    state->cached=std::move(block);
    return SpillBacking(std::move(state));
}
SpillBacking SpillBacking::fromSpill(const SpillHandle& handle, std::shared_ptr<MemoryAdmission> memory,
    std::stop_token stop, SpillBackingFault fault) {
    cancelled(stop);
    if(!handle) invalid("Spill backing requires a live spill handle");
    auto state=createState(handle.size(),std::move(memory),{},std::move(fault),stop);
    cancelled(stop);
    state->disk=handle;
    return SpillBacking(std::move(state));
}
std::uint64_t SpillBacking::size() const noexcept { return state_ ? state_->size : 0; }
ResidentLease SpillBacking::resident(std::stop_token stop) const {
    const auto state=state_;
    cancelled(stop);
    if(!state) invalid("Cannot read an empty spill backing");
    SpillHandle disk;
    {
        std::unique_lock lock(state->mutex);
        state->wait(lock,stop,true);
        if(state->cached) return ResidentLease(state->cached);
        disk=state->disk;
        state->busy=true;
    }
    Transition transition(*state);
    auto block=state->allocateResident(stop);
    disk.readInto({block->data.get(),static_cast<std::size_t>(block->size)},stop);
    state->checkpoint(SpillBackingStage::PublishResident,stop);
    {
        std::lock_guard lock(state->mutex);
        cancelled(stop);
        state->cached=block;
    }
    return ResidentLease(std::move(block));
}
bool SpillBacking::spill(std::stop_token stop) const {return spillImpl(stop,true);}
bool SpillBacking::trySpill(std::stop_token stop) const {return spillImpl(stop,false);}
bool SpillBacking::spillImpl(std::stop_token stop,bool wait) const {
    const auto state=state_;
    cancelled(stop);
    if(!state) invalid("Cannot spill an empty backing");
    std::shared_ptr<const detail::SpillResidentBlock> source;
    SpillHandle disk;
    {
        std::unique_lock lock(state->mutex,std::defer_lock);
        if(wait) {lock.lock(); state->wait(lock,stop,false);}
        else {
            if(!lock.try_lock() || state->busy) return false;
            cancelled(stop);
        }
        if(!state->cached) return false;
        source=state->cached;
        disk=state->disk;
        state->busy=true;
    }
    Transition transition(*state);
    // Retire an unpublished handle before waking another transition on failure.
    SpillHandle candidate=std::move(disk);
    if(!candidate) {
        if(!state->store) invalid("Spill backing has no writable store");
        candidate=state->store->put({source->data.get(),static_cast<std::size_t>(source->size)},stop);
    }
    state->checkpoint(SpillBackingStage::PublishSpill,stop);
    {
        std::lock_guard lock(state->mutex);
        cancelled(stop);
        state->disk=std::move(candidate);
        state->cached.reset(); // source and outstanding leases still pin this block.
    }
    source.reset(); // Release on the calling worker, outside the state mutex.
    return true;
}
SpillHandle SpillBacking::spillHandle() const {
    const auto state=state_;
    if(!state) return {};
    std::lock_guard lock(state->mutex);
    return state->disk;
}
SpillBackingStatus SpillBacking::status() const {
    const auto state=state_;
    if(!state) return {};
    std::lock_guard lock(state->mutex);
    return {bool(state->cached),bool(state->disk),state->busy,
        state->cached ? state->cached->reservation.bytes() : 0};
}
}
