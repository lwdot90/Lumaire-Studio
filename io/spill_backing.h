#pragma once
#include "core/memory_admission.h"
#include "io/spill_store.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <utility>

namespace compositor::io {
namespace detail { struct SpillBackingState; struct SpillResidentBlock; }
enum class SpillBackingStage { AllocateResident, PublishResident, PublishSpill };
// Test seam, empty in production. May throw or request stop. Must be thread-safe
// and must not reenter residency operations on this backing.
using SpillBackingFault=std::function<void(SpillBackingStage,std::uint64_t)>;

class ResidentLease final {
public:
    ResidentLease()=default;
    explicit operator bool() const noexcept { return bool(block_); }
    std::span<const std::uint8_t> bytes() const noexcept;
    std::uint64_t size() const noexcept;
    std::uint64_t chargedBytes() const noexcept;
private:
    friend class SpillBacking;
    explicit ResidentLease(std::shared_ptr<const detail::SpillResidentBlock> block):block_(std::move(block)) {}
    std::shared_ptr<const detail::SpillResidentBlock> block_;
};
struct SpillBackingStatus {
    bool resident=false, spilled=false, busy=false;
    std::uint64_t residentCharge=0;
};

// Copies share one immutable payload and its mutable residency state. IO stays
// on the calling worker; see docs/spill-backing-contract.md for ownership rules.
class SpillBacking final {
public:
    SpillBacking()=default;
    static SpillBacking create(std::span<const std::uint8_t> bytes,
        std::shared_ptr<MemoryAdmission> memory, std::shared_ptr<SpillStore> store,
        std::stop_token stop={}, SpillBackingFault fault={});
    static SpillBacking fromSpill(const SpillHandle& handle, std::shared_ptr<MemoryAdmission> memory,
        std::stop_token stop={}, SpillBackingFault fault={});
    explicit operator bool() const noexcept { return bool(state_); }
    std::uint64_t size() const noexcept;
    ResidentLease resident(std::stop_token stop={}) const;
    // True if this call dropped a cached copy; false if already disk-only.
    bool spill(std::stop_token stop={}) const;
    // Atomically skips an occupied transition; pressure workers must not wait on rehydration.
    bool trySpill(std::stop_token stop={}) const;
    SpillHandle spillHandle() const;
    SpillBackingStatus status() const;
    static std::uint64_t stateCharge() noexcept;
    static std::uint64_t residentCharge(std::uint64_t size);
private:
    bool spillImpl(std::stop_token stop,bool wait) const;
    explicit SpillBacking(std::shared_ptr<detail::SpillBackingState> state):state_(std::move(state)) {}
    std::shared_ptr<detail::SpillBackingState> state_;
};
}
