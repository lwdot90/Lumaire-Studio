#include "rendering/cpu_worker.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <cerrno>
#include <QColorSpace>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace compositor;
using namespace compositor::engine;
namespace {
void expect(bool condition,const char* message) {if(!condition) throw std::runtime_error(message);}
template<class Predicate> void eventually(Predicate predicate,const char* message) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!predicate()) {
        if(std::chrono::steady_clock::now()>=deadline) throw std::runtime_error(message);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
std::shared_ptr<RuntimeResources> resources(unsigned workers) {
    auto limits=ResourceLimits::forMachine(8ull*1024*1024*1024,workers);
    limits.computeWorkers=workers;limits.cpuMips=16*MipCache::slotBytes;
    return std::make_shared<RuntimeResources>(limits,[]{return MemorySample{4ull*1024*1024*1024,64ull*1024*1024};});
}
DocumentPtr document(Pixel color,Sampling sampling=Sampling::Nearest,int side=16) {
    LayerNode layer{Id::generate()};layer.sampling=sampling;
    layer.raster=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,side,side},pack(color));
    return std::make_shared<const DocumentSnapshot>(Id::generate(),side,side,72,std::vector<LayerNode>{layer});
}
Viewport viewport(int side=16) {return {16,16,1,16./side,{side/2.,side/2.},false,double(side),double(side)};}
struct Results {
    std::mutex mutex;std::condition_variable changed;
    std::vector<std::uint64_t> generations;QImage image;QString error;
    void complete(std::uint64_t generation,QImage output,QString failure) {
        std::lock_guard lock(mutex);generations.push_back(generation);image=std::move(output);error=std::move(failure);
        changed.notify_all();
    }
    QImage wait(std::uint64_t generation) {
        std::unique_lock lock(mutex);
        expect(changed.wait_for(lock,std::chrono::seconds(5),[&]{return !generations.empty() && generations.back()==generation;}),
            "Latest CPU request did not complete");
        expect(error.isEmpty() && !image.isNull(),"CPU worker delivered an error or missing image");
        return image;
    }
    bool only(std::vector<std::uint64_t> expected) {
        std::lock_guard lock(mutex);return generations==expected;
    }
};
void sharedAdmissionAndSupersession() {
    auto runtime=resources(2);
    auto firstSlot=runtime->compute.acquire(WorkScheduler::Priority::Processing,false);
    auto secondSlot=runtime->compute.acquire(WorkScheduler::Priority::Processing,false);
    expect(firstSlot && secondSlot,"Could not hold both compute slots");
    Results results;
    CpuWorker worker([&](auto generation,auto image,auto error){results.complete(generation,std::move(image),std::move(error));},runtime);
    auto first=document({1,0,0,1});std::weak_ptr<const DocumentSnapshot> obsolete=first;
    worker.request(1,first,viewport(),16,16);first.reset();
    eventually([&]{return runtime->compute.snapshot().waiting[0]==1;},"CPU rendering bypassed the shared compute gate");
    expect(runtime->compute.snapshot().active==2 && results.only({}),"CPU request completed while all slots were held");

    auto second=document({0,1,0,1});std::weak_ptr<const DocumentSnapshot> replaced=second;
    // The replacement deliberately reuses the generation: cancellation must
    // follow the request token, rather than generation inequality alone.
    worker.request(1,second,viewport(),16,16);second.reset();
    eventually([&]{return obsolete.expired() && runtime->compute.snapshot().waiting[0]==1;},
        "Same-generation superseded CPU request remained queued or retained its document");
    worker.request(100,document({0,0,1,1}),viewport(),16,16);
    eventually([&]{return replaced.expired() && runtime->compute.snapshot().waiting[0]==1;},
        "Newest CPU request did not replace queued work");
    expect(results.only({}),"Superseded blocked CPU work published an image");
    // One free slot must suffice; other application work remains admitted.
    secondSlot.reset();
    expect(results.wait(100).pixel(8,8)==0xff0000ffu,"Newest CPU request returned stale source pixels");
    expect(results.only({100}),"CPU worker published a superseded generation");
    eventually([&]{const auto state=runtime->compute.snapshot();return state.active==1 && state.waiting[0]==0;},
        "CPU completion leaked shared admission");
}
void supersessionDuringSpillRead() {
    // Explicit build-directory fixture: SpillStore verifies disk-backed storage.
    // QTemporaryDir's default system temporary path is deliberately not used.
    QTemporaryDir fixture(QCoreApplication::applicationDirPath()+"/cpu-spill-test-XXXXXX");
    expect(fixture.isValid(),"Could not create owned disk-backed CPU spill fixture");
    struct ReadGate {
        std::mutex mutex;std::condition_variable changed;
        bool armed=false,entered=false,released=false;
        int checkpoint(io::SpillIoStage stage) {
            if(stage!=io::SpillIoStage::Read) return 0;
            std::unique_lock lock(mutex);
            if(!armed) return 0;
            armed=false;entered=true;changed.notify_all();
            return changed.wait_for(lock,std::chrono::seconds(5),[&]{return released;}) ? 0 : ETIMEDOUT;
        }
        void release() {
            std::lock_guard lock(mutex);released=true;changed.notify_all();
        }
    };
    auto gate=std::make_shared<ReadGate>();
    auto storage=std::make_shared<io::SpillStore>(fixture.path().toStdString(),
        io::SpillLimits{65536,65536,0,8,1,32},
        [gate](io::SpillIoStage stage,std::uint64_t){return gate->checkpoint(stage);});
    auto limits=ResourceLimits::forMachine(8ull*1024*1024*1024,1);
    limits.computeWorkers=1;limits.cpuMips=16*MipCache::slotBytes;
    auto runtime=std::make_shared<RuntimeResources>(limits,[]{return MemorySample{4ull*1024*1024*1024,64ull*1024*1024};},storage);
    const auto baseline=runtime->memory->snapshot();
    const std::array<Pixel,4> pixels{Pixel{1,0,0,1},Pixel{0,1,0,1},Pixel{0,0,1,1},Pixel{1,1,1,1}};
    auto tile=runtime->tiles->create(2,2,pixels);
    expect(tile->spillable() && tile->spill() && !tile->spillStatus().resident,
        "CPU supersession fixture did not evict its dense tile");
    TileMap map;map.emplace(TileCoord{0,0},tile);
    LayerNode layer{Id::generate()};layer.raster=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,2,2},PackedPixel{},std::move(map));
    auto old=std::make_shared<const DocumentSnapshot>(Id::generate(),2,2,72,std::vector<LayerNode>{std::move(layer)});
    std::weak_ptr<const DocumentSnapshot> obsolete=old;
    std::weak_ptr<const Tile> obsoleteTile=tile;
    tile.reset();
    Results results;
    CpuWorker worker([&](auto generation,auto image,auto error){results.complete(generation,std::move(image),std::move(error));},runtime);
    // This guard releases the read before worker teardown on every failure path.
    struct Release {std::shared_ptr<ReadGate> gate;~Release(){gate->release();}} release{gate};
    {std::lock_guard lock(gate->mutex);gate->armed=true;}
    worker.request(7,old,viewport(2),16,16);old.reset();
    {
        std::unique_lock lock(gate->mutex);
        expect(gate->changed.wait_for(lock,std::chrono::seconds(2),[&]{return gate->entered;}),
            "CPU worker did not enter spilled source rehydration");
    }
    expect(results.only({}),"Blocked spill read published a CPU image");
    worker.request(7,document({0,0,1,1}),viewport(),16,16);
    gate->release();
    expect(results.wait(7).pixel(8,8)==0xff0000ffu && results.only({7}),
        "Same-generation spill supersession published obsolete pixels or an error");
    eventually([&] {
        const auto held=runtime->memory->snapshot();
        return obsolete.expired() && obsoleteTile.expired() &&
            held.pendingCpu==baseline.pendingCpu && held.pendingGpu==baseline.pendingGpu &&
            held.committedCpu==baseline.committedCpu+16*16*4 && held.committedGpu==baseline.committedGpu &&
            runtime->compute.snapshot().active==0;
    },"Cancelled rehydration retained obsolete tile backing, CPU scratch or admission");
}
void suspendReleasesSourcesAndResumes() {
    auto runtime=resources(1);Results results;
    CpuWorker worker([&](auto generation,auto image,auto error){results.complete(generation,std::move(image),std::move(error));},runtime);
    auto cached=document({1,0,0,1},Sampling::Lanczos,64);
    std::weak_ptr<const DocumentSnapshot> completed=cached;
    std::weak_ptr<const RasterSnapshot> cachedSource=cached->singleLayer().raster;
    worker.request(1,cached,viewport(64),16,16);cached.reset();
    expect(results.wait(1).pixel(8,8)==0xffff0000u,"High-quality CPU render returned wrong pixels");
    eventually([&]{return completed.expired();},"Completed CPU request retained its document");
    expect(!cachedSource.expired(),"Fixture did not populate a source-retaining mip cache");

    auto holder=runtime->compute.acquire(WorkScheduler::Priority::Processing,false);
    auto queued=document({0,1,0,1});std::weak_ptr<const DocumentSnapshot> cancelled=queued;
    worker.request(2,queued,viewport(),16,16);queued.reset();
    eventually([&]{return runtime->compute.snapshot().waiting[0]==1;},"CPU suspend fixture did not reach admission wait");
    worker.suspend();
    eventually([&]{return cancelled.expired() && cachedSource.expired() && runtime->compute.snapshot().waiting[0]==0;},
        "Suspension did not cancel admission and release obsolete source/mip storage");
    expect(runtime->compute.snapshot().active==1 && results.only({1}),"Suspended CPU work acquired a slot or completed");
    worker.request(3,document({0,0,1,1}),viewport(),16,16);
    eventually([&]{return runtime->compute.snapshot().waiting[0]==1;},"CPU worker did not resume after suspension");
    holder.reset();
    expect(results.wait(3).pixel(8,8)==0xff0000ffu,"Resumed CPU worker returned obsolete pixels");
    expect(results.only({1,3}),"Cancelled CPU request completed during resumption");
}
void teardownWhileAdmissionBlocked() {
    auto runtime=resources(2);
    auto firstSlot=runtime->compute.acquire(WorkScheduler::Priority::Processing,false);
    auto secondSlot=runtime->compute.acquire(WorkScheduler::Priority::Processing,false);
    Results results;
    auto worker=std::make_unique<CpuWorker>([&](auto generation,auto image,auto error){results.complete(generation,std::move(image),std::move(error));},runtime);
    auto queued=document({1,0,0,1});std::weak_ptr<const DocumentSnapshot> source=queued;
    worker->request(1,queued,viewport(),16,16);queued.reset();
    eventually([&]{return runtime->compute.snapshot().waiting[0]==1;},"CPU teardown fixture did not reach admission wait");
    worker.reset();
    const auto state=runtime->compute.snapshot();
    expect(state.active==2 && state.waiting[0]==0 && source.expired(),"CPU teardown leaked queued work or released another job's slot");
    expect(results.only({}),"Admission-blocked CPU teardown published a callback");
}
void imageBackingSharesPixelsAndMetadata() {
    MemoryAdmission admission(4096,0,[]{return MemorySample{8192,0};});
    QImage native(7,3,QImage::Format_RGB32);native.fill(0xff1144aau);
    native.setDevicePixelRatio(1.75);native.setDotsPerMeterX(1234);native.setDotsPerMeterY(2345);
    native.setOffset({2,3});native.setColorSpace(QColorSpace(QColorSpace::SRgb));
    native.setText("fixture","shared backing");
    const auto* pixels=native.constBits();
    const auto bytes=static_cast<std::uint64_t>(native.sizeInBytes());
    auto image=admittedImage(std::move(native),admission.require(bytes));
    expect(image.constBits()==pixels && image.pixel(3,1)==0xff1144aau,"Admitted image copied or changed rendered pixels");
    expect(image.devicePixelRatio()==1.75 && image.dotsPerMeterX()==1234 && image.dotsPerMeterY()==2345 &&
           image.offset()==QPoint(2,3) && image.colorSpace()==QColorSpace(QColorSpace::SRgb) &&
           image.text("fixture")=="shared backing","Admitted image lost presentation metadata");
    auto copy=image;
    image={};
    expect(copy.constBits()==pixels && admission.snapshot().committedCpu==bytes,
           "Shared image copy lost pixel backing or its charge");
    copy={};
    expect(admission.snapshot().committedCpu==0,"Last image reference did not release its charge");
    bool rejected=false;
    try {admittedImage(QImage(3,2,QImage::Format_RGB32),admission.require(1));}
    catch(const std::invalid_argument&) {rejected=true;}
    const auto state=admission.snapshot();
    expect(rejected && state.pendingCpu==0 && state.committedCpu==0,"Invalid image charge did not roll back");
}
void imageOutlivesWorkerAndResources() {
    QImage image,copy;
    std::shared_ptr<MemoryAdmission> memory;
    std::weak_ptr<RuntimeResources> disposed;
    {
        auto runtime=resources(1);disposed=runtime;memory=runtime->memory;
        {
            Results results;
            CpuWorker worker([&](auto generation,auto output,auto error){results.complete(generation,std::move(output),std::move(error));},runtime);
            worker.request(1,document({0,1,0,1}),viewport(),16,16);
            image=results.wait(1);copy=image;
        }
        runtime.reset();
    }
    const auto bytes=static_cast<std::uint64_t>(image.sizeInBytes());
    const auto retained=memory->snapshot();
    expect(disposed.expired() && image.constBits()==copy.constBits() && copy.pixel(8,8)==0xff00ff00u,
           "Transferred CPU image retained runtime ownership or lost its pixels");
    expect(retained.pendingCpu==0 && retained.pendingGpu==0 && retained.committedCpu==bytes && retained.committedGpu==0,
           "CPU image or rendering scratch has the wrong post-teardown charge");
    image={};
    expect(memory->snapshot().committedCpu==bytes && copy.pixel(8,8)==0xff00ff00u,
           "CPU image charge was released before its last queued copy");
    copy={};
    expect(memory->snapshot().committedCpu==0,"Retained CPU image leaked its backing charge");
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        sharedAdmissionAndSupersession();supersessionDuringSpillRead();suspendReleasesSourcesAndResumes();teardownWhileAdmissionBlocked();
        imageBackingSharesPixelsAndMetadata();imageOutlivesWorkerAndResources();
        std::cout<<"CPU shared admission, supersession, suspend/cache release, resumption, teardown and retained image backing passed\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
