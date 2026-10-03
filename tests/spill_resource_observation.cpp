#include "core/document.h"
#include "core/resources.h"
#include "core/spill_coordinator.h"
#include "io/spill_directory.h"
#include "io/spill_store.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>
using namespace compositor;
using namespace compositor::engine;
using namespace compositor::io;
namespace {
constexpr std::uint64_t mib=1024*1024,payloadLimit=8*mib;
void require(bool okay,const char* message) {if(!okay) throw std::runtime_error(message);}
struct Fixture {
    std::filesystem::path path;
    explicit Fixture(const std::filesystem::path& parent) {
        const auto absolute=std::filesystem::absolute(parent);
        auto pattern=(absolute/"spill-observation-XXXXXX").string();
        const auto created=::mkdtemp(pattern.data());require(created,"Create private disk observation fixture");path=created;
    }
    ~Fixture() {std::error_code error;std::filesystem::remove_all(path,error);}
};
Pixel expected(int index,int x,int y) {
    return {static_cast<float>(index%8)/8,static_cast<float>((x+y)%4)/4,static_cast<float>(index%4)/4,1};
}
struct Observation {
    std::uint64_t maxRss=0,minAvailable=UINT64_MAX,maxCommittedCpu=0,maxPendingCpu=0,maxCommittedGpu=0,maxPendingGpu=0,maxDiskBytes=0,maxDiskFileBytes=0;
    std::size_t samples=0;
    void phase(const char* name,const std::shared_ptr<MemoryAdmission>& admission,const SpillStore& disk) {
        const auto system=MemorySample::read();require(system.resident && system.available,"Read real Linux memory probe");
        const auto memory=admission->snapshot();const auto storage=disk.stats();
        std::uint64_t diskFileBytes=0;
        for(const auto& entry:std::filesystem::directory_iterator(disk.sessionDirectory()))
            if(entry.is_regular_file()) diskFileBytes+=entry.file_size();
        maxRss=std::max(maxRss,*system.resident);minAvailable=std::min(minAvailable,*system.available);
        maxCommittedCpu=std::max(maxCommittedCpu,memory.committedCpu);maxPendingCpu=std::max(maxPendingCpu,memory.pendingCpu);
        maxCommittedGpu=std::max(maxCommittedGpu,memory.committedGpu);maxPendingGpu=std::max(maxPendingGpu,memory.pendingGpu);
        maxDiskBytes=std::max(maxDiskBytes,storage.bytes);maxDiskFileBytes=std::max(maxDiskFileBytes,diskFileBytes);++samples;
        std::cout<<"{\"phase\":\""<<name<<"\",\"rss_bytes\":"<<*system.resident
            <<",\"mem_available_bytes\":"<<*system.available<<",\"committed_cpu_bytes\":"<<memory.committedCpu
            <<",\"pending_cpu_bytes\":"<<memory.pendingCpu<<",\"committed_gpu_bytes\":"<<memory.committedGpu
            <<",\"pending_gpu_bytes\":"<<memory.pendingGpu<<",\"spill_charged_bytes\":"<<storage.bytes
            <<",\"disk_file_bytes\":"<<diskFileBytes<<",\"spill_entries\":"<<storage.entries<<",\"active_io\":"<<storage.activeIo
            <<",\"cleanup_failures\":"<<storage.cleanupFailures<<"}\n";
    }
};
void verify(const DocumentPtr& document) {
    const auto raster=document->singleLayer().raster;
    require(raster->tiles.size()==16,"Snapshot retains all dense tiles");
    for(const auto& [coordinate,tile]:raster->tiles) {
        const int index=static_cast<int>(coordinate.y*4+coordinate.x);
        auto lease=tile->read();
        for(int y=0;y<256;++y) for(int x=0;x<256;++x)
            require(lease.pixel(x,y)==pack(expected(index,x,y)),"Canonical pixel changed across spill/rehydration");
    }
}
void run(const std::filesystem::path& parent) {
    Fixture fixture(parent);
    const auto directory=prepareSpillDirectory(SpillDirectoryOptions{fixture.path,{}});
    auto disk=std::make_shared<SpillStore>(directory,SpillLimits{16*mib,512*1024,64*mib,32,1,64*1024});
    auto profile=ResourceLimits::detect();
    profile.canonicalTiles=16*mib;profile.cpuMips=mib;profile.thumbnailMips=mib;profile.computeWorkers=1;
    // Retain the actual machine application's admission envelope and required
    // desktop headroom. This observation does not manufacture memory pressure.
    require(profile.systemHeadroom>=512*mib,"Desktop headroom is at least 512 MiB");
    auto runtime=std::make_shared<RuntimeResources>(profile,MemorySample::read,disk);
    auto admission=runtime->memory;
    std::cout<<"{\"observation\":\"bounded_actual_spill\",\"automatic_pressure_qualification\":false,\"dense_payload_limit_bytes\":"
        <<payloadLimit<<",\"application_limit_bytes\":"<<profile.applicationMemory<<",\"system_headroom_bytes\":"<<profile.systemHeadroom<<"}\n";
    Observation observation;observation.phase("runtime_ready",admission,*disk);
    TileMap map;
    auto inputCharge=std::optional<MemoryAdmission::Reservation>(admission->require(256*256*sizeof(Pixel)));
    std::vector<Pixel> input(256*256);inputCharge->commit();
    for(int index=0;index<16;++index) {
        for(int y=0;y<256;++y) for(int x=0;x<256;++x) input[static_cast<std::size_t>(y*256+x)]=expected(index,x,y);
        map.emplace(TileCoord{index%4,index/4},runtime->tiles->create(256,256,input));
        observation.phase("dense_tile_added",admission,*disk);
    }
    std::vector<Pixel>().swap(input);inputCharge.reset();
    auto raster=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,1024,1024},PackedPixel{},std::move(map));
    auto original=std::make_shared<const DocumentSnapshot>(Id::generate(),Id::generate(),1024,1024,72,raster);
    auto history=std::make_unique<DocumentHistory>(original);
    {
        auto edit=history->begin();edit.replace({0,0},runtime->tiles->constant(256,256,{0,0,0,1}));
        require(history->commit(edit,"Observation replacement"),"Publish constant replacement history");
    }
    auto captured=original;auto held=original->singleLayer().raster->tiles.at({0,0})->read();
    observation.phase("snapshot_history_lease_held",admission,*disk);
    const auto reclaimed=runtime->spillCoordinator->reclaim(payloadLimit);
    require(reclaimed.accepted && !reclaimed.failed && reclaimed.droppedCachedBytes>=payloadLimit,"Explicit bounded spill completed");
    observation.phase("explicit_spill_with_lease",admission,*disk);
    require(held.pixel(5,7)==pack(expected(0,5,7)),"Outstanding lease retained exact bytes");
    held={};observation.phase("held_lease_released",admission,*disk);
    require(history->undo(),"Undo after disk eviction");verify(history->current());verify(captured);
    require(history->redo(),"Redo after verified reload");
    require(history->current()->singleLayer().raster->tiles.at({0,0})->pixel(0,0)==pack(Pixel{0,0,0,1}),"Redo constant replacement exact");
    observation.phase("verified_undo_redo_reload",admission,*disk);
    auto drain=runtime->spillCoordinator->retirementDrain();
    history.reset();captured.reset();original.reset();raster.reset();
    drain();observation.phase("tiles_retired",admission,*disk);
    require(!runtime->tiles->usedBytes() && !disk->stats().entries,"Final retirement drained tile and disk ownership");
    runtime.reset();drain();observation.phase("runtime_closed",admission,*disk);
    std::cout<<"{\"result\":\"passed\",\"exact_pixels_verified\":true,\"samples\":"<<observation.samples
        <<",\"phase_max_rss_bytes\":"<<observation.maxRss<<",\"phase_min_mem_available_bytes\":"<<observation.minAvailable
        <<",\"phase_max_committed_cpu_bytes\":"<<observation.maxCommittedCpu<<",\"phase_max_pending_cpu_bytes\":"<<observation.maxPendingCpu
        <<",\"phase_max_committed_gpu_bytes\":"<<observation.maxCommittedGpu<<",\"phase_max_pending_gpu_bytes\":"<<observation.maxPendingGpu
        <<",\"phase_max_disk_file_bytes\":"<<observation.maxDiskFileBytes<<",\"phase_max_spill_charged_bytes\":"<<observation.maxDiskBytes
        <<",\"lp8_qualified\":false,\"mechanism\":\"explicit_coordinator_reclaim\"}\n";
}
}
int main(int argc,char** argv) {
    try {require(argc==2,"Pass an existing verified disk-backed build directory");run(argv[1]);return 0;}
    catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
