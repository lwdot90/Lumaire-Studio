#include "rendering/tile_residency.h"
#include <stdexcept>

namespace compositor {
TileResidency::TileResidency(std::size_t byteBudget):entries_(byteBudget/slotBytes) {}
void TileResidency::begin(std::uint64_t completed) {
    if(pending_) throw std::logic_error("Tile batch already pending");
    if(completed<completed_ || completed>submitted_)
        throw std::invalid_argument("Invalid tile retirement timeline");
    completed_=completed;
    pending_=true;
}
std::optional<TileResidency::Lease> TileResidency::acquire(const engine::TilePtr& tile) {
    if(!pending_) throw std::logic_error("No pending tile batch");
    if(!tile) throw std::invalid_argument("Null resident tile");
    for(std::size_t i=0;i<entries_.size();++i) {
        auto& entry=entries_[i];
        if(entry.tile==tile) {
            entry.reserved=true;
            // A repeated acquisition in the same batch does not request a
            // second upload; the first lease already carries that obligation.
            return Lease{i,false};
        }
    }
    std::optional<std::size_t> oldest;
    for(std::size_t i=0;i<entries_.size();++i) {
        const auto& entry=entries_[i];
        if(entry.reserved || entry.lastUse>completed_) continue;
        if(!entry.tile) { oldest=i; break; }
        if(!oldest || entry.lastUse<entries_[*oldest].lastUse) oldest=i;
    }
    if(!oldest) return std::nullopt;
    entries_[*oldest]=Entry{tile,0,true,false};
    return Lease{*oldest,true};
}
void TileResidency::submitted(std::uint64_t serial) {
    if(!pending_) throw std::logic_error("No pending tile batch");
    if(serial<=submitted_) throw std::invalid_argument("Tile submission must advance timeline");
    for(auto& entry:entries_) if(entry.reserved) {
        entry.lastUse=serial;
        entry.uploaded=true;
        entry.reserved=false;
    }
    submitted_=serial;
    pending_=false;
}
void TileResidency::cancel() {
    if(!pending_) throw std::logic_error("No pending tile batch");
    for(auto& entry:entries_) if(entry.reserved) {
        if(!entry.uploaded) entry=Entry{};
        else entry.reserved=false;
    }
    pending_=false;
}
std::size_t TileResidency::residentCount() const {
    std::size_t count=0;
    for(const auto& entry:entries_) if(entry.tile) ++count;
    return count;
}
}
