#include "core/spill_coordinator.h"
#include "io/spill_store.h"
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
using namespace compositor;
using namespace compositor::engine;
using namespace compositor::io;
namespace {
void check(bool okay,const char* message) {if(!okay) throw std::runtime_error(message);}
struct Fixture {
    std::filesystem::path path;
    explicit Fixture(const std::filesystem::path& parent) {
        auto pattern=(parent/"spill-coordinator-XXXXXX").string();
        const auto created=::mkdtemp(pattern.data());check(created,"Create disk fixture");path=created;
    }
    ~Fixture() {std::error_code error;std::filesystem::remove_all(path,error);}
};
std::shared_ptr<MemoryAdmission> memory() {
    return std::make_shared<MemoryAdmission>(16*1024*1024,0,[] {return MemorySample{32*1024*1024,0};});
}
std::shared_ptr<SpillStore> store(const Fixture& fixture,SpillFault fault={}) {
    return std::make_shared<SpillStore>(fixture.path,SpillLimits{65536,4096,0,16,1,32},std::move(fault));
}
TilePtr dense(TileStore& tiles) {
    const std::array<Pixel,2> pixels{Pixel{1,0,0,1},Pixel{0,1,0,1}};
    return tiles.create(2,1,pixels);
}
void reclamation(const std::filesystem::path& parent) {
    Fixture fixture(parent);auto admission=memory();auto disk=store(fixture);
    SpillCoordinator coordinator(admission,{4,2,2});TileStore tiles(65536,admission,disk);
    tiles.setTileDeleterFactory([&] {return coordinator.makeTileDeleter();});
    auto tile=dense(tiles);check(coordinator.registerTile(tile),"Register dense tile");
    check(coordinator.registerTile(tile),"Duplicate registration is idempotent");
    auto lease=tile->read();const auto before=admission->snapshot().committedCpu;
    const auto result=coordinator.reclaim(1);
    check(result.accepted && !result.failed && result.droppedCachedBytes && !result.releasedTrackedBytes,"Leased resident charge remains after eviction");
    check(admission->snapshot().committedCpu==before,"Eviction retained lease charge");
    check(lease.pixel(0,0)==pack(Pixel{1,0,0,1}),"Lease preserves bytes");
    lease={};check(admission->snapshot().committedCpu<before,"Last lease releases resident charge");
    check(tile->pixel(1,0)==pack(Pixel{0,1,0,1}),"Verified rehydration preserves bytes");
    std::stop_source canceled;canceled.request_stop();
    check(coordinator.reclaim(1,canceled.get_token()).canceled,"Canceled request is reported");
    tile.reset();coordinator.waitIdle();check(tiles.usedBytes()==0,"Worker retirement releases tile budget");
    coordinator.shutdown();coordinator.shutdown();
    check(!coordinator.requestReclaim(1),"Closed queue rejects requests");
}
void pinnedThenFreeable(const std::filesystem::path& parent) {
    Fixture fixture(parent);auto admission=memory();auto disk=store(fixture);
    SpillCoordinator coordinator(admission,{4,2,2});TileStore tiles(65536,admission,disk);
    auto pinned=dense(tiles),freeable=dense(tiles);
    check(coordinator.registerTile(pinned) && coordinator.registerTile(freeable),"Register ordered eviction candidates");
    auto lease=pinned->read();const auto freeCharge=freeable->spillStatus().residentCharge;
    const auto result=coordinator.reclaim(freeCharge);
    check(result.examined==2 && result.releasedTrackedBytes>=freeCharge,"Reclamation continues past leased cache removal");
    check(!pinned->spillStatus().resident && !freeable->spillStatus().resident,"Both cached copies were removed");
    check(lease.pixel(0,0)==pack(Pixel{1,0,0,1}),"Pinned first tile remains readable");
}
void failures(const std::filesystem::path& parent) {
    Fixture fixture(parent);auto admission=memory();auto disk=store(fixture,[](auto stage,auto) {return stage==SpillIoStage::Write?EIO:0;});
    SpillCoordinator coordinator(admission,{2,1,1});TileStore tiles(65536,admission,disk);auto tile=dense(tiles);
    check(coordinator.registerTile(tile),"Register failure fixture");
    const auto result=coordinator.reclaim(1);
    check(result.failed==1 && !result.droppedCachedBytes && tile->spillStatus().resident,"Failed spill preserves resident tile");
}
void boundedQueues(const std::filesystem::path& parent) {
    Fixture fixture(parent);auto admission=memory();SpillCoordinator coordinator(admission,{1,1,1});
    TileStore tiles(65536,admission,store(fixture));auto first=dense(tiles),second=dense(tiles);
    check(coordinator.registerTile(first) && !coordinator.registerTile(second),"Weak registry is bounded");
    first.reset();check(coordinator.registerTile(second),"Expired registry entries are reused");
    std::mutex mutex;std::condition_variable changed;bool entered=false,release=false;
    check(coordinator.retire([&] {std::unique_lock lock(mutex);entered=true;changed.notify_all();changed.wait(lock,[&] {return release;});}),"Enqueue cleanup barrier");
    {std::unique_lock lock(mutex);changed.wait(lock,[&] {return entered;});}
    check(coordinator.requestReclaim(1) && !coordinator.requestReclaim(1),"Pending IO queue is bounded");
    check(coordinator.retire([] {}) && !coordinator.retire([] {}),"Cleanup queue is bounded");
    {std::lock_guard lock(mutex);release=true;}changed.notify_all();coordinator.waitIdle();
}
void abandonedAndEscaped(const std::filesystem::path& parent) {
    Fixture fixture(parent);auto admission=memory();std::atomic<bool> removed=false;std::thread::id removalThread;
    auto disk=store(fixture,[&](auto stage,auto) {if(stage==SpillIoStage::RemoveFile) {removalThread=std::this_thread::get_id();removed=true;}return 0;});
    TileStore tiles(65536,admission,disk);
    auto coordinator=std::make_unique<SpillCoordinator>(admission,SpillCoordinatorLimits{1,1,1});
    {auto unused=coordinator->makeTileDeleter();}
    tiles.setTileDeleterFactory([&] {return coordinator->makeTileDeleter();});auto tile=dense(tiles);
    bool rejected=false;try {auto extra=coordinator->makeTileDeleter();} catch(const std::runtime_error&) {rejected=true;}
    check(rejected,"Outstanding retirement tickets bounded");
    auto drain=coordinator->retirementDrain();
    // Final Tile cleanup after wrapper shutdown must still execute on the IO thread.
    const auto caller=std::this_thread::get_id();
    check(coordinator->registerTile(tile),"Register escaped Tile before spilling");
    auto leased=tile->read();
    check(coordinator->reclaim(1).droppedCachedBytes>0 && disk->stats().entries==1,"Escaped Tile has a published disk entry");
    coordinator.reset();
    tile.reset();drain();check(tiles.usedBytes()==0 && disk->stats().entries==0,"Escaped Tile drained disk cleanup");
    check(removed && removalThread!=caller,"Final file removal runs on IO worker");
    check(leased.pixel(0,0)==pack(Pixel{1,0,0,1}),"Resident lease survives worker endpoint teardown");
    leased={};
}
}
int main(int argc,char** argv) {
    try {check(argc==2,"Pass disk-backed fixture directory");reclamation(argv[1]);pinnedThenFreeable(argv[1]);failures(argv[1]);boundedQueues(argv[1]);abandonedAndEscaped(argv[1]);std::cout<<"5 spill coordinator groups passed\n";return 0;}
    catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
