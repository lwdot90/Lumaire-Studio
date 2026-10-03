#include "app/window.h"
#include "core/editing_types.h"
#include "io/spill_store.h"
#include <QApplication>
#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QPainter>
#include <QRegion>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidgetItemIterator>
#include <QtTest>
#include <algorithm>

using namespace compositor;
namespace {
QString buildDirectory;
QByteArray pixels(const engine::DocumentPtr& document) {
    QByteArray result;
    const auto& raster=*document->singleLayer().raster;
    for(int y=0;y<document->height;++y) for(int x=0;x<document->width;++x)
        for(const auto value:engine::pack(raster.pixel(x,y))) {
            result.append(static_cast<char>(value&255));result.append(static_cast<char>(value>>8));
        }
    return result;
}
std::shared_ptr<RuntimeResources> resourcesIn(const QString& path) {
    io::SpillLimits spillLimits;spillLimits.maxBytes=64*1024*1024;
    spillLimits.maxPayloadBytes=2*1024*1024;spillLimits.maxEntries=256;spillLimits.maxIoOperations=1;
    auto spill=std::make_shared<io::SpillStore>(path.toStdString(),spillLimits);
    return std::make_shared<RuntimeResources>(ResourceLimits::forMachine(8ull*1024*1024*1024,2),
        []{return MemorySample{4ull*1024*1024*1024,64ull*1024*1024};},spill);
}
}

