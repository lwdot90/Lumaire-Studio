#include "app/adjustment_preview.h"
#include <QtConcurrent/QtConcurrentRun>
#include <QtTest>
#include <array>
#include <atomic>
#include <thread>

using namespace compositor;
using namespace compositor::engine;
namespace {
std::shared_ptr<RuntimeResources> resources() {
    return std::make_shared<RuntimeResources>(ResourceLimits::forMachine(8ull*1024*1024*1024,2),
        []{return MemorySample{4ull*1024*1024*1024,64ull*1024*1024};});
}
DocumentPtr fixture(TileStore& store) {
    const std::array pixels{Pixel{.125f,.0625f,.03125f,.5f}};
    TileMap tiles;tiles.emplace(TileCoord{0,0},store.create(1,1,pixels));
    auto raster=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,1,1},PackedPixel{},std::move(tiles));
    return std::make_shared<const DocumentSnapshot>(Id::generate(),Id::generate(),1,1,72,raster);
}
}
class AdjustmentPreviewTest final:public QObject {
    Q_OBJECT
private slots:
    void rapidRequestsPublishOnlyLatestAndRevisionsUseSource() {
        auto runtime=resources();QThreadPool pool;pool.setMaxThreadCount(1);
        auto input=fixture(*runtime->tiles);const auto target=input->singleLayer().id;
        AdjustmentPreview preview(runtime,&pool);int ready=0,failures=0;DocumentPtr result;
        connect(&preview,&AdjustmentPreview::previewReady,this,[&](DocumentPtr document,std::shared_ptr<EditTransaction> edit,RgbHistogram histogram) {
            QCOMPARE(QThread::currentThread(),preview.thread());QVERIFY(edit);QCOMPARE(histogram.samples,std::uint64_t(1));
            ++ready;result=std::move(document);
        });
        connect(&preview,&AdjustmentPreview::failed,this,[&](QString){++failures;});
        preview.request(input,target,{{AdjustmentKind::Exposure,1}});
        preview.request(input,target,{{AdjustmentKind::Exposure,-1}});
        preview.request(input,target,{{AdjustmentKind::Exposure,2}});
        QTRY_COMPARE(ready,1);QCOMPARE(failures,0);
        QCOMPARE(pack(result->singleLayer().raster->pixel(0,0)),pack(Pixel{.5f,.25f,.125f,.5f}));
        QCOMPARE(result->singleLayer().adjustments->source,input->singleLayer().raster);
        preview.request(result,target,{{AdjustmentKind::Exposure,1}});
        QTRY_COMPARE(ready,2);QCOMPARE(failures,0);
        QCOMPARE(pack(result->singleLayer().raster->pixel(0,0)),pack(Pixel{.25f,.125f,.0625f,.5f}));
        QCOMPARE(result->singleLayer().adjustments->source,input->singleLayer().raster);
        QCOMPARE(pack(input->singleLayer().raster->pixel(0,0)),pack(Pixel{.125f,.0625f,.03125f,.5f}));
        pool.waitForDone();
    }
    void activeSupersessionWaitsForRetirementAndPublishesLatest() {
        auto runtime=resources();QThreadPool pool;pool.setMaxThreadCount(1);
        auto holder=runtime->compute.acquire(WorkScheduler::Priority::Processing,true);
        auto input=fixture(*runtime->tiles);const auto target=input->singleLayer().id;
        AdjustmentPreview preview(runtime,&pool);int ready=0,failures=0;DocumentPtr result;
        connect(&preview,&AdjustmentPreview::previewReady,this,[&](DocumentPtr document,std::shared_ptr<EditTransaction>,RgbHistogram){++ready;result=std::move(document);});
        connect(&preview,&AdjustmentPreview::failed,this,[&](QString){++failures;});
        preview.request(input,target,{{AdjustmentKind::Exposure,1}});
        QTRY_COMPARE(runtime->compute.snapshot().waiting[1],1u);
        preview.request(input,target,{{AdjustmentKind::Exposure,2}});
        QTest::qWait(200);QCOMPARE(ready,0);QCOMPARE(failures,0);
        QVERIFY(runtime->compute.snapshot().waiting[1]<=1u);
        holder.reset();QTRY_COMPARE(ready,1);QCOMPARE(failures,0);
        QCOMPARE(pack(result->singleLayer().raster->pixel(0,0)),pack(Pixel{.5f,.25f,.125f,.5f}));
        pool.waitForDone();
    }
    void cancelActiveAndPendingSuppressesSignals() {
        auto runtime=resources();QThreadPool pool;pool.setMaxThreadCount(1);
        auto holder=runtime->compute.acquire(WorkScheduler::Priority::Processing,true);
        auto input=fixture(*runtime->tiles);const auto target=input->singleLayer().id;
        AdjustmentPreview preview(runtime,&pool);int delivered=0;
        connect(&preview,&AdjustmentPreview::previewReady,this,[&](DocumentPtr,std::shared_ptr<EditTransaction>,RgbHistogram){++delivered;});
        connect(&preview,&AdjustmentPreview::failed,this,[&](QString){++delivered;});
        preview.request(input,target,{{AdjustmentKind::Exposure,1}});
        QTRY_COMPARE(runtime->compute.snapshot().waiting[1],1u);
        preview.request(input,target,{{AdjustmentKind::Exposure,2}});preview.cancel();
        QTRY_COMPARE(runtime->compute.snapshot().waiting[1],0u);
        holder.reset();pool.waitForDone();QTest::qWait(200);QCOMPARE(delivered,0);
        preview.request(input,target,{{AdjustmentKind::Exposure,1}});preview.cancel();
        QTest::qWait(200);QCOMPARE(delivered,0);QCOMPARE(pool.activeThreadCount(),0);
    }
    void destructionWithActiveAndQueuedWorkIsSafe() {
        auto runtime=resources();QThreadPool pool;pool.setMaxThreadCount(1);
        auto input=fixture(*runtime->tiles);const auto target=input->singleLayer().id;
        auto holder=runtime->compute.acquire(WorkScheduler::Priority::Processing,true);
        auto preview=std::make_unique<AdjustmentPreview>(runtime,&pool);
        preview->request(input,target,{{AdjustmentKind::Exposure,1}});
        QTRY_COMPARE(runtime->compute.snapshot().waiting[1],1u);
        preview.reset();QTRY_COMPARE(runtime->compute.snapshot().waiting[1],0u);
        holder.reset();pool.waitForDone();

        std::atomic<bool> release=false,started=false;
        auto blocker=QtConcurrent::run(&pool,[&]{started=true;while(!release.load()) std::this_thread::yield();});
        struct ReleaseOnExit {std::atomic<bool>& flag;~ReleaseOnExit(){flag=true;}} releaseOnExit{release};
        QTRY_VERIFY(started.load());
        preview=std::make_unique<AdjustmentPreview>(runtime,&pool);
        preview->request(input,target,{{AdjustmentKind::Exposure,2}});
        QTest::qWait(200);preview.reset();release=true;pool.waitForDone();blocker.waitForFinished();
        QCoreApplication::processEvents();QCOMPARE(runtime->compute.snapshot().active,0u);
        QCOMPARE(runtime->compute.snapshot().waiting[1],0u);
    }
};
QTEST_MAIN(AdjustmentPreviewTest)
#include "adjustment_preview_test.moc"
