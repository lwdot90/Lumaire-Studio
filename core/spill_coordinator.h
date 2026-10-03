#pragma once
#include "core/tiles.h"
#include <functional>
#include <exception>
#include <memory>
#include <stop_token>

namespace compositor::engine {
struct SpillCoordinatorLimits {
    std::size_t maxRegisteredTiles=4096;
    std::size_t maxPendingRequests=8;
    std::size_t maxPendingRetirements=64;
};
struct SpillReclaimResult {
    std::uint64_t droppedCachedBytes=0;
    std::uint64_t releasedTrackedBytes=0;
    std::size_t examined=0, failed=0;
    bool canceled=false, accepted=false;
    std::exception_ptr firstFailure;
};
// One IO thread, independent of compute admission. Registration never retains
// tiles. Synchronous reclaim belongs on a worker, never the UI thread.
class SpillCoordinator final {
public:
    explicit SpillCoordinator(std::shared_ptr<MemoryAdmission> memory,
        SpillCoordinatorLimits limits={});
    ~SpillCoordinator();
    SpillCoordinator(const SpillCoordinator&)=delete;
    SpillCoordinator& operator=(const SpillCoordinator&)=delete;
    bool registerTile(const TilePtr& tile);
    // Obtain before allocating a Tile. The fixed retirement slot is retained
    // by its custom deleter; exhaustion throws before a Tile is constructed.
    std::function<void(const Tile*)> makeTileDeleter();
    SpillReclaimResult reclaim(std::uint64_t bytes,std::stop_token stop={});
    bool requestReclaim(std::uint64_t bytes);
    // Accepted cleanup runs on the IO thread, including during shutdown.
    // Rejection leaves ownership with the caller: pass an lvalue callable
    // holding shared ownership, and retain it if this returns false.
    bool retire(const std::function<void()>& cleanup);
    void waitIdle();
    // Keep this fence before destroying the wrapper if Tiles outlive it. Call
    // after releasing all custom deleters/tiles; it waits for their cleanup.
    std::function<void()> retirementDrain() const;
    void shutdown();
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
