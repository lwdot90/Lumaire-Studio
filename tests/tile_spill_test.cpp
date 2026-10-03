#include "core/tiles.h"
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace compositor;
using namespace compositor::engine;
using namespace compositor::io;
namespace fs=std::filesystem;
namespace {
void expect(bool pass,const char* message) {if(!pass) throw std::runtime_error(message);}
template<class F> void rejects(F action) {
    try {action();} catch(const std::exception&) {return;}
    throw std::runtime_error("Expected tile backing failure");
}
struct Fixture {
    fs::path directory;
    std::shared_ptr<std::atomic<std::uint64_t>> available=std::make_shared<std::atomic<std::uint64_t>>(16*1024*1024);
    std::shared_ptr<MemoryAdmission> memory=std::make_shared<MemoryAdmission>(16*1024*1024,0,[value=available]{return MemorySample{value->load(),0};});
    std::shared_ptr<SpillStore> store;
    Fixture(const fs::path& root,SpillFault fault={}) {
        auto pattern=(root/"live-tile-test-XXXXXX").string();
        const auto made=::mkdtemp(pattern.data()); expect(made,"Create private tile fixture");directory=made;
        store=std::make_shared<SpillStore>(directory,SpillLimits{1024*1024,65536,0,64,1,128},std::move(fault));
    }
    ~Fixture() {store.reset(); std::error_code error;fs::remove_all(directory,error);}
};
std::vector<Pixel> pixels(int width=17,int height=9) {
    std::vector<Pixel> result(static_cast<std::size_t>(width)*static_cast<std::size_t>(height));
    for(std::size_t i=0;i<result.size();++i) result[i]={float(i%7)/8,float(i%5)/8,float(i%3)/4,1};
    return result;
}
void roundTrip(const fs::path& root) {
    Fixture f(root);TileStore store(65536,f.memory,f.store),legacy(65536);
    const auto values=pixels();auto tile=store.create(17,9,values),reference=legacy.create(17,9,values);
    const auto bytes=reference->canonicalBytes();
    const auto version=tile->version();
    const auto logical=tile->retainedBytes();
    expect(tile->spillable() && !tile->uniform() && tile->samePixels(*reference),"Disk tile changed canonical identity");
    const auto budget=store.usedBytes(),resident=tile->spillStatus().residentCharge;
    const auto charged=f.memory->snapshot().committedCpu;
    expect(tile->spill(),"First spill must retire cache");
    expect(!tile->spillStatus().resident && tile->spillStatus().spilled,"Disk-only state");
    expect(f.memory->snapshot().committedCpu==charged-resident,"Resident charge was not released");
    expect(store.usedBytes()==budget && tile->version()==version && tile->retainedBytes()==logical && !tile->uniform(),"Spill changed immutable metadata");
    expect(tile->read().canonicalBytes()==bytes && tile->samePixels(*reference),"Rehydration changed exact bytes");
    expect(tile->spill(),"Reloaded cache must retire");expect(!tile->spill(),"Repeat disk-only spill must be a no-op");
    auto recreated=store.fromCanonical(17,9,bytes);expect(recreated->samePixels(*tile),"Canonical import changed representation");
}
void pinnedLifetime(const fs::path& root) {
    Fixture f(root);Tile::ReadLease lease;
    std::uint64_t resident=0;
    {
        TileStore store(65536,f.memory,f.store);auto tile=store.create(17,9,pixels());
        lease=tile->read();resident=tile->spillStatus().residentCharge;const auto original=lease.pixel(16,8);
        tile->spill();expect(f.memory->snapshot().committedCpu>=resident,"Pinned lease dropped resident admission");
        tile.reset();expect(store.usedBytes()==0,"Lease retained immutable Tile metadata unnecessarily");
        expect(lease.pixel(16,8)==original && f.memory->snapshot().committedCpu==resident,"Owner destruction invalidated bytes or leaked metadata");
    }
    f.store.reset();expect(lease.pixel(0,0)==pack(pixels()[0]),"Store destruction invalidated lease");
    lease={};expect(f.memory->snapshot().committedCpu==0,"Final lease did not release admission");
}
void constantAndLegacy(const fs::path& root) {
    Fixture f(root);TileStore disk(65536,f.memory,f.store),legacy(65536,f.memory);
    auto constant=disk.constant(256,256,{.25f,.5f,1,1});expect(constant->uniform() && !constant->spillable() && !constant->spill(),"Constant must remain inline");
    auto tile=legacy.create(17,9,pixels());auto lease=tile->read();const auto charge=legacy.usedBytes();tile.reset();
    expect(legacy.usedBytes()==charge && lease.pixel(16,8)==pack(pixels()[152]),"Legacy lease failed to pin storage");
    lease={};expect(legacy.usedBytes()==0,"Legacy payload budget did not retire");
    auto inlineLease=constant->read();constant.reset();expect(inlineLease.pixel(255,255)==pack(Pixel{.25f,.5f,1,1}),"Inline constant lease changed bytes");
}
void failures(const fs::path& root) {
    Fixture f(root,[](SpillIoStage stage,std::uint64_t){return stage==SpillIoStage::Write ? ENOSPC : 0;});
    TileStore store(65536,f.memory,f.store);auto tile=store.create(17,9,pixels());const auto bytes=tile->canonicalBytes();
    rejects([&]{tile->spill();});expect(tile->spillStatus().resident && !tile->spillStatus().spilled && tile->canonicalBytes()==bytes,"Failed spill damaged pixels");
    Fixture other(root);TileStore working(65536,other.memory,other.store);auto reload=working.create(17,9,pixels());reload->spill();
    const auto metadata=other.memory->snapshot().committedCpu;other.available->store(0);
    rejects([&]{reload->read();});expect(!reload->spillStatus().resident && other.memory->snapshot().committedCpu==metadata,"Denied rehydration leaked or published bytes");
    other.available->store(16*1024*1024);expect(reload->canonicalBytes()==bytes,"Denied rehydration lost disk backing");
}
void cancellation(const fs::path& root) {
    Fixture f(root);TileStore store(65536,f.memory,f.store);auto tile=store.create(17,9,pixels());tile->spill();
    std::stop_source source;source.request_stop();rejects([&]{tile->read(source.get_token());});
    rejects([&]{tile->spill(source.get_token());});expect(!tile->spillStatus().resident,"Cancelled read published cache");
    auto lease=tile->read();std::vector<std::uint8_t> destination(17*9*8);rejects([&]{lease.copyCanonicalBytes(destination,source.get_token());});
    auto constant=store.constant(2,2,{0,0,0,0});rejects([&]{constant->read(source.get_token());});
}
void busyPressure(const fs::path& root) {
    std::atomic<bool> entered=false,release=false;
    Fixture f(root,[&](SpillIoStage stage,std::uint64_t) {
        if(stage==SpillIoStage::Read) {
            entered.store(true);entered.notify_all();
            release.wait(false);
        }
        return 0;
    });
    TileStore store(65536,f.memory,f.store);auto tile=store.create(17,9,pixels());tile->spill();
    std::exception_ptr error;
    std::thread reader([&]{try {tile->read();} catch(...) {error=std::current_exception();}});
    entered.wait(false);
    const bool reclaimed=tile->trySpill();
    release.store(true);release.notify_all();reader.join();
    if(error) std::rethrow_exception(error);
    expect(!reclaimed,"Pressure eviction waited for or interrupted a rehydration transition");
    expect(tile->trySpill() && !tile->spillStatus().resident,"Idle nonwaiting spill failed");
}
void callbacksAndMetadata(const fs::path& root) {
    Fixture f(root);TileStore store(65536,f.memory,f.store);unsigned pressure=0,observed=0;
    store.setSpillCallbacks([&](std::size_t peak){expect(peak>0,"Missing allocation peak");++pressure;},[&](const TilePtr& tile){expect(bool(tile),"Observer sees unpublished tile");++observed;});
    auto tile=store.create(17,9,pixels());expect(pressure==1 && observed==1,"Construction callbacks did not run");
    const auto budget=store.usedBytes();expect(budget<tile->retainedBytes(),"Disk budget retained logical dense payload");
    TileStore tooSmall(1,f.memory,f.store);const auto charge=f.memory->snapshot().committedCpu;
    rejects([&]{tooSmall.create(17,9,pixels());});expect(tooSmall.usedBytes()==0 && f.memory->snapshot().committedCpu==charge,"Failed metadata admission leaked charge");
    rejects([&]{TileStore invalid(65536,{},f.store);});
}
}
int main(int argc,char** argv) {
    try {
        if(argc!=2) throw std::invalid_argument("Expected disk-backed fixture parent");
        roundTrip(argv[1]);pinnedLifetime(argv[1]);constantAndLegacy(argv[1]);failures(argv[1]);cancellation(argv[1]);busyPressure(argv[1]);callbacksAndMetadata(argv[1]);
        std::cout<<"Seven live tile spill groups passed\n";return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
