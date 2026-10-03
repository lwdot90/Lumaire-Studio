#include "core/resources.h"
#include "core/tiles.h"
#include "core/spill_coordinator.h"
#include "io/spill_directory.h"
#include <cstdlib>
#include <filesystem>
#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>
#include <unistd.h>

namespace compositor {
namespace {
constexpr std::uint64_t mib=1024*1024,gib=1024*mib;
}
ResourceLimits ResourceLimits::forMachine(std::uint64_t physicalBytes,unsigned hardwareThreads) {
    // Unknown inventory uses a conservative profile. A smaller machine is not
    // automatically qualified by these settings; LP8 is the tested release goal.
    const auto ram=physicalBytes ? physicalBytes : 4*gib;
    const bool low=ram<=8*gib;
    const auto canonical=low ? std::min(ram/8,768*mib)
        : std::min({ram/4,4*gib,ram-2*gib});
    const auto bounded=std::min(canonical,static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()));
    const auto workers=hardwareThreads>2 ? hardwareThreads-2 : 1;
    return {low,static_cast<std::size_t>(bounded),
        std::size_t(low ? 8*mib : 16*mib),std::size_t(low ? 4*mib : 16*mib),
        8*mib,std::size_t(low ? 16*mib : 64*mib),std::size_t(low ? 64*mib : 256*mib),
        low ? std::min(2u,std::max(1u,hardwareThreads)) : workers,
        std::min(ram/2,low ? 2*gib : 12*gib),low ? 512*mib : 2*gib};
}
ResourceLimits ResourceLimits::detect() {
    const auto pages=::sysconf(_SC_PHYS_PAGES),size=::sysconf(_SC_PAGESIZE);
    std::uint64_t bytes=0;
    if(pages>0 && size>0 && static_cast<std::uint64_t>(pages)<=std::numeric_limits<std::uint64_t>::max()/static_cast<std::uint64_t>(size))
        bytes=static_cast<std::uint64_t>(pages)*static_cast<std::uint64_t>(size);
    return forMachine(bytes,std::thread::hardware_concurrency());
}
struct WorkScheduler::State {
    explicit State(unsigned workers,unsigned heavyWorkers):limit(workers),heavyLimit(heavyWorkers) {}
    const unsigned limit,heavyLimit;
    unsigned active=0,heavy=0;
    std::array<std::array<unsigned,2>,3> waiting{};
    mutable std::mutex mutex;
    std::condition_variable_any changed;
    bool eligible(std::size_t priority,bool memoryHeavy) const {
        if(active>=limit || (memoryHeavy && heavy>=heavyLimit)) return false;
        for(std::size_t i=0;i<priority;++i)
            if(waiting[i][0] || (heavy<heavyLimit && waiting[i][1])) return false;
        return true;
    }
};
WorkScheduler::WorkScheduler(unsigned workers,unsigned heavyWorkers):state_(std::make_shared<State>(workers,heavyWorkers)) {
    if(workers==0 || heavyWorkers==0 || heavyWorkers>workers) throw std::invalid_argument("Invalid worker admission limits");
}
void WorkScheduler::Permit::release() noexcept {
    if(!state_) return;
    {
        std::lock_guard lock(state_->mutex);
        --state_->active;
        if(heavy_) --state_->heavy;
    }
    state_->changed.notify_all();state_.reset();
}
WorkScheduler::Permit::~Permit() {release();}
WorkScheduler::Permit::Permit(Permit&& other) noexcept:state_(std::move(other.state_)),heavy_(other.heavy_) {}
WorkScheduler::Permit& WorkScheduler::Permit::operator=(Permit&& other) noexcept {
    if(this!=&other) {release();state_=std::move(other.state_);heavy_=other.heavy_;}
    return *this;
}
std::optional<WorkScheduler::Permit> WorkScheduler::acquire(Priority priority,bool heavy,std::stop_token stop,
                                                          const std::function<bool()>& cancelled) {
    const auto index=static_cast<std::size_t>(priority);
    if(index>=3) throw std::invalid_argument("Invalid worker priority");
    const auto state=state_;
    std::unique_lock lock(state->mutex);
    const auto aborted=[&]{return stop.stop_requested() || (cancelled && cancelled());};
    if(aborted()) return std::nullopt;
    auto& waiting=state->waiting[index][heavy ? 1 : 0];++waiting;
    try {
        while(!aborted() && !state->eligible(index,heavy)) {
            // Stop tokens wake immediately. Replacing a request is checked at
            // most 5 ms later, without requiring UI producers to take this lock.
            state->changed.wait_for(lock,stop,std::chrono::milliseconds(5),[&]{return aborted() || state->eligible(index,heavy);});
        }
        const bool stopped=aborted();
        --waiting;state->changed.notify_all();
        if(stopped) return std::nullopt;
        ++state->active;if(heavy) ++state->heavy;
        return Permit(state,heavy);
    } catch(...) {--waiting;state->changed.notify_all();throw;}
}
WorkScheduler::Snapshot WorkScheduler::snapshot() const {
    std::lock_guard lock(state_->mutex);
    Snapshot result{state_->active,state_->heavy,{}};
    for(std::size_t i=0;i<3;++i) result.waiting[i]=state_->waiting[i][0]+state_->waiting[i][1];
    return result;
}
std::shared_ptr<RuntimeResources> defaultRuntimeResources() {
    static auto resources=[] {
        const auto limits=ResourceLimits::detect();
        std::shared_ptr<io::SpillStore> store;
        std::string error;
        try {
            io::SpillDirectoryOptions options;
            if(const auto* state=std::getenv("XDG_STATE_HOME")) options.xdgStateDirectory=state;
            if(const auto* home=std::getenv("HOME")) options.homeDirectory=home;
            const auto directory=io::prepareSpillDirectory(options);
            const auto available=std::filesystem::space(directory).available;
            io::SpillLimits disk;
            disk.minFreeBytes=gib;disk.maxBytes=std::min(32*gib,available/4);
            disk.maxPayloadBytes=256*256*8;disk.maxEntries=65536;disk.maxIoOperations=1;
            store=std::make_shared<io::SpillStore>(directory,disk);
        } catch(const std::exception& failure) {error=failure.what();}
        auto result=std::make_shared<RuntimeResources>(limits,MemorySample::read,std::move(store));
        result->storageError=std::move(error);return result;
    }();
    return resources;
}
RuntimeResources::RuntimeResources(ResourceLimits profile,MemoryAdmission::Probe probe,std::shared_ptr<io::SpillStore> storage)
    :limits(profile),compute(profile.computeWorkers),
     memory(std::make_shared<MemoryAdmission>(profile.applicationMemory,profile.systemHeadroom,std::move(probe))),
     spill(std::move(storage)),
     spillCoordinator(spill ? std::make_shared<engine::SpillCoordinator>(memory,engine::SpillCoordinatorLimits{65536,8,64}) : nullptr),
     tiles(std::make_shared<engine::TileStore>(profile.canonicalTiles,memory,spill)) {
    if(!spillCoordinator) return;
    spill->admitMetadata(memory);
    const auto weak=std::weak_ptr<engine::SpillCoordinator>(spillCoordinator);
    tiles->setTileDeleterFactory([weak] {
        const auto coordinator=weak.lock();
        if(!coordinator) throw std::runtime_error("Tile IO coordinator is unavailable");
        return coordinator->makeTileDeleter();
    });
    tiles->setSpillCallbacks({},[weak](const engine::TilePtr& tile) {
        if(!tile->spillable()) return;
        const auto coordinator=weak.lock();
        if(!coordinator || !coordinator->registerTile(tile)) throw std::length_error("Tile spill registry is full");
    });
    memory->setReclaimer([weak](std::uint64_t bytes) {
        if(const auto coordinator=weak.lock()) {
            const auto target=bytes>std::numeric_limits<std::uint64_t>::max()/2 ? bytes : bytes*2;
            const auto result=coordinator->reclaim(target);
            if(result.firstFailure && !result.releasedTrackedBytes) std::rethrow_exception(result.firstFailure);
        }
    });
}
}