class PhotoWorkflowTest final:public QObject {
    Q_OBJECT
private slots:
    void reachableAdjustmentsSaveAndExport() {
        QTemporaryDir directory(QDir(buildDirectory).filePath("photo-workflow-XXXXXX"));
        QVERIFY(directory.isValid());
        QImage input(48,32,QImage::Format_RGB32);
        for(int y=0;y<input.height();++y) for(int x=0;x<input.width();++x) {
            const int level=32+x*191/(input.width()-1);
            input.setPixel(x,y,qRgb(level,level,level));
        }
        const auto inputPath=directory.filePath("gradient.png");QVERIFY(input.save(inputPath));
        MainWindow window(nullptr,true,{},resourcesIn(directory.path()));window.show();
        QSignalSpy finished(&window,&MainWindow::operationFinished);
        window.openPath(inputPath);QTRY_COMPARE_WITH_TIMEOUT(finished.count(),1,10000);
        QVERIFY2(finished.last()[0].toBool(),qPrintable(finished.last()[1].toString()));
        const auto imported=window.currentDocument();QVERIFY(imported);
        auto* dock=window.findChild<QDockWidget*>("photoEditingDock");QVERIFY(dock);
        dock->show();dock->raise();
        for(const auto* name:{"exposureAdjustmentActionButton","brightnessAdjustmentActionButton",
                "contrastAdjustmentActionButton","saturationAdjustmentActionButton",
                "levelsAdjustmentActionButton","curvesAdjustmentActionButton","colorBalanceAdjustmentActionButton"}) {
            auto* button=dock->findChild<QToolButton*>(name);QVERIFY2(button,name);
            QTRY_VERIFY_WITH_TIMEOUT(QRegion(button->rect()).subtracted(button->visibleRegion()).isEmpty(),1000);
            QVERIFY(button->isEnabled());
        }
        QList<QAction*> actions;
        for(const auto* name:{"levelsAdjustmentAction","curvesAdjustmentAction","colorBalanceAdjustmentAction"}) {
            auto* action=window.findChild<QAction*>(name);QVERIFY2(action,name);actions.append(action);
        }
        // Changing the target must gate the reachable UI without editing pixels.
        const auto rasterId=*window.activeLayer();window.addLayer(true);
        auto* tree=window.findChild<QTreeWidget*>("layerTree");QVERIFY(tree);
        for(auto* action:actions) QVERIFY(!action->isEnabled());
        QTreeWidgetItem* rasterItem=nullptr;
        for(QTreeWidgetItemIterator item(tree);*item;++item)
            if((*item)->data(0,Qt::UserRole).toString()==QString::fromStdString(rasterId.text())) rasterItem=*item;
        QVERIFY(rasterItem);tree->setCurrentItem(rasterItem);
        for(auto* action:actions) QVERIFY(action->isEnabled());
        window.undoCurrent();QVERIFY(window.currentDocument()==imported);
        qsizetype operations=finished.count();
        for(int kind=0;kind<actions.size();++kind) {
            auto* button=dock->findChild<QToolButton*>(actions[kind]->objectName()+"Button");QVERIFY(button);
            const auto before=window.currentDocument();const auto beforePixels=pixels(before);
            const bool beforeDirty=window.currentDirty();
            // Both Cancel and accepting defaults must preserve the revision,
            // operation count and history (the later single undo proves this).
            for(const bool cancel:{true,false}) {
                bool found=false;
                QTimer::singleShot(0,&window,[&] {
                    auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());
                    if(!dialog) return;
                    found=dialog->objectName()=="photoAdjustmentDialog";
                    auto* box=dialog->findChild<QDialogButtonBox*>();
                    if(!box) {dialog->reject();return;}
                    auto* choice=box->button(cancel?QDialogButtonBox::Cancel:QDialogButtonBox::Ok);
                    if(choice) QTest::mouseClick(choice,Qt::LeftButton);else dialog->reject();
                });
                QTest::mouseClick(button,Qt::LeftButton);QVERIFY(found);
                QTRY_VERIFY_WITH_TIMEOUT(!window.currentBusy(),1000);
                QCOMPARE(finished.count(),operations);QVERIFY(window.currentDocument()==before);
                QCOMPARE(pixels(window.currentDocument()),beforePixels);QCOMPARE(window.currentDirty(),beforeDirty);
            }
            bool configured=false;
            QTimer::singleShot(0,&window,[&,kind] {
                auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if(!dialog) return;
                if(kind==1) {
                    auto* preset=dialog->findChild<QComboBox*>("curvePreset");
                    auto* editor=dialog->findChild<QWidget*>("toneCurveEditor");
                    if(preset && editor) {
                        const auto index=preset->findText("Gentle contrast");
                        if(index>=0) {preset->setCurrentIndex(index);configured=true;}
                    }
                } else {
                    auto* field=dialog->findChild<QDoubleSpinBox*>(kind==0?"levelsInputBlack":"balanceWarmth");
                    if(field) {
                        field->setFocus();field->selectAll();QTest::keyClicks(field,kind==0?"24":"30");
                        QTest::keyClick(field,Qt::Key_Tab);configured=true;
                    }
                }
                auto* box=dialog->findChild<QDialogButtonBox*>();
                if(configured && box && box->button(QDialogButtonBox::Ok))
                    QTest::mouseClick(box->button(QDialogButtonBox::Ok),Qt::LeftButton);
                else dialog->reject();
            });
            ++operations;QTest::mouseClick(button,Qt::LeftButton);QVERIFY(configured);
            QTRY_COMPARE_WITH_TIMEOUT(finished.count(),operations,10000);
            QVERIFY2(finished.last()[0].toBool(),qPrintable(finished.last()[1].toString()));
            const auto adjusted=window.currentDocument();const auto adjustedPixels=pixels(adjusted);
            QVERIFY(adjusted!=before);QVERIFY(adjustedPixels!=beforePixels);QVERIFY(window.currentDirty());
            window.undoCurrent();QVERIFY(window.currentDocument()==before);QCOMPARE(pixels(window.currentDocument()),beforePixels);
            window.redoCurrent();QVERIFY(window.currentDocument()==adjusted);QCOMPARE(pixels(window.currentDocument()),adjustedPixels);
        }
#define COMPLETE(command) do { ++operations;command;QTRY_COMPARE_WITH_TIMEOUT(finished.count(),operations,10000); \
    QVERIFY2(finished.last()[0].toBool(),qPrintable(finished.last()[1].toString())); } while(false)
        const auto edited=window.currentDocument();const auto editedPixels=pixels(edited);
        const auto project=directory.filePath("edited.cproj"),pngPath=directory.filePath("edited.png"),jpegPath=directory.filePath("edited.jpg");
        COMPLETE(window.saveCurrentTo(project));QVERIFY(!window.currentDirty());
        COMPLETE(window.exportCurrentTo(pngPath));COMPLETE(window.exportCurrentTo(jpegPath));
        QImage png(pngPath),jpeg(jpegPath);QVERIFY(!png.isNull());QVERIFY(!jpeg.isNull());
        QCOMPARE(png.size(),input.size());QCOMPARE(jpeg.size(),input.size());
        qint64 jpegError=0;
        for(int y=0;y<input.height();++y) for(int x=0;x<input.width();++x) {
            const auto sample=edited->singleLayer().raster->pixel(x,y);
            const auto encode=[](float value){return qRound(255*std::clamp(engine::encodeSrgb(value),0.f,1.f));};
            const QColor expected(encode(sample.r),encode(sample.g),encode(sample.b));
            const auto actual=png.pixelColor(x,y);QCOMPARE(actual,expected);
            const auto lossy=jpeg.pixelColor(x,y);
            jpegError+=std::abs(lossy.red()-expected.red())+std::abs(lossy.green()-expected.green())+std::abs(lossy.blue()-expected.blue());
        }
        QVERIFY2(jpegError<qint64(input.width())*input.height()*3*8,"JPEG decode deviates excessively from the edited raster");
        COMPLETE(window.openPath(project));QCOMPARE(pixels(window.currentDocument()),editedPixels);QVERIFY(!window.currentDirty());
        const auto reopened=directory.filePath("reopened.png");COMPLETE(window.exportCurrentTo(reopened));
        QCOMPARE(QImage(reopened).convertToFormat(QImage::Format_RGBA8888),png.convertToFormat(QImage::Format_RGBA8888));
        const auto screenshot=qEnvironmentVariable("LUMAIRE_PHOTO_SCREENSHOT");
        if(!screenshot.isEmpty()) {
            QVERIFY(QDir::isAbsolutePath(screenshot));auto* canvas=window.currentCanvas();QVERIFY(canvas);
            const auto frame=canvas->frameCount();canvas->fit();
            QTRY_VERIFY_WITH_TIMEOUT(canvas->frameCount()>frame,10000);
            QTRY_VERIFY_WITH_TIMEOUT(!canvas->captureCpuPresentation().isNull(),10000);
            auto capture=window.grab();QVERIFY(!capture.isNull());
            // Widget capture plus accepted renderer image; not a desktop capture.
            QPainter painter(&capture);painter.drawImage(QRect(window.mapFromGlobal(canvas->mapToGlobal(QPoint(0,0))),canvas->size()),canvas->captureCpuPresentation());painter.end();
            QVERIFY(capture.save(screenshot));
        }
#undef COMPLETE
    }
};
int main(int argc,char** argv) {
    if(argc<2) {qCritical("Pass the existing disk-backed build directory");return 2;}
    buildDirectory=QDir(QString::fromLocal8Bit(argv[1])).absolutePath();
    QApplication app(argc,argv);PhotoWorkflowTest test;return QTest::qExec(&test,argc-1,argv+1);
}
#include "photo_workflow_test.moc"
