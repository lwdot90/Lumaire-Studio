#pragma once
#include "core/tiles.h"
#include <cstdint>
#include <optional>
#include <vector>

namespace compositor {
// Single render-worker policy for a fixed-size GPU tile pool. No Vulkan calls,
// allocation of GPU memory, waits, or document mutation. Each slot must have
// storage for a full canonical 256x256 RGBA16F tile, including uniform tiles.
// Identity is the retained Tile object, NOT its store-local version number.
// All uploads and reads use one ordered queue. Separate queues need additional
// dependency/ownership handling and must not use this policy unmodified.
class TileResidency {
public:
    static constexpr std::size_t slotBytes=256*256*8;
    struct Lease {
        std::size_t slot;
        // True only on first acquisition of a new tile in this batch. The
        // caller must preserve that upload obligation when deduplicating work.
        bool upload;
    };
    explicit TileResidency(std::size_t byteBudget);
    // completed is the observed value of this pool's ONE submission timeline.
    // Only one unsubmitted batch is allowed. Leases are valid for that batch;
    // consumers must reacquire them for later batches.
    void begin(std::uint64_t completed);
    // Empty means pressure: submit/cancel this batch or wait asynchronously for
    // retirement. Never evicts a pending or in-flight tile to satisfy pressure.
    std::optional<Lease> acquire(const engine::TilePtr& tile);
    // Call only AFTER successful queue submission which uploads every lease
    // marked upload and signals serial after ALL uses of these slots. The
    // caller supplies barriers and keeps staging alive through retirement.
    void submitted(std::uint64_t serial);
    // Only for a batch never submitted to the GPU. New uploads are discarded.
    void cancel();
    std::size_t capacity() const { return entries_.size(); }
    std::size_t residentCount() const;
    bool pending() const { return pending_; }
private:
    struct Entry {
        engine::TilePtr tile;
        std::uint64_t lastUse=0;
        bool reserved=false;
        bool uploaded=false;
    };
    std::vector<Entry> entries_;
    std::uint64_t completed_=0,submitted_=0;
    bool pending_=false;
};
}
