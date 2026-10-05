#include "app/window.h"
#include "io/spill_store.h"
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <algorithm>
#include <cmath>

using namespace compositor;
namespace {
QString buildDirectory;
QByteArray canonical(const engine::RasterSnapshot& raster) {
    QByteArray result;
    for(auto y=raster.extent.y;y<raster.extent.y+raster.extent.height;++y)
        for(auto x=raster.extent.x;x<raster.extent.x+raster.extent.width;++x)
            for(auto value:engine::pack(raster.pixel(x,y))) {
                result.append(char(value&255));result.append(char(value>>8));
            }
    return result;
}
std::shared_ptr<RuntimeResources> resourcesIn(const QString& path) {
    io::SpillLimits limits;limits.maxBytes=64*1024*1024;limits.maxPayloadBytes=2*1024*1024;
    limits.maxEntries=256;limits.maxIoOperations=1;
    auto spill=std::make_shared<io::SpillStore>(path.toStdString(),limits);
    return std::make_shared<RuntimeResources>(ResourceLimits::forMachine(8ull*1024*1024*1024,2),
        []{return MemorySample{4ull*1024*1024*1024,64ull*1024*1024};},spill);
}
}
class RevisablePhotoWorkflowTest final:public QObject {
    Q_OBJECT
private slots:
    void retainedSourcePreviewRevisionAndExport() {
        QTemporaryDir directory(QDir(buildDirectory).filePath("revisable-photo-XXXXXX"));QVERIFY(directory.isValid());
        QImage input(12,8,QImage::Format_RGB32);input.fill(QColor(72,104,136));
        const auto inputPath=directory.filePath("source.png");QVERIFY(input.save(inputPath));
        MainWindow window(nullptr,true,{},resourcesIn(directory.path()));window.show();
        QSignalSpy finished(&window,&MainWindow::operationFinished);
        window.openPath(inputPath);QTRY_COMPARE_WITH_TIMEOUT(finished.count(),1,10000);
        QVERIFY2(finished.last()[0].toBool(),qPrintable(finished.last()[1].toString()));
        const auto original=window.currentDocument();QVERIFY(original);
        const bool originalDirty=window.currentDirty();
        const auto source=original->singleLayer().raster;
        const auto sourcePixel=source->pixel(0,0);
        auto* action=window.findChild<QAction*>("revisableExposureAction");QVERIFY(action);QVERIFY(action->isEnabled());
        const auto captureDirectory=qEnvironmentVariable("LUMAIRE_D2_CAPTURE_DIR");
        if(!captureDirectory.isEmpty()) {
            QVERIFY(QDir::isAbsolutePath(captureDirectory));QVERIFY(QDir().mkpath(captureDirectory));
        }
        const auto drive=[&](double expectedInitial,double next,bool cancel) {
            const auto before=window.currentDocument();const auto beforeDirty=window.currentDirty();
            bool configured=false,done=false;QString error;
            QTimer timer;timer.setInterval(10);
            QElapsedTimer elapsed;elapsed.start();
            connect(&timer,&QTimer::timeout,&window,[&] {
                auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if(!dialog) return;
                if(elapsed.elapsed()>10000) {error="Exposure preview timed out";dialog->reject();return;}
                if(dialog->objectName()!="revisableExposureDialog") {error="Wrong dialog";dialog->reject();return;}
                auto* value=dialog->findChild<QDoubleSpinBox*>("revisableExposureValue");
                auto* buttons=dialog->findChild<QDialogButtonBox*>("revisableExposureButtons");
                if(!value || !buttons) {error="Missing exposure controls";dialog->reject();return;}
                if(window.currentDocument()!=before || window.currentDirty()!=beforeDirty) {error="Preview changed document history";dialog->reject();return;}
                auto* ok=buttons->button(QDialogButtonBox::Ok);
                if(!configured) {
                    if(!ok->isEnabled()) return;
                    if(value->value()!=expectedInitial) {error="Saved exposure value was not restored";dialog->reject();return;}
                    value->setFocus();value->selectAll();QTest::keyClicks(value,QString::number(next));QTest::keyClick(value,Qt::Key_Tab);
                    configured=true;return;
                }
                if(!ok->isEnabled()) return;
                if(!captureDirectory.isEmpty() && expectedInitial==0 && next==1 && !cancel) {
                    const QDir capture(captureDirectory);
                    if(!dialog->grab().save(capture.filePath("revisable-exposure-dialog.png")) ||
                       !window.grab().save(capture.filePath("revisable-exposure-workspace.png"))) {
                        error="Could not save ready preview screenshots";dialog->reject();return;
                    }
                }
                done=true;timer.stop();QTest::mouseClick(buttons->button(cancel?QDialogButtonBox::Cancel:QDialogButtonBox::Ok),Qt::LeftButton);
            });
            timer.start();action->trigger();timer.stop();
            if(!done && error.isEmpty()) error="Dialog closed before its completed preview";
            return error;
        };
        auto error=drive(0,1,false);QVERIFY2(error.isEmpty(),qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(),2,10000);QVERIFY(finished.last()[0].toBool());
        const auto plus=window.currentDocument();QVERIFY(plus!=original);
        QVERIFY(plus->singleLayer().adjustments);QVERIFY(plus->singleLayer().adjustments->source==source);
        const auto plusPixel=plus->singleLayer().raster->pixel(0,0);
        QCOMPARE(engine::pack(plusPixel),engine::pack({sourcePixel.r*2,sourcePixel.g*2,sourcePixel.b*2,sourcePixel.a}));
        window.undoCurrent();QVERIFY(window.currentDocument()==original);QCOMPARE(window.currentDirty(),originalDirty);
        window.redoCurrent();QVERIFY(window.currentDocument()==plus);
        const auto project=directory.filePath("retained.cproj");window.saveCurrentTo(project);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(),3,10000);QVERIFY(finished.last()[0].toBool());QVERIFY(!window.currentDirty());
        window.openPath(project);QTRY_COMPARE_WITH_TIMEOUT(finished.count(),4,10000);QVERIFY(finished.last()[0].toBool());
        const auto reopened=window.currentDocument();QVERIFY(reopened->singleLayer().adjustments);
        QCOMPARE(engine::pack(reopened->singleLayer().adjustments->source->pixel(0,0)),engine::pack(sourcePixel));
        QCOMPARE(engine::pack(reopened->singleLayer().raster->pixel(0,0)),engine::pack(plusPixel));
        error=drive(1,-1,false);QVERIFY2(error.isEmpty(),qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(),5,10000);QVERIFY(finished.last()[0].toBool());
        const auto minus=window.currentDocument();const auto minusPixel=minus->singleLayer().raster->pixel(0,0);
        QCOMPARE(engine::pack(minusPixel),engine::pack({sourcePixel.r*.5f,sourcePixel.g*.5f,sourcePixel.b*.5f,sourcePixel.a}));
        window.undoCurrent();QVERIFY(window.currentDocument()==reopened);window.redoCurrent();QVERIFY(window.currentDocument()==minus);
        const auto count=finished.count();error=drive(-1,2,true);QVERIFY2(error.isEmpty(),qPrintable(error));
        QCOMPARE(finished.count(),count);QVERIFY(window.currentDocument()==minus);
        QCOMPARE(engine::pack(window.currentDocument()->singleLayer().raster->pixel(0,0)),engine::pack(minusPixel));
        window.undoCurrent();QVERIFY(window.currentDocument()==reopened);window.redoCurrent();QVERIFY(window.currentDocument()==minus);
        const auto pngPath=directory.filePath("minus.png");window.exportCurrentTo(pngPath);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(),count+1,10000);QVERIFY(finished.last()[0].toBool());
        const QImage output(pngPath);QVERIFY(!output.isNull());QCOMPARE(output.size(),input.size());
        const auto encode=[](float channel){return qRound(255*std::clamp(engine::encodeSrgb(channel),0.f,1.f));};
        const QColor expected(encode(minusPixel.r),encode(minusPixel.g),encode(minusPixel.b));
        for(int y=0;y<output.height();++y) for(int x=0;x<output.width();++x) QCOMPARE(output.pixelColor(x,y),expected);
        // The session cancel signal must invalidate a modal draft as well as
        // ordinary jobs. Click invokes the real statusbar control's signal;
        // that background control is intentionally inaccessible to physical
        // pointer input while the modal dialog owns the native surface.
        auto* cancelOperation=window.findChild<QPushButton*>("cancelOperationButton");QVERIFY(cancelOperation);
        const auto beforeSessionCancel=window.currentDocument();const auto beforeSessionDirty=window.currentDirty();
        const auto beforeSessionCount=finished.count();bool cancelSent=false;
        QTimer cancelTimer;cancelTimer.setInterval(10);QElapsedTimer cancelElapsed;cancelElapsed.start();
        connect(&cancelTimer,&QTimer::timeout,&window,[&] {
            auto* modal=qobject_cast<QDialog*>(QApplication::activeModalWidget());if(!modal) return;
            if(cancelElapsed.elapsed()>10000) {modal->reject();cancelTimer.stop();return;}
            if(modal->objectName()!="revisableExposureDialog") {modal->reject();cancelTimer.stop();return;}
            if(cancelOperation->isEnabled()) {cancelSent=true;cancelTimer.stop();cancelOperation->click();}
        });
        cancelTimer.start();action->trigger();cancelTimer.stop();QVERIFY(cancelSent);
        QVERIFY(!window.currentBusy());QVERIFY(window.currentDocument()==beforeSessionCancel);
        QCOMPARE(window.currentDirty(),beforeSessionDirty);QCOMPARE(finished.count(),beforeSessionCount);
        const auto adjustedBytes=canonical(*beforeSessionCancel->singleLayer().raster);
        for(const auto* name:{"exposureAdjustmentAction","toolBrush","toolErase"}) {
            auto* disabled=window.findChild<QAction*>(name);QVERIFY(disabled);QVERIFY(!disabled->isEnabled());
        }
        for(const auto* name:{"toolMove","transformLayerAction","cropDocumentAction","createRevealMaskAction"}) {
            auto* available=window.findChild<QAction*>(name);QVERIFY(available);QVERIFY(available->isEnabled());
        }
        auto* selectionTool=window.findChild<QAction*>("toolRectangleSelection");QVERIFY(selectionTool);
        selectionTool->trigger();QVERIFY(selectionTool->isChecked());
        for(const auto key:{Qt::Key_B,Qt::Key_E}) {
            QTest::keyClick(window.currentCanvas(),key);QCoreApplication::processEvents();
            QVERIFY(selectionTool->isChecked());
        }
        for(const auto* name:{"toolBrush","toolErase"}) {
            auto* painting=window.findChild<QAction*>(name);QVERIFY(painting);
            QVERIFY(!painting->isEnabled());QVERIFY(!painting->isChecked());
        }
        auto* moveTool=window.findChild<QAction*>("toolMove");QVERIFY(moveTool);moveTool->trigger();
        auto* rasterize=window.findChild<QAction*>("rasterizeAdjustmentsAction");QVERIFY(rasterize);QVERIFY(rasterize->isEnabled());
        rasterize->trigger();QTRY_COMPARE_WITH_TIMEOUT(finished.count(),beforeSessionCount+1,10000);QVERIFY(finished.last()[0].toBool());
        const auto detached=window.currentDocument();QVERIFY(!detached->singleLayer().adjustments);
        QCOMPARE(canonical(*detached->singleLayer().raster),adjustedBytes);
        for(const auto* name:{"exposureAdjustmentAction","toolBrush","toolErase"}) {
            auto* enabled=window.findChild<QAction*>(name);QVERIFY(enabled);QVERIFY(enabled->isEnabled());
        }
        window.undoCurrent();QVERIFY(window.currentDocument()==beforeSessionCancel);QVERIFY(window.currentDocument()->singleLayer().adjustments);
        QCOMPARE(canonical(*window.currentDocument()->singleLayer().raster),adjustedBytes);
        window.redoCurrent();QVERIFY(window.currentDocument()==detached);QVERIFY(!window.currentDocument()->singleLayer().adjustments);
        QCOMPARE(canonical(*window.currentDocument()->singleLayer().raster),adjustedBytes);
        if(!captureDirectory.isEmpty()) {
            const QDir capture(captureDirectory);
            // Refuse replacement: callers supply a fresh private build directory.
            QVERIFY(QFile::copy(project,capture.filePath("revisable-exposure-plus-one.cproj")));
            QVERIFY(QFile::copy(pngPath,capture.filePath("revisable-exposure-minus-one.png")));
        }
    }
};
int main(int argc,char** argv) {
    if(argc<2) {qCritical("Pass the existing disk-backed build directory");return 2;}
    buildDirectory=QDir(QString::fromLocal8Bit(argv[1])).absolutePath();
    QApplication app(argc,argv);RevisablePhotoWorkflowTest test;return QTest::qExec(&test,argc-1,argv+1);
}
#include "revisable_photo_workflow_test.moc"
