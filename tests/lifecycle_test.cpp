#include "app/window.h"
#include "core/tiles.h"
#include "io/spill_store.h"
#include <QApplication>
#include <QDialog>
#include <QFocusEvent>
#include <QLineEdit>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScreen>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <atomic>

using namespace compositor;
namespace {
// Phase observations are independent, instantaneous samples, not peaks or a
// low-resource qualification. Do not wait for cleanup or alter native timing.
void observeResources(const char* phase,const char* backend,const RuntimeResources& resources) {
    const auto process=MemorySample::read();
    const auto ledger=resources.memory->snapshot();
    const auto optionalBytes=[](const std::optional<std::uint64_t>& value) {
        return value ? QJsonValue(qint64(*value)) : QJsonValue(QJsonValue::Null);
    };
    QJsonObject entry{{"event","lifecycle_resource_observation"},{"phase",phase},{"backend",backend},
        {"observed_ns",qint64(monotonicNs())},{"qualification",false},
        {"rss_bytes",optionalBytes(process.resident)},{"mem_available_bytes",optionalBytes(process.available)},
        {"committed_cpu_bytes",qint64(ledger.committedCpu)},{"committed_gpu_bytes",qint64(ledger.committedGpu)},
        {"pending_cpu_bytes",qint64(ledger.pendingCpu)},{"pending_gpu_bytes",qint64(ledger.pendingGpu)},
        {"tile_budget_bytes",qint64(resources.tiles->usedBytes())},{"spill_enabled",bool(resources.spill)}};
    if(resources.spill) {
        const auto disk=resources.spill->stats();
        entry.insert("spill",QJsonObject{{"charged_bytes",qint64(disk.bytes)},{"entries",qint64(disk.entries)},
            {"active_io",qint64(disk.activeIo)},{"cleanup_failures",qint64(disk.cleanupFailures)},
            {"cleanup_blocked",disk.cleanupBlocked}});
    } else entry.insert("spill",QJsonValue(QJsonValue::Null));
    qInfo().noquote()<<QJsonDocument(entry).toJson(QJsonDocument::Compact);
}
}
class QuietPointer:public QObject {
    bool eventFilter(QObject*,QEvent* e) override {
        return e->type()==QEvent::MouseMove || e->type()==QEvent::Enter || e->type()==QEvent::Leave;
    }
};
class LifecycleTest:public QObject {
    Q_OBJECT
private slots:
    void lifecycle() {
        // Qt 6.11 Wayland's test-only expose workaround attaches an shm buffer
        // when rendering is asynchronous. That conflicts with Vulkan explicit
        // sync. We render real buffers, so exercise the production expose path.
        qunsetenv("QT_QTESTLIB_RUNNING");
        const bool cpu=qEnvironmentVariable("COMPOSITOR_TEST_BACKEND","cpu")!="vulkan";
        const auto resources=defaultRuntimeResources();
        const auto backend=cpu ? "cpu" : "vulkan";
        std::atomic<int> validationErrors=0; QVulkanInstance instance;
        if(!cpu) {
            QVERIFY2(instance.supportedLayers().contains("VK_LAYER_KHRONOS_validation"),"Qualification requires actual validation layer");
            instance.setLayers({"VK_LAYER_KHRONOS_validation"}); instance.setApiVersion(QVersionNumber(1,2));
            instance.installDebugOutputFilter(QVulkanInstance::DebugUtilsFilter([&](auto severity,auto,const void* data) {
                if(severity.testFlag(QVulkanInstance::ErrorSeverity)) {
                    ++validationErrors;
                    qWarning().noquote()<<static_cast<const VkDebugUtilsMessengerCallbackDataEXT*>(data)->pMessage;
                }
                return false;
            }));
            QVERIFY(instance.create());
        }
        {
            MainWindow window(&instance,cpu); window.show();
            auto* canvas=window.currentCanvas(); QVERIFY(canvas);
            connect(canvas,&Canvas::diagnostic,this,[](const QString& message){qInfo().noquote()<<message;});
            QTRY_VERIFY_WITH_TIMEOUT(canvas->frameCount()>0,10000);
            QCOMPARE(canvas->cpu(),cpu);
            qInfo()<<"initial DPR:"<<canvas->devicePixelRatio();
            observeResources("initial_frame",backend,*resources);
            const auto cleanDocument=window.currentDocument();
            if(!cpu) {
                QSignalSpy operations(&window,&MainWindow::operationFinished);
                window.fillCurrent({1,0,0,.75f});QTRY_COMPARE_WITH_TIMEOUT(operations.count(),1,10000);
                QVERIFY(operations.last()[0].toBool());
                window.addLayer();window.fillCurrent({0,0,1,.5f});QTRY_COMPARE_WITH_TIMEOUT(operations.count(),2,10000);
                QVERIFY(operations.last()[0].toBool());
                window.setLayerAppearance(true,.625f,engine::BlendMode::Screen);
                const auto frames=window.currentCanvas()->frameCount();
                QTRY_VERIFY_WITH_TIMEOUT(window.currentCanvas()->frameCount()>frames,15000);
                QVERIFY(!window.currentCanvas()->cpu());QCOMPARE(window.currentCanvas(),canvas);
                QCOMPARE(window.currentDocument()->layers().size(),std::size_t(2));
                observeResources("layered_frame",backend,*resources);
                qInfo("phase: GPU high-quality mip presentation");
                const auto ordinaryDocument=window.currentDocument();
                const auto ordinaryViewport=canvas->viewport();
                QSignalSpy filteredFrames(canvas,&Canvas::timing);
                window.setLayerSampling(engine::Sampling::Lanczos);
                auto reduced=ordinaryViewport;reduced.zoom=.25;reduced.followsFit=false;
                const auto requestedAfter=monotonicNs();
                canvas->restoreViewport(reduced,"High-quality mip lifecycle check");
                // Ignore completions belonging to older in-flight requests.
                const auto filteredCompleted=[&] {
                    for(const auto& event:filteredFrames) {
                        const auto entry=event[0].toJsonObject();
                        if(entry["event"]=="vulkan_frame" &&
                           entry["requested_ns"].toInteger()>=requestedAfter) return true;
                    }
                    return false;
                };
                QTRY_VERIFY_WITH_TIMEOUT(filteredCompleted(),30000);
                QCOMPARE(window.currentCanvas(),canvas);QVERIFY(!canvas->cpu());
                observeResources("mip_frame",backend,*resources);
                window.undoCurrent();QVERIFY(window.currentDocument()==ordinaryDocument);
                filteredFrames.clear();
                const auto restoreRequestedAfter=monotonicNs();
                canvas->restoreViewport(ordinaryViewport,"Restore lifecycle viewport");
                // Restoring starts asynchronous work too. An arbitrary quiet
                // delay can expire before this frame finishes under validation
                // or ASan, which would misclassify its completion as idle work.
                const auto restoredCompleted=[&] {
                    for(const auto& event:filteredFrames) {
                        const auto entry=event[0].toJsonObject();
                        if(entry["event"]=="vulkan_frame" &&
                           entry["requested_ns"].toInteger()>=restoreRequestedAfter) return true;
                    }
                    return false;
                };
                QTRY_VERIFY_WITH_TIMEOUT(restoredCompleted(),30000);
            }
            // Isolate idle from a user moving the physical mouse on this desktop.
            // This filter does not suppress expose/update/resize/timer events.
            QuietPointer quiet; canvas->installEventFilter(&quiet);
            QTest::qWait(1500); auto idle=canvas->frameCount(); QTest::qWait(500); QCOMPARE(canvas->frameCount(),idle);
            canvas->removeEventFilter(&quiet);
            qInfo("phase: resize");
            for(int i=0;i<8;++i) { window.resize(800+i*23,600+i*11); QTest::qWait(40); }
            QTRY_VERIFY(canvas->frameCount()>idle);
            QTest::mouseMove(canvas,QPoint(100,100));
            QTest::keyPress(canvas,Qt::Key_Space); QTest::mousePress(canvas,Qt::LeftButton,{},QPoint(100,100));
            const auto center=canvas->viewport().center;
            QTest::mouseMove(canvas,QPoint(140,120)); QTest::mouseRelease(canvas,Qt::LeftButton,{},QPoint(140,120)); QTest::keyRelease(canvas,Qt::Key_Space);
            QTRY_VERIFY_WITH_TIMEOUT(!window.currentBusy(),10000);
            QVERIFY(canvas->viewport().center.x!=center.x);
            // Focus loss cancels a held Space gesture, even without key-up.
            QTest::keyPress(canvas,Qt::Key_Space);
            QFocusEvent lost(QEvent::FocusOut); QCoreApplication::sendEvent(canvas,&lost);
            const auto afterFocus=canvas->viewport().center;
            QTest::mousePress(canvas,Qt::LeftButton,{},QPoint(100,100));
            QTest::mouseMove(canvas,QPoint(130,130)); QTest::mouseRelease(canvas,Qt::LeftButton,{},QPoint(130,130));
            QTRY_VERIFY_WITH_TIMEOUT(!window.currentBusy(),10000);
            QCOMPARE(canvas->viewport().center.x,afterFocus.x);
            QCOMPARE(canvas->viewport().center.y,afterFocus.y);
            canvas->actualPixels(); QCOMPARE(canvas->viewport().zoom,1.0);
            qInfo("phase: tabs");
            window.addDocument(); QTRY_VERIFY(window.currentCanvas()->frameCount()>0); QCOMPARE(window.tabs()->count(),2);
            window.tabs()->setCurrentIndex(0); QTest::qWait(150); QCOMPARE(window.currentCanvas(),canvas);
            // Closing another tab must not destroy the original canvas.
            window.closeDocument(1); QCOMPARE(window.currentCanvas(),canvas);
            qInfo("phase: minimize/restore");
            window.showMinimized(); QTest::qWait(250);
            idle=canvas->frameCount();
            if(QGuiApplication::platformName()=="wayland") {
                // xdg-shell has no unset_minimized request. Synthetic QTest
                // input cannot supply a compositor activation serial. Remap
                // for automated recovery; taskbar restore is a manual scenario.
                qInfo("Wayland automated restore uses hide/show remapping");
                window.hide();
            }
            window.showNormal(); window.activateWindow();
            QTRY_VERIFY_WITH_TIMEOUT(canvas->isExposed(),10000);
            canvas->redraw(); QTRY_VERIFY(canvas->frameCount()>idle);
            qInfo("phase: monitor moves");
            for(auto* screen:QGuiApplication::screens()) { window.windowHandle()->setScreen(screen); window.move(screen->availableGeometry().topLeft()+QPoint(20,20)); QTest::qWait(100); qInfo()<<"requested screen:"<<screen->name()<<"canvas DPR:"<<canvas->devicePixelRatio(); }
            { QDialog dialog(&window); QLineEdit edit(&dialog);
                dialog.setModal(true); dialog.show(); dialog.activateWindow(); edit.setFocus();
                QTRY_VERIFY(edit.hasFocus());
                QTest::keyClicks(&edit,"focus"); QCOMPARE(edit.text(),QString("focus")); dialog.close(); }
            canvas->activateCanvas();
            QTest::qWait(100);
            if(QGuiApplication::platformName()=="wayland" && !window.isActiveWindow()) {
                qInfo("Compositor did not reactivate the app after dialog close; real-user focus recovery remains a manual check");
            } else {
                QTRY_VERIFY(QGuiApplication::focusWindow()==canvas || (QApplication::focusWidget() &&
                    QApplication::focusWidget()->property("canvasTarget").value<Canvas*>()==canvas));
                const auto beforeRecovery=canvas->viewport().center;
                QTest::keyPress(QGuiApplication::focusWindow(),Qt::Key_Space);
                QTest::mousePress(canvas,Qt::LeftButton,{},QPoint(100,100));
                QTest::mouseMove(canvas,QPoint(130,115));QTest::mouseRelease(canvas,Qt::LeftButton,{},QPoint(130,115));
                QTest::keyRelease(QGuiApplication::focusWindow(),Qt::Key_Space);
                QTRY_VERIFY_WITH_TIMEOUT(!window.currentBusy(),10000);
                QVERIFY(canvas->viewport().center.x!=beforeRecovery.x);
                const auto tabCount=window.tabs()->count();
                QTest::keyClick(QGuiApplication::focusWindow(),Qt::Key_N,Qt::ControlModifier);
                QTRY_COMPARE(window.tabs()->count(),tabCount+1);
                window.closeDocument(window.tabs()->currentIndex());QCOMPARE(window.currentCanvas(),canvas);
            }
            QTest::mouseClick(canvas,Qt::LeftButton,{},QPoint(120,120));
            QTRY_VERIFY_WITH_TIMEOUT(!window.currentBusy(),10000);
            // Actual native surface destruction/recreation, with the worker alive.
            qInfo("phase: native surface recreation");
            idle=canvas->frameCount(); canvas->destroy(); canvas->show(); canvas->redraw(); QTRY_VERIFY_WITH_TIMEOUT(canvas->frameCount()>idle,10000);
            if(!cpu) {
                qInfo("phase: CPU fallback");
                const auto saved=canvas->viewport(); canvas->forceCpu("injected backend failure");
                QTRY_VERIFY(window.currentCanvas()->cpu()); canvas=window.currentCanvas();
                QCOMPARE(canvas->viewport().center.x,saved.center.x); QCOMPARE(canvas->viewport().zoom,saved.zoom);
                // This is completion/lifecycle coverage, not a performance
                // gate: the layered scalar fallback is instrumented under ASan.
                idle=canvas->frameCount();const auto fallbackStart=monotonicNs();canvas->redraw();
                QTRY_VERIFY_WITH_TIMEOUT(canvas->frameCount()>idle,15000);
                qInfo()<<"CPU fallback completion observation ms:"<<(monotonicNs()-fallbackStart)/1000000;
                observeResources("fallback_frame","cpu",*resources);
            }
            // Focus-loss pointer recovery now exercises the real Move tool.
            // Retire its edit job and restore the original clean snapshot before
            // closing, so lifecycle teardown cannot open a dirty-file dialog.
            QTRY_VERIFY_WITH_TIMEOUT(!window.currentBusy(),10000);
            for(int edit=0;edit<16 && window.currentDocument()!=cleanDocument;++edit) {
                window.undoCurrent();
                QTRY_VERIFY_WITH_TIMEOUT(!window.currentBusy(),10000);
            }
            QVERIFY(window.currentDocument()==cleanDocument);QVERIFY(!window.currentDirty());
            window.close();
        }
        instance.destroy(); // Include instance teardown in the validation gate.
        observeResources("final_teardown",backend,*resources);
        QCOMPARE(validationErrors.load(),0);
    }
};
QTEST_MAIN(LifecycleTest)
#include "lifecycle_test.moc"
