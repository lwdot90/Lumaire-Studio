#include "core/resources.h"
#include "core/spill_coordinator.h"
#include "core/document.h"
#include "io/spill_store.h"
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace compositor;
using namespace compositor::engine;
using namespace compositor::io;
namespace {
constexpr std::uint64_t envelope=64*1024*1024;
void check(bool okay,const char* message) {if(!okay) throw std::runtime_error(message);}
struct Fixture {
    std::filesystem::path path;
    explicit Fixture(const std::filesystem::path& parent) {
        auto pattern=(parent/"runtime-spill-XXXXXX").string();
        const auto created=::mkdtemp(pattern.data());check(created,"Create runtime disk fixture");path=created;
    }
    ~Fixture() {std::error_code error;std::filesystem::remove_all(path,error);}
};
struct Environment {
    Fixture fixture;
    std::shared_ptr<std::atomic<std::uint64_t>> rss=std::make_shared<std::atomic<std::uint64_t>>(0);
    std::shared_ptr<SpillStore> disk;
    std::shared_ptr<RuntimeResources> runtime;
    explicit Environment(const std::filesystem::path& parent,SpillFault fault={}):fixture(parent) {
        disk=std::make_shared<SpillStore>(fixture.path,SpillLimits{1024*1024,65536,0,32,1,1024},std::move(fault));
        auto limits=ResourceLimits::forMachine(8ull*1024*1024*1024,1);
        limits.applicationMemory=envelope;limits.systemHeadroom=0;limits.canonicalTiles=2*1024*1024;
        runtime=std::make_shared<RuntimeResources>(limits,[sample=rss] {return MemorySample{300*1024*1024,sample->load()};},disk);
    }
    void pressure(std::uint64_t room) {
        const auto tracked=runtime->memory->snapshot().committedCpu;
        check(tracked+room<envelope,"Bookkeeping fits fake memory envelope");rss->store(envelope-tracked-room);
    }
};
std::vector<Pixel> pixels(bool alternate=false) {
    std::vector<Pixel> result;result.reserve(256*32);
    for(int i=0;i<256*32;++i) result.push_back(((i%2)==0)!=alternate?Pixel{1,.25f,0,1}:Pixel{0,.5f,1,1});
    return result;
}
void pressureAndHistory(const std::filesystem::path& parent) {
    Environment env(parent);auto firstPixels=pixels(),secondPixels=pixels(true);
    auto first=env.runtime->tiles->create(256,32,firstPixels);
    TileMap map;map.emplace(TileCoord{0,0},first);
    auto raster=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,256,32},PackedPixel{},std::move(map));
    auto initial=std::make_shared<const DocumentSnapshot>(Id::generate(),Id::generate(),256,32,72,raster);
    DocumentHistory history(initial);
    auto second=env.runtime->tiles->create(256,32,secondPixels);
    auto edit=history.begin();edit.replace({0,0},second);check(history.commit(edit,"Change pixels"),"Publish history fixture");
    env.pressure(100000);
    auto third=env.runtime->tiles->create(256,32,firstPixels);
    check(!first->spillStatus().resident || !second->spillStatus().resident,"Allocation admission automatically evicts live history tile");
    check(env.disk->stats().entries>0,"Pressure eviction publishes disk backing");
    env.rss->store(0);
    check(history.undo(),"Undo survives pressure eviction");
    check(history.current()->singleLayer().raster->pixel(0,0)==firstPixels[0],"Undo restored exact old sample");
    check(history.current()->singleLayer().raster->pixel(1,0)==firstPixels[1],"Undo restored second exact sample");
    check(history.redo() && history.current()->singleLayer().raster->pixel(0,0)==secondPixels[0],"Redo reloads immutable replacement");
    check(third->pixel(0,0)==pack(firstPixels[0]),"New tile preserved through pressure retry");
}
void failurePreservation(const std::filesystem::path& parent) {
    Environment env(parent,[](auto stage,auto) {return stage==SpillIoStage::Write?EIO:0;});
    const auto values=pixels();auto prior=env.runtime->tiles->create(256,32,values);
    env.pressure(1024);bool rejected=false;
    try {auto next=env.runtime->tiles->create(256,32,values);}
    catch(const SpillError& error) {check(error.code()==SpillErrorCode::Io,"Pressure failure preserves injected disk cause");rejected=true;}
    catch(const std::length_error&) {rejected=true;}
    check(rejected,"Failed pressure spill rejects allocation");
    check(prior->spillStatus().resident && !prior->spillStatus().spilled,"Failed spill preserves prior resident publication");
    check(!env.disk->stats().entries,"Failed write publishes no handle");
    env.rss->store(0);check(prior->pixel(1,0)==pack(values[1]),"Prior exact bytes survive failed admission");
    check(env.runtime->memory->snapshot().pendingCpu==0,"Failed allocation releases pending charges");
}
void runtimeLifetime(const std::filesystem::path& parent) {
    Environment env(parent);const auto values=pixels();auto tile=env.runtime->tiles->create(256,32,values);
    auto lease=tile->read();check(env.runtime->spillCoordinator->reclaim(1).droppedCachedBytes>0,"Spill held runtime tile");
    auto memory=env.runtime->memory;auto tiles=env.runtime->tiles;auto drain=env.runtime->spillCoordinator->retirementDrain();
    env.runtime.reset();tile.reset();drain();
    check(!tiles->usedBytes() && !env.disk->stats().entries,"Runtime teardown retires escaped Tile on IO worker");
    check(lease.pixel(1,0)==pack(values[1]),"Lease survives Runtime and Tile destruction");
    const auto before=memory->snapshot().committedCpu;lease={};
    check(memory->snapshot().committedCpu<before,"Escaped resident lease releases independent charge");
}
}
int main(int argc,char** argv) {
    try {check(argc==2,"Pass disk-backed fixture directory");pressureAndHistory(argv[1]);failurePreservation(argv[1]);runtimeLifetime(argv[1]);std::cout<<"3 runtime spill groups passed\n";return 0;}
    catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
