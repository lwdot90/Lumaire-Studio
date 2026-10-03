#include "rendering/thumbnail_worker.h"
#include "rendering/display_pixel.h"
#include <QCoreApplication>
#include <atomic>
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
    limits.computeWorkers=workers;limits.thumbnailMips=16*MipCache::slotBytes;
    return std::make_shared<RuntimeResources>(limits,[]{return MemorySample{4ull*1024*1024*1024,64ull*1024*1024};});
}
DocumentPtr document(Pixel color,Sampling sampling=Sampling::Nearest,int width=16,int height=16) {
    LayerNode layer{Id::generate()};layer.sampling=sampling;
    layer.raster=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,width,height},pack(color));
    return std::make_shared<const DocumentSnapshot>(Id::generate(),width,height,96,std::vector<LayerNode>{layer});
}
DocumentPtr tiledDocument(RuntimeResources& runtime,Pixel color,Sampling sampling,int side) {
    TileMap tiles;
    tiles.emplace(TileCoord{},runtime.tiles->constant(side,side,color));
    LayerNode layer{Id::generate()};layer.sampling=sampling;
    layer.raster=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,side,side},PackedPixel{},std::move(tiles));
    std::vector<LayerNode> layers;layers.push_back(std::move(layer));
    return std::make_shared<const DocumentSnapshot>(Id::generate(),side,side,96,std::move(layers));
}
DocumentPtr sourceDocument(std::shared_ptr<const RasterSnapshot> source) {
    const int width=static_cast<int>(source->extent.width),height=static_cast<int>(source->extent.height);
    LayerNode layer{Id::generate()};layer.sampling=Sampling::Lanczos;layer.raster=std::move(source);
    std::vector<LayerNode> layers;layers.push_back(std::move(layer));
    return std::make_shared<const DocumentSnapshot>(Id::generate(),width,height,96,std::move(layers));
}
void ledger(const MemoryAdmission& memory,std::uint64_t committed) {
    const auto state=memory.snapshot();
    expect(state.pendingCpu==0 && state.pendingGpu==0 && state.committedGpu==0 && state.committedCpu==committed,
        "Thumbnail suspension retained scratch/pending reservations or released live image backing");
}
struct Results {
    struct Batch {std::uint64_t generation;std::string documentId;std::vector<ThumbnailWorker::Result> images;};
    std::mutex mutex;std::condition_variable changed;std::vector<Batch> batches;
    void complete(std::uint64_t generation,DocumentPtr source,std::vector<ThumbnailWorker::Result> images) {
        std::lock_guard lock(mutex);batches.push_back({generation,source ? source->id.text() : "",std::move(images)});changed.notify_all();
    }
    Batch wait(std::uint64_t generation) {
        std::unique_lock lock(mutex);
        expect(changed.wait_for(lock,std::chrono::seconds(5),[&]{return !batches.empty() && batches.back().generation==generation;}),
            "Latest thumbnail request did not complete");
        expect(!batches.back().documentId.empty(),"Thumbnail result lost its document identity");
        return batches.back();
    }
    QImage image(std::uint64_t generation) {
        const auto batch=wait(generation);
        expect(batch.images.size()==1 && batch.images[0].error.isEmpty() && !batch.images[0].image.isNull(),"Thumbnail worker delivered an invalid batch");
        return batch.images[0].image;
    }
    bool only(std::vector<std::uint64_t> expected) {
        std::lock_guard lock(mutex);
        if(batches.size()!=expected.size()) return false;
        for(std::size_t i=0;i<expected.size();++i) if(batches[i].generation!=expected[i]) return false;
        return true;
    }
    void clearImages() {
        std::lock_guard lock(mutex);
        for(auto& batch:batches) for(auto& result:batch.images) result.image={};
    }
};
void sourceIdentity() {
    auto runtime=resources(2);Results results;
    ThumbnailWorker worker([&](auto generation,auto source,auto images){results.complete(generation,std::move(source),std::move(images));},runtime);
    const auto first=document({1,0,0,1},Sampling::Nearest,100,50);
    auto layers=first->layers();const auto source=layers[0].raster;
    worker.request(1,first,{layers[0].id});
    expect(results.image(1).pixel(10,10)==0xffff0000u,"Thumbnail returned wrong source");
    layers[0].raster=std::make_shared<const RasterSnapshot>(source->id,source->extent,pack({0,1,0,1}),TileMap{},source->revision);
    const auto replacement=std::make_shared<const DocumentSnapshot>(first->id,100,50,96,layers);
    worker.request(2,replacement,{layers[0].id});
    expect(results.image(2).pixel(10,10)==0xff00ff00u,"Equal ID/revision reused stale source thumbnail");
    worker.request(3,first,{layers[0].id});
    expect(results.image(3).pixel(10,10)==0xffff0000u,"Cached thumbnail source recovery failed");
}
void sharedAdmissionAndSupersession() {
    auto runtime=resources(2);
    auto firstSlot=runtime->compute.acquire(WorkScheduler::Priority::Processing,false);
    auto secondSlot=runtime->compute.acquire(WorkScheduler::Priority::Processing,false);
    expect(firstSlot && secondSlot,"Could not hold both thumbnail compute slots");
    Results results;
    ThumbnailWorker worker([&](auto generation,auto source,auto images){results.complete(generation,std::move(source),std::move(images));},runtime);
    auto first=document({1,0,0,1});std::weak_ptr<const DocumentSnapshot> obsolete=first;
    worker.request(1,first,{first->singleLayer().id});first.reset();
    eventually([&]{return runtime->compute.snapshot().waiting[2]==1;},"Thumbnails bypassed shared background admission");
    expect(runtime->compute.snapshot().active==2 && results.only({}),"Thumbnail completed while compute slots were held");
    auto second=document({0,1,0,1});std::weak_ptr<const DocumentSnapshot> replaced=second;
    // Same-generation replacement requires its own cancellation token.
    worker.request(1,second,{second->singleLayer().id});second.reset();
    eventually([&]{return obsolete.expired() && runtime->compute.snapshot().waiting[2]==1;},
        "Same-generation superseded thumbnail retained its document or queued admission");
    auto newest=document({0,0,1,1});
    worker.request(100,newest,{newest->singleLayer().id});
    eventually([&]{return replaced.expired() && runtime->compute.snapshot().waiting[2]==1;},
        "Newest thumbnail did not replace admission-blocked work");
    expect(results.only({}),"Superseded thumbnail batch was published");
    secondSlot.reset();
    const auto batch=results.wait(100);
    expect(batch.documentId==newest->id.text() && batch.images.size()==1 && batch.images[0].id==newest->singleLayer().id,
        "Newest thumbnail batch returned stale document/layer identity");
    expect(results.image(100).pixel(36,36)==0xff0000ffu,"Newest thumbnail returned stale pixels");
    expect(results.only({100}),"Thumbnail worker published a superseded generation");
    eventually([&]{const auto state=runtime->compute.snapshot();return state.active==1 && state.waiting[2]==0;},
        "Thumbnail completion leaked shared admission");
}
void emptyBatchCancelsClearsAndResumes() {
    auto runtime=resources(1);Results results;
    ThumbnailWorker worker([&](auto generation,auto source,auto images){results.complete(generation,std::move(source),std::move(images));},runtime);
    auto cached=document({1,0,0,1},Sampling::Lanczos,144,144);
    std::weak_ptr<const DocumentSnapshot> completed=cached;
    std::weak_ptr<const RasterSnapshot> cachedSource=cached->singleLayer().raster;
    worker.request(1,cached,{cached->singleLayer().id});cached.reset();
    expect(results.image(1).pixel(36,36)==0xffff0000u,"High-quality thumbnail returned wrong pixels");
    eventually([&]{return completed.expired();},"Completed thumbnail request retained its document");
    expect(!cachedSource.expired(),"Fixture did not populate a source-retaining thumbnail mip cache");

    auto holder=runtime->compute.acquire(WorkScheduler::Priority::Processing,false);
    auto queued=document({0,1,0,1});std::weak_ptr<const DocumentSnapshot> cancelled=queued;
    worker.request(2,queued,{queued->singleLayer().id});queued.reset();
    eventually([&]{return runtime->compute.snapshot().waiting[2]==1;},"Thumbnail cancellation fixture did not reach admission wait");
    worker.request(3,document({}),{});
    expect(results.wait(3).images.empty(),"Empty thumbnail batch retained output rows");
    eventually([&]{return cancelled.expired() && cachedSource.expired() && runtime->compute.snapshot().waiting[2]==0;},
        "Empty thumbnail batch failed to cancel admission or release cached sources");
    expect(runtime->compute.snapshot().active==1 && results.only({1,3}),"Cancelled thumbnail consumed admission or completed");
    auto resumed=document({0,0,1,1});worker.request(4,resumed,{resumed->singleLayer().id});
    eventually([&]{return runtime->compute.snapshot().waiting[2]==1;},"Thumbnail worker did not resume after empty batch");
    holder.reset();
    expect(results.image(4).pixel(36,36)==0xff0000ffu,"Resumed thumbnail worker returned obsolete pixels");
    expect(results.only({1,3,4}),"Cancelled thumbnail completed during resumption");
}
void suspendWithoutDocumentReleasesCanonicalAndResumes() {
    auto runtime=resources(2);Results results;
    ThumbnailWorker worker([&](auto generation,auto source,auto images){results.complete(generation,std::move(source),std::move(images));},runtime);
    auto cached=tiledDocument(*runtime,{1,0,0,1},Sampling::Lanczos,144);
    std::weak_ptr<const DocumentSnapshot> completed=cached;
    std::weak_ptr<const RasterSnapshot> cachedSource=cached->singleLayer().raster;
    std::weak_ptr<const Tile> cachedTile=cached->singleLayer().raster->tiles.at(TileCoord{});
    worker.request(1,cached,{cached->singleLayer().id});cached.reset();
    auto retained=results.image(1);
    expect(retained.pixel(36,36)==0xffff0000u,"Canonical thumbnail fixture returned wrong pixels");
    eventually([&]{return completed.expired();},"Completed thumbnail still retained the closed document");
    expect(!cachedSource.expired() && !cachedTile.expired(),"Canonical thumbnail fixture did not retain source tiles through mips");
    const auto imageBytes=static_cast<std::uint64_t>(retained.sizeInBytes());
    results.clearImages();
    auto firstSlot=runtime->compute.acquire(WorkScheduler::Priority::Processing,false);
    auto secondSlot=runtime->compute.acquire(WorkScheduler::Priority::Processing,false);
    // Both blocked documents share the cached raster, so graph pruning cannot
    // release the old source before suspend performs its document-free clear.
    auto queued=sourceDocument(cachedSource.lock());
    std::weak_ptr<const DocumentSnapshot> firstQueued=queued;
    std::weak_ptr<const Tile> firstQueuedTile=queued->singleLayer().raster->tiles.at(TileCoord{});
    worker.request(2,queued,{queued->singleLayer().id});queued.reset();
    eventually([&]{return runtime->compute.snapshot().waiting[2]==1;},"Thumbnail suspend fixture did not fill all compute slots");
    auto pending=sourceDocument(cachedSource.lock());
    std::weak_ptr<const DocumentSnapshot> secondQueued=pending;
    std::weak_ptr<const Tile> secondQueuedTile=pending->singleLayer().raster->tiles.at(TileCoord{});
    worker.request(3,pending,{pending->singleLayer().id});pending.reset();
    worker.suspend();
    eventually([&] {
        const auto state=runtime->memory->snapshot();
        return firstQueued.expired() && secondQueued.expired() && firstQueuedTile.expired() && secondQueuedTile.expired() &&
            cachedSource.expired() && cachedTile.expired() && runtime->tiles->usedBytes()==0 &&
            runtime->compute.snapshot().waiting[2]==0 && state.pendingCpu==0 && state.committedCpu==imageBytes;
    },"Document-free suspension retained canonical/cache/queued ownership while admission was blocked");
    ledger(*runtime->memory,imageBytes);
    expect(runtime->compute.snapshot().active==2 && results.only({1}) && retained.pixel(36,36)==0xffff0000u,
        "Thumbnail suspend consumed a held slot, published stale work or invalidated a retained image copy");

    auto resumed=document({0,0,1,1});worker.request(4,resumed,{resumed->singleLayer().id});
    eventually([&]{return runtime->compute.snapshot().waiting[2]==1;},"Thumbnail did not resume after document-free suspension");
    firstSlot.reset();
    expect(results.image(4).pixel(36,36)==0xff0000ffu,"Resumed document-free thumbnail returned stale pixels");
    expect(results.only({1,4}),"Suspended queued thumbnail published a stale batch during resumption");
    ledger(*runtime->memory,2*imageBytes);
    results.clearImages();retained={};resumed.reset();worker.suspend();
    eventually([&]{const auto state=runtime->memory->snapshot();return state.pendingCpu==0 && state.committedCpu==0;},
        "Final thumbnail suspension retained scratch or image/cache backing");
    ledger(*runtime->memory,0);
}
void imageRefusalReleasesScratchAndResumes() {
    auto sufficient=std::make_shared<std::atomic<bool>>(false);
    auto limits=ResourceLimits::forMachine(8ull*1024*1024*1024,1);
    limits.thumbnailMips=16*MipCache::slotBytes;
    constexpr std::uint64_t imageBytes=72*72*4,scratchBytes=2*1024*1024;
    const auto floor=limits.systemHeadroom;
    auto runtime=std::make_shared<RuntimeResources>(limits,[sufficient,floor] {
        return MemorySample{sufficient->load() ? 4ull*1024*1024*1024 : floor+scratchBytes+imageBytes-1,0};
    });
    Results results;
    ThumbnailWorker worker([&](auto generation,auto source,auto images){results.complete(generation,std::move(source),std::move(images));},runtime);
    const auto source=document({1,0,0,1},Sampling::Nearest,144,144);
    worker.request(1,source,{source->singleLayer().id});
    const auto failed=results.wait(1);
    expect(failed.images.size()==1 && failed.images[0].image.isNull() && !failed.images[0].error.isEmpty(),
        "Thumbnail pixels were allocated after scratch spent the remaining system headroom");
    ledger(*runtime->memory,0);
    worker.suspend();sufficient->store(true);
    worker.request(2,source,{source->singleLayer().id});
    expect(results.image(2).pixel(36,36)==0xffff0000u,"Thumbnail did not recover after image admission refusal and suspension");
    ledger(*runtime->memory,imageBytes);
    results.clearImages();worker.suspend();
    eventually([&]{const auto state=runtime->memory->snapshot();return state.pendingCpu==0 && state.committedCpu==0;},
        "Recovered thumbnail retained scratch/pixel reservations after suspension");
    ledger(*runtime->memory,0);
}
void teardownWhileAdmissionBlocked() {
    auto runtime=resources(2);
    auto firstSlot=runtime->compute.acquire(WorkScheduler::Priority::Processing,false);
    auto secondSlot=runtime->compute.acquire(WorkScheduler::Priority::Processing,false);
    Results results;
    auto worker=std::make_unique<ThumbnailWorker>([&](auto generation,auto source,auto images){results.complete(generation,std::move(source),std::move(images));},runtime);
    auto queued=document({1,0,0,1});std::weak_ptr<const DocumentSnapshot> source=queued;
    worker->request(1,queued,{queued->singleLayer().id});queued.reset();
    eventually([&]{return runtime->compute.snapshot().waiting[2]==1;},"Thumbnail teardown fixture did not reach admission wait");
    worker.reset();
    const auto state=runtime->compute.snapshot();
    expect(state.active==2 && state.waiting[2]==0 && source.expired(),"Thumbnail teardown leaked queued work or released another job's slot");
    expect(results.only({}),"Admission-blocked thumbnail teardown published a callback");
}
void cachedImageOutlivesWorkerAndResources() {
    QImage image,copy;
    std::shared_ptr<MemoryAdmission> memory;
    std::weak_ptr<RuntimeResources> disposed;
    {
        auto runtime=resources(1);disposed=runtime;memory=runtime->memory;
        {
            Results results;
            ThumbnailWorker worker([&](auto generation,auto source,auto images){results.complete(generation,std::move(source),std::move(images));},runtime);
            const auto source=document({0,0,1,1},Sampling::Nearest,100,50);
            worker.request(1,source,{source->singleLayer().id});
            image=results.image(1);copy=image;
            worker.request(2,source,{source->singleLayer().id});
            expect(results.image(2).constBits()==image.constBits(),"Thumbnail cache copied its admitted pixel backing");
            worker.request(3,source,{});
            expect(results.wait(3).images.empty(),"Thumbnail fixture did not clear its cache");
        }
        runtime.reset();
    }
    const auto bytes=static_cast<std::uint64_t>(image.sizeInBytes());
    const auto retained=memory->snapshot();
    expect(disposed.expired() && image.size()==QSize(72,36) && copy.devicePixelRatio()==2 &&
           image.constBits()==copy.constBits() && copy.pixel(36,18)==0xff0000ffu,
           "Transferred thumbnail retained runtime ownership or lost pixels/DPR");
    expect(retained.pendingCpu==0 && retained.pendingGpu==0 && retained.committedCpu==bytes && retained.committedGpu==0,
           "Thumbnail cache/queued copies duplicated the charge or retained scratch");
    image={};
    expect(memory->snapshot().committedCpu==bytes && copy.pixel(36,18)==0xff0000ffu,
           "Thumbnail backing charge was released before its last queued copy");
    copy={};
    expect(memory->snapshot().committedCpu==0,"Retained thumbnail leaked its backing charge");
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        auto source=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,100,50},pack({1,0,0,1}));
        LayerNode layer{Id::generate()};layer.raster=source;layer.sampling=Sampling::Nearest;
        layer.visible=false;layer.opacity=0;layer.blend=BlendMode::Multiply;layer.parent=Id::generate();
        MipCache cache(1024*1024);
        auto image=renderThumbnail(layer,100,50,cache);
        expect(image.size()==QSize(72,36) && image.devicePixelRatio()==2,"Canvas-shaped Retina thumbnail geometry");
        expect(image.pixel(35,17)==0xffff0000u,"Source thumbnail inherited hidden/opacity/blend appearance");
        layer.localToDocument.tx=50;
        image=renderThumbnail(layer,100,50,cache);
        expect(image.pixel(60,17)==0xffff0000u,"Thumbnail placement lost source pixels");
        expect(image.pixel(6,6)==displayArgb({},decodeSrgb(.32f),6,6),"Thumbnail checker/placement mismatch");
        expect(renderThumbnail(layer,100,50,cache,[]{return true;}).isNull(),"Cancelled thumbnail published pixels");
        std::stop_source thumbnailStop;thumbnailStop.request_stop();
        expect(renderThumbnail(layer,100,50,cache,{},{},thumbnailStop.get_token()).isNull(),
            "Stopped thumbnail published pixels");
        layer.sampling=Sampling::Lanczos;int checks=0;
        expect(renderThumbnail(layer,100,50,cache,[&]{return ++checks>100;}).isNull() && checks>100,"In-progress filtering ignored thumbnail cancellation");
        sourceIdentity();sharedAdmissionAndSupersession();emptyBatchCancelsClearsAndResumes();
        suspendWithoutDocumentReleasesCanonicalAndResumes();imageRefusalReleasesScratchAndResumes();teardownWhileAdmissionBlocked();
        cachedImageOutlivesWorkerAndResources();
        std::cout<<"thumbnail geometry/source identity, shared admission, supersession, document-free suspension, scratch rollback, resumption, teardown and retained image backing passed\n";
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
