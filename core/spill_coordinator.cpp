#include "core/spill_coordinator.h"
#include <algorithm>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace compositor::engine {
namespace {
std::uint64_t add(std::uint64_t a,std::uint64_t b) {
    if(b>std::numeric_limits<std::uint64_t>::max()-a) throw std::overflow_error("Spill coordinator capacity overflow");
    return a+b;
}
}
struct SpillCoordinator::State {
    struct Request {
        std::uint64_t bytes=0;
        std::stop_token stop;
        SpillReclaimResult result;
        bool done=false,synchronous=false;
    };
    struct RetirementSlot {const Tile* tile=nullptr;bool occupied=false;};
    struct Ticket {
        std::shared_ptr<State> state;std::size_t index;bool transferred=false;
        ~Ticket() {
            if(!transferred) {std::lock_guard lock(state->mutex);state->slots[index].occupied=false;--state->outstanding;state->changed.notify_all();}
        }
        void retire(const Tile* tile) noexcept {
            auto endpoint=std::move(state);
            std::lock_guard lock(endpoint->mutex);
            endpoint->slots[index].tile=tile;transferred=true;++endpoint->ready;endpoint->changed.notify_all();
        }
    };
    std::shared_ptr<MemoryAdmission> memory;
    MemoryAdmission::Reservation charge;
    SpillCoordinatorLimits limits;
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::weak_ptr<const Tile>> tiles;
    std::vector<std::shared_ptr<Request>> requests;
    std::vector<std::function<void()>> retirements;
    std::vector<RetirementSlot> slots;
    std::size_t outstanding=0,ready=0,issuedRequests=0;
    bool closed=false, active=false, exited=false;
    std::stop_source stopping;
    std::thread worker;
    static std::uint64_t capacity(SpillCoordinatorLimits limits) {
        if(!limits.maxRegisteredTiles || !limits.maxPendingRequests || !limits.maxPendingRetirements || limits.maxPendingRequests==std::numeric_limits<std::size_t>::max())
            throw std::invalid_argument("Spill coordinator capacities must be positive");
        auto bytes=std::uint64_t(sizeof(State)+128);
        for(const auto [count,size]:{std::pair{limits.maxRegisteredTiles,sizeof(std::weak_ptr<const Tile>)+sizeof(RetirementSlot)+sizeof(Ticket)+192},
                std::pair{limits.maxPendingRequests+1,sizeof(std::shared_ptr<Request>)+sizeof(Request)+128},
                std::pair{limits.maxPendingRetirements,sizeof(std::function<void()>)+128}}) {
            if(count>std::numeric_limits<std::uint64_t>::max()/size) throw std::overflow_error("Spill coordinator capacity overflow");
            bytes=add(bytes,count*size);
        }
        return bytes;
    }
    State(std::shared_ptr<MemoryAdmission> admission,SpillCoordinatorLimits bounds)
        :memory(std::move(admission)),charge(memory->require(capacity(bounds))),limits(bounds) {
        tiles.reserve(limits.maxRegisteredTiles);requests.reserve(limits.maxPendingRequests);
        retirements.reserve(limits.maxPendingRetirements);charge.commit();
        slots.resize(limits.maxRegisteredTiles);
    }
    SpillReclaimResult perform(const Request& request) {
        SpillReclaimResult result;result.accepted=true;
        std::stop_source cancel;
        std::stop_callback external(request.stop,[&] {cancel.request_stop();});
        std::stop_callback shutdown(stopping.get_token(),[&] {cancel.request_stop();});
        const auto before=memory->snapshot().committedCpu;
        std::size_t index=0;
        while(result.releasedTrackedBytes<request.bytes) {
            if(cancel.stop_requested()) {result.canceled=true;break;}
            std::weak_ptr<const Tile> candidate;
            {std::lock_guard lock(mutex);if(index>=tiles.size()) break;candidate=tiles[index++];}
            auto tile=candidate.lock();if(!tile) continue;
            ++result.examined;
            try {
                const auto status=tile->spillStatus();
                if(status.resident && tile->trySpill(cancel.get_token()))
                    result.droppedCachedBytes=add(result.droppedCachedBytes,status.residentCharge);
            } catch(...) {++result.failed;if(!result.firstFailure) result.firstFailure=std::current_exception(); if(cancel.stop_requested()) {result.canceled=true;break;}}
            const auto current=memory->snapshot().committedCpu;
            result.releasedTrackedBytes=before>current?before-current:0;
        }
        const auto after=memory->snapshot().committedCpu;
        result.releasedTrackedBytes=before>after?before-after:0;
        return result;
    }
    void run() noexcept {
        for(;;) {
            std::shared_ptr<Request> request;
            std::function<void()> cleanup;
            const Tile* retiredTile=nullptr;std::size_t retiredIndex=0;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock,[&] {return (closed && !outstanding) || ready || !requests.empty() || !retirements.empty();});
                if(ready) {
                    for(std::size_t i=0;i<slots.size();++i) if(slots[i].tile) {retiredIndex=i;retiredTile=slots[i].tile;slots[i].tile=nullptr;--ready;break;}
                }
                else if(!retirements.empty()) {cleanup=std::move(retirements.front());retirements.erase(retirements.begin());}
                else if(!requests.empty()) {request=std::move(requests.front());requests.erase(requests.begin());}
                else if(closed) break;
                active=true;
            }
            if(retiredTile) {
                delete retiredTile;
                std::lock_guard lock(mutex);slots[retiredIndex].occupied=false;--outstanding;
            }
            if(cleanup) {try {cleanup();} catch(...) {} cleanup={};}
            if(request) {
                auto result=perform(*request);
                std::lock_guard lock(mutex);request->result=result;request->done=true;
                const bool synchronous=request->synchronous;request.reset();
                if(!synchronous) --issuedRequests;
            }
            {std::lock_guard lock(mutex);active=false;}
            changed.notify_all();
        }
        {std::lock_guard lock(mutex);exited=true;}
        changed.notify_all();
    }
};
SpillCoordinator::SpillCoordinator(std::shared_ptr<MemoryAdmission> memory,SpillCoordinatorLimits limits) {
    if(!memory) throw std::invalid_argument("Spill coordinator requires memory admission");
    state_=std::make_shared<State>(std::move(memory),limits);
    state_->worker=std::thread([state=state_] {state->run();});
}
SpillCoordinator::~SpillCoordinator() {shutdown();}
bool SpillCoordinator::registerTile(const TilePtr& tile) {
    if(!tile || !tile->spillable()) return false;
    auto& state=*state_;std::lock_guard lock(state.mutex);
    if(state.closed) return false;
    for(const auto& weak:state.tiles) if(!weak.owner_before(tile) && !tile.owner_before(weak)) return true;
    if(state.tiles.size()==state.limits.maxRegisteredTiles)
        std::erase_if(state.tiles,[](const auto& weak) {return weak.expired();});
    if(state.tiles.size()==state.limits.maxRegisteredTiles) return false;
    state.tiles.emplace_back(tile);return true;
}
std::function<void(const Tile*)> SpillCoordinator::makeTileDeleter() {
    auto state=state_;std::lock_guard lock(state->mutex);
    if(state->closed) throw std::runtime_error("Spill coordinator is closed");
    for(std::size_t index=0;index<state->slots.size();++index) if(!state->slots[index].occupied) {
        auto ticket=std::make_shared<State::Ticket>();ticket->state=state;ticket->index=index;
        // Build the callable before assigning the slot, so an allocation
        // failure cannot consume retirement capacity.
        ticket->transferred=true;
        std::function<void(const Tile*)> deleter=[ticket](const Tile* tile) noexcept {ticket->retire(tile);};
        state->slots[index].occupied=true;++state->outstanding;ticket->transferred=false;
        return deleter;
    }
    throw std::runtime_error("Spill tile retirement capacity exhausted");
}
SpillReclaimResult SpillCoordinator::reclaim(std::uint64_t bytes,std::stop_token stop) {
    auto& state=*state_;std::unique_lock lock(state.mutex);
    if(state.closed || state.requests.size()>=state.limits.maxPendingRequests ||
       state.issuedRequests>=state.limits.maxPendingRequests+1) return {};
    auto request=std::make_shared<State::Request>();request->bytes=bytes;request->stop=stop;request->synchronous=true;
    state.requests.push_back(request);++state.issuedRequests;state.changed.notify_all();
    state.changed.wait(lock,[&] {return request->done;});
    const auto result=request->result;
    request.reset();--state.issuedRequests;
    return result;
}
bool SpillCoordinator::requestReclaim(std::uint64_t bytes) {
    auto& state=*state_;std::lock_guard lock(state.mutex);
    if(state.closed || state.requests.size()>=state.limits.maxPendingRequests ||
       state.issuedRequests>=state.limits.maxPendingRequests+1) return false;
    auto request=std::make_shared<State::Request>();request->bytes=bytes;
    state.requests.push_back(std::move(request));++state.issuedRequests;state.changed.notify_all();return true;
}
bool SpillCoordinator::retire(const std::function<void()>& cleanup) {
    if(!cleanup) return false;
    auto& state=*state_;std::lock_guard lock(state.mutex);
    if(state.closed || state.retirements.size()>=state.limits.maxPendingRetirements) return false;
    state.retirements.push_back(cleanup);state.changed.notify_all();return true;
}
void SpillCoordinator::waitIdle() {
    auto& state=*state_;std::unique_lock lock(state.mutex);
    state.changed.wait(lock,[&] {return !state.active && state.requests.empty() && state.retirements.empty() && !state.ready;});
}
std::function<void()> SpillCoordinator::retirementDrain() const {
    return [state=state_] {
        std::unique_lock lock(state->mutex);
        state->changed.wait(lock,[&] {return !state->outstanding && !state->active && (!state->closed || state->exited);});
    };
}
void SpillCoordinator::shutdown() {
    auto& state=*state_;
    {std::lock_guard lock(state.mutex);state.closed=true;state.stopping.request_stop();}
    state.changed.notify_all();
    waitIdle();
    // Issued Tile deleters retain the state. The worker exits only after they
    // have all released their slots, allowing snapshots to outlive the wrapper.
    bool outstanding;{std::lock_guard lock(state.mutex);outstanding=state.outstanding!=0;}
    if(state.worker.joinable()) {if(outstanding) state.worker.detach();else state.worker.join();}
}
}
