#include "app/window.h"
#include "io/spill_store.h"
#include <QApplication>
#include <QDir>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <chrono>
#include <future>
#include <thread>

using namespace compositor;
namespace {
QString buildDirectory;
QByteArray pixels(const engine::DocumentPtr& document) {
    QByteArray result;
    for(int y=0;y<document->height;++y) for(int x=0;x<document->width;++x)
        for(const auto value:engine::pack(document->singleLayer().raster->pixel(x,y))) {
            result.append(static_cast<char>(value&255));result.append(static_cast<char>(value>>8));
        }
    return result;
}
std::shared_ptr<RuntimeResources> resourcesIn(const QString& directory) {
    io::SpillLimits spillLimits;spillLimits.maxBytes=64*1024*1024;
    spillLimits.maxPayloadBytes=2*1024*1024;spillLimits.maxEntries=256;spillLimits.maxIoOperations=1;
    return std::make_shared<RuntimeResources>(ResourceLimits::forMachine(8ull*1024*1024*1024,2),
        []{return MemorySample{4ull*1024*1024*1024,64ull*1024*1024};},
        std::make_shared<io::SpillStore>(directory.toStdString(),spillLimits));
}
}
class JobCancellationTest final:public QObject {
    Q_OBJECT
private slots:
    void cancellationLatchDoesNotFollowAnotherBusyTab() {
        QTemporaryDir directory(QDir(buildDirectory).filePath("cancel-tabs-XXXXXX"));QVERIFY(directory.isValid());
        QImage input(24,16,QImage::Format_RGB32);input.fill(QColor(120,90,60));
        const auto path=directory.filePath("gradient.png");QVERIFY(input.save(path));
        auto resources=resourcesIn(directory.path());
        MainWindow window(nullptr,true,{},resources);window.show();
        QSignalSpy finished(&window,&MainWindow::operationFinished);
        window.openPath(path);QTRY_COMPARE_WITH_TIMEOUT(finished.count(),1,10000);QVERIFY(finished.last()[0].toBool());
        auto* firstPage=window.tabs()->currentWidget();const auto firstDocument=window.currentDocument();const bool firstDirty=window.currentDirty();
        window.openPath(path);QTRY_COMPARE_WITH_TIMEOUT(finished.count(),2,10000);QVERIFY(finished.last()[0].toBool());
        auto* secondPage=window.tabs()->currentWidget();const auto secondDocument=window.currentDocument();const bool secondDirty=window.currentDirty();
        QVERIFY(firstPage!=secondPage);
        std::promise<std::optional<WorkScheduler::Permit>> promise;auto future=promise.get_future();
        std::jthread blocker([&](std::stop_token stop) {
            promise.set_value(resources->compute.acquire(WorkScheduler::Priority::Processing,true,stop));
        });
        QTRY_VERIFY_WITH_TIMEOUT(future.wait_for(std::chrono::milliseconds(0))==std::future_status::ready,10000);
        auto held=future.get();QVERIFY(held.has_value());blocker.join();
        engine::AdjustmentParameters levels;levels.kind=engine::AdjustmentKind::Levels;levels.levels.gamma=2;
        window.tabs()->setCurrentWidget(firstPage);window.applyAdjustment(levels);QVERIFY(window.currentBusy());
        window.tabs()->setCurrentWidget(secondPage);window.applyAdjustment(levels);QVERIFY(window.currentBusy());
        // The window processing pool is serial: the first task waits on
        // admission while the second remains queued in that pool.
        QTRY_VERIFY_WITH_TIMEOUT(resources->compute.snapshot().waiting[1]>=1,10000);
        auto* cancel=window.findChild<QPushButton*>("cancelOperationButton");QVERIFY(cancel);
        auto* status=window.findChild<QLabel*>("operationStatusLabel");QVERIFY(status);
        window.tabs()->setCurrentWidget(firstPage);QVERIFY(cancel->isEnabled());
        QTest::mouseClick(cancel,Qt::LeftButton);QVERIFY(!cancel->isEnabled());
        QVERIFY(status->text().contains("cancel",Qt::CaseInsensitive));
        // The shared status widget must reflect this session's independent
        // cancellation state even while the previous task is still retiring.
        window.tabs()->setCurrentWidget(secondPage);QVERIFY(window.currentBusy());
        QVERIFY(cancel->isVisible());QVERIFY(cancel->isEnabled());
        QVERIFY(!status->text().contains("cancel",Qt::CaseInsensitive));
        QTest::mouseClick(cancel,Qt::LeftButton);QVERIFY(!cancel->isEnabled());held.reset();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(),4,10000);
        QVERIFY(!finished.at(2)[0].toBool());QVERIFY(!finished.at(3)[0].toBool());
        window.tabs()->setCurrentWidget(firstPage);QVERIFY(!window.currentBusy());
        QVERIFY(window.currentDocument()==firstDocument);QCOMPARE(window.currentDirty(),firstDirty);
        window.tabs()->setCurrentWidget(secondPage);QVERIFY(!window.currentBusy());
        QVERIFY(window.currentDocument()==secondDocument);QCOMPARE(window.currentDirty(),secondDirty);
        QVERIFY(!cancel->isVisible());
    }
    void queuedAdjustmentCancelsWithoutChangingHistory() {
        QTemporaryDir directory(QDir(buildDirectory).filePath("cancel-workflow-XXXXXX"));QVERIFY(directory.isValid());
        QImage input(32,24,QImage::Format_RGB32);
        for(int y=0;y<input.height();++y) for(int x=0;x<input.width();++x)
            input.setPixel(x,y,qRgb(32+x*6,48+x*4,64+x*3));
        const auto path=directory.filePath("gradient.png");QVERIFY(input.save(path));
        auto resources=resourcesIn(directory.path());
        MainWindow window(nullptr,true,{},resources);window.show();
        QSignalSpy finished(&window,&MainWindow::operationFinished);window.openPath(path);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(),1,10000);QVERIFY(finished.last()[0].toBool());
        const auto imported=window.currentDocument();const auto importedPixels=pixels(imported);const bool importedDirty=window.currentDirty();
        engine::AdjustmentParameters prior;prior.kind=engine::AdjustmentKind::Brightness;prior.value=.1;
        window.applyAdjustment(prior);QTRY_COMPARE_WITH_TIMEOUT(finished.count(),2,10000);
        QVERIFY(finished.last()[0].toBool());const auto redoRevision=window.currentDocument();
        QVERIFY(redoRevision!=imported);window.undoCurrent();QVERIFY(window.currentDocument()==imported);
        QCOMPARE(window.currentDirty(),importedDirty);
        // Acquire only on a helper worker. The moved permit holds the single
        // heavy-processing slot while light interactive canvas work can proceed.
        std::promise<std::optional<WorkScheduler::Permit>> promise;auto future=promise.get_future();
        std::jthread blocker([&](std::stop_token stop) {
            promise.set_value(resources->compute.acquire(WorkScheduler::Priority::Processing,true,stop));
        });
        QTRY_VERIFY_WITH_TIMEOUT(future.wait_for(std::chrono::milliseconds(0))==std::future_status::ready,10000);
        auto held=future.get();QVERIFY(held.has_value());blocker.join();
        engine::AdjustmentParameters levels;levels.kind=engine::AdjustmentKind::Levels;levels.levels.gamma=2;
        window.applyAdjustment(levels);QVERIFY(window.currentBusy());
        QTRY_VERIFY_WITH_TIMEOUT(resources->compute.snapshot().waiting[1]>=1,10000);
        auto* cancel=window.findChild<QPushButton*>("cancelOperationButton");QVERIFY(cancel);
        auto* progress=window.findChild<QProgressBar*>("operationProgress");QVERIFY(progress);
        auto* status=window.findChild<QLabel*>("operationStatusLabel");QVERIFY(status);
        QTRY_VERIFY_WITH_TIMEOUT(!cancel->visibleRegion().isEmpty(),1000);
        QVERIFY(cancel->isEnabled());QVERIFY(progress->isVisible());
        QTest::mouseClick(cancel,Qt::LeftButton);
        QVERIFY(!cancel->isEnabled());QVERIFY(status->text().contains("cancel",Qt::CaseInsensitive));
        // Release before any destructor can wait for processing. Cancellation
        // may already have retired the queued task while this permit was held.
        held.reset();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(),3,10000);
        QVERIFY(!finished.last()[0].toBool());QVERIFY(!window.currentBusy());
        QVERIFY(window.currentDocument()==imported);QCOMPARE(pixels(window.currentDocument()),importedPixels);
        QCOMPARE(window.currentDirty(),importedDirty);QVERIFY(!cancel->isVisible());QVERIFY(!progress->isVisible());
        window.redoCurrent();QVERIFY(window.currentDocument()==redoRevision);
        window.undoCurrent();QVERIFY(window.currentDocument()==imported);QCOMPARE(window.currentDirty(),importedDirty);
        window.applyAdjustment(levels);QTRY_COMPARE_WITH_TIMEOUT(finished.count(),4,10000);
        QVERIFY2(finished.last()[0].toBool(),qPrintable(finished.last()[1].toString()));
        const auto adjusted=window.currentDocument();QVERIFY(adjusted!=imported);QVERIFY(pixels(adjusted)!=importedPixels);
        QVERIFY(!window.currentBusy());QVERIFY(!cancel->isVisible());QVERIFY(!progress->isVisible());
        window.undoCurrent();QVERIFY(window.currentDocument()==imported);QCOMPARE(pixels(window.currentDocument()),importedPixels);
        QCOMPARE(window.currentDirty(),importedDirty);window.redoCurrent();QVERIFY(window.currentDocument()==adjusted);
    }
};
int main(int argc,char** argv) {
    if(argc<2) {qCritical("Pass the existing disk-backed build directory");return 2;}
    buildDirectory=QDir(QString::fromLocal8Bit(argv[1])).absolutePath();
    QApplication app(argc,argv);JobCancellationTest test;return QTest::qExec(&test,argc-1,argv+1);
}
#include "job_cancellation_test.moc"
