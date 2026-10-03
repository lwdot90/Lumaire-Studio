#include "app/window.h"
#include <QAction>
#include <QApplication>
#include <QSignalSpy>
#include <QTest>
#include <atomic>
#include <cmath>

using namespace compositor;
class EditingTransitionTest:public QObject {
    Q_OBJECT
private slots:
    void firstStrokeAfterBackendAndTabTransition() {
        // Exercise real native surfaces, not Qt's Wayland shm expose workaround.
        qunsetenv("QT_QTESTLIB_RUNNING");
        std::atomic<int> validationErrors=0;
        QVulkanInstance instance;
        QVERIFY2(instance.supportedLayers().contains("VK_LAYER_KHRONOS_validation"),
                 "Native acceptance requires the actual Vulkan validation layer");
        instance.setLayers({"VK_LAYER_KHRONOS_validation"});
        instance.setApiVersion(QVersionNumber(1,2));
        instance.installDebugOutputFilter(QVulkanInstance::DebugUtilsFilter([&](auto severity,auto,const void* data) {
            if(severity.testFlag(QVulkanInstance::ErrorSeverity)) {
                ++validationErrors;
                qWarning().noquote()<<static_cast<const VkDebugUtilsMessengerCallbackDataEXT*>(data)->pMessage;
            }
            return false;
        }));
        QVERIFY(instance.create());
        {
            MainWindow window(&instance,false,"Intel");window.show();
            auto* brush=window.findChild<QAction*>("toolBrush");
            auto* move=window.findChild<QAction*>("toolMove");
            QVERIFY(brush);QVERIFY(move);QVERIFY(move->isChecked());
            QSignalSpy operations(&window,&MainWindow::operationFinished);
            auto* canvas=window.currentCanvas();QVERIFY(canvas);
            connect(canvas,&Canvas::diagnostic,this,[](const QString& message){qInfo().noquote()<<message;});
            QTRY_VERIFY_WITH_TIMEOUT(canvas->frameCount()>0,10000);
            QVERIFY2(!canvas->cpu(),"Initial native Intel canvas unexpectedly fell back to CPU");
            const auto firstClean=window.currentDocument();
            brush->trigger();QVERIFY(brush->isChecked());
            QTRY_VERIFY_WITH_TIMEOUT(window.currentCanvas()->cpu(),10000);
            canvas=window.currentCanvas();
            QTRY_VERIFY_WITH_TIMEOUT(canvas->isExposed(),10000);
            const QPoint start(canvas->width()/2,canvas->height()/2);
            const QPoint finish=start+QPoint(12,0);
            const auto firstPoint=canvas->viewport().documentPixel(
                static_cast<int>(std::floor(start.x()*canvas->viewport().dpr)),
                static_cast<int>(std::floor(start.y()*canvas->viewport().dpr)));
            QTest::mousePress(canvas,Qt::LeftButton,{},start);
            QTest::mouseMove(canvas,finish);
            QTest::mouseRelease(canvas,Qt::LeftButton,{},finish);
            QTRY_COMPARE_WITH_TIMEOUT(operations.count(),1,10000);
            QVERIFY2(operations.last()[0].toBool(),qPrintable(operations.last()[1].toString()));
            QVERIFY(window.currentDocument()!=firstClean);QVERIFY(window.currentDirty());
            const auto firstPixel=window.currentDocument()->singleLayer().raster->pixel(
                static_cast<std::int64_t>(std::floor(firstPoint.x)),static_cast<std::int64_t>(std::floor(firstPoint.y)));
            QVERIFY2(firstPixel.a>0,"First stroke after GPU-to-CPU transition lost its pixels");
            window.undoCurrent();QVERIFY(window.currentDocument()==firstClean);QVERIFY(!window.currentDirty());

            // Brush remains selected while a newly created GPU canvas is remapped
            // to CPU. The first gesture on that replacement must also survive.
            window.addDocument();QCOMPARE(window.tabs()->count(),2);QVERIFY(brush->isChecked());
            const auto secondClean=window.currentDocument();
            QVERIFY2(!window.currentCanvas()->cpu(),"New tab did not begin with its configured GPU canvas");
            QTRY_VERIFY_WITH_TIMEOUT(window.currentCanvas()->cpu(),10000);
            canvas=window.currentCanvas();
            QTRY_VERIFY_WITH_TIMEOUT(canvas->isExposed(),10000);
            const QPoint secondStart(canvas->width()/2,canvas->height()/2);
            const auto secondPoint=canvas->viewport().documentPixel(
                static_cast<int>(std::floor(secondStart.x()*canvas->viewport().dpr)),
                static_cast<int>(std::floor(secondStart.y()*canvas->viewport().dpr)));
            QTest::mousePress(canvas,Qt::LeftButton,{},secondStart);
            QTest::mouseMove(canvas,secondStart+QPoint(12,0));
            QTest::mouseRelease(canvas,Qt::LeftButton,{},secondStart+QPoint(12,0));
            QTRY_COMPARE_WITH_TIMEOUT(operations.count(),2,10000);
            QVERIFY2(operations.last()[0].toBool(),qPrintable(operations.last()[1].toString()));
            QVERIFY(window.currentDocument()!=secondClean);QVERIFY(window.currentDirty());
            const auto secondPixel=window.currentDocument()->singleLayer().raster->pixel(
                static_cast<std::int64_t>(std::floor(secondPoint.x)),static_cast<std::int64_t>(std::floor(secondPoint.y)));
            QVERIFY2(secondPixel.a>0,"First stroke after active-Brush tab transition lost its pixels");
            window.undoCurrent();QVERIFY(window.currentDocument()==secondClean);QVERIFY(!window.currentDirty());
            window.tabs()->setCurrentIndex(0);QVERIFY(window.currentDocument()==firstClean);QVERIFY(!window.currentDirty());
            QTRY_VERIFY_WITH_TIMEOUT(!window.currentBusy(),10000);
            window.close();
        }
        instance.destroy();
        QCOMPARE(validationErrors.load(),0);
    }
};
QTEST_MAIN(EditingTransitionTest)
#include "editing_transition_test.moc"
