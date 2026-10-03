#include "app/window.h"
#include "io/spill_store.h"
#include <QApplication>
#include <QAction>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QTimer>
#include <functional>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QPointer>
#include <cmath>
#include <numbers>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QtTest>

using namespace compositor;
namespace {
QString buildDirectory;
QByteArray canonical(const engine::RasterSnapshot& raster) {
    QByteArray bytes;
    for(auto y=raster.extent.y;y<raster.extent.y+raster.extent.height;++y)
        for(auto x=raster.extent.x;x<raster.extent.x+raster.extent.width;++x)
            for(auto sample:engine::pack(raster.pixel(x,y))) {
                bytes.append(char(sample&255));bytes.append(char(sample>>8));
            }
    return bytes;
}
QAction* command(MainWindow& window,const QString& name,const QKeySequence& shortcut={}) {
    for(auto* action:window.findChildren<QAction*>())
        if((!name.isEmpty() && action->objectName()==name) || (!shortcut.isEmpty() && action->shortcut()==shortcut)) return action;
    return nullptr;
}
// A bounded timer drives the real Qt dialog. An unexpected dialog is rejected
// immediately; absent dialogs cannot leave an unbounded nested event loop.
bool dialogCommand(QAction* action,const std::function<bool(QDialog*)>& configure) {
    if(!action || !action->isEnabled()) return false;
    bool handled=false,success=false,configuring=false;
    QPointer<QDialog> driven;
    QTimer driver;driver.setInterval(10);
    QElapsedTimer elapsed;elapsed.start();
    QObject::connect(&driver,&QTimer::timeout,[&] {
        if(configuring) return;
        auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if(dialog && driven!=dialog) {
            driven=dialog;handled=true;configuring=true;success=configure(dialog);configuring=false;
            if(!success) dialog->reject();
        }
        if(elapsed.elapsed()>3000) {
            if(dialog) dialog->reject();
            driver.stop();
        }
    });
    driver.start();action->trigger();driver.stop();return handled && success;
}
bool acceptForm(QDialog* dialog,const QString& name,const QList<QPair<QString,double>>& fields) {
    if(dialog->objectName()!=name || !QTest::qWaitForWindowActive(dialog,1000)) return false;
    for(const auto& field:fields) {
        auto* control=dialog->findChild<QWidget*>(field.first);if(!control) return false;
        auto* editor=control->findChild<QLineEdit*>();if(!editor) return false;
        QTest::mouseClick(editor,Qt::LeftButton);
        if(!QTest::qWaitFor([editor]{return editor->hasFocus();},500)) return false;
        QTest::keyClick(editor,Qt::Key_A,Qt::ControlModifier);
        QTest::keyClicks(editor,QString::number(field.second));
        if(editor->text()!=QString::number(field.second)) return false;
        QTest::keyClick(editor,Qt::Key_Tab);
        if(auto* real=qobject_cast<QDoubleSpinBox*>(control)) {
            if(!QTest::qWaitFor([&]{return std::abs(real->value()-field.second)<1e-9;},500)) {qWarning()<<"Uncommitted control"<<field.first<<real->value()<<editor->text()<<editor->hasFocus();return false;}
        } else if(auto* integer=qobject_cast<QSpinBox*>(control)) {
            if(!QTest::qWaitFor([&]{return integer->value()==int(field.second);},500)) {qWarning()<<"Uncommitted control"<<field.first<<integer->value()<<editor->text()<<editor->hasFocus();return false;}
        } else return false;
    }
    auto* box=dialog->findChild<QDialogButtonBox*>();if(!box || !box->button(QDialogButtonBox::Ok)) return false;
    QTest::mouseClick(box->button(QDialogButtonBox::Ok),Qt::LeftButton);return true;
}
bool fileDialog(QDialog* dialog,const QString& path) {
    auto* file=qobject_cast<QFileDialog*>(dialog);if(!file) return false;
    // Select the private fixture directory before typing its filename. Path
    // completion can otherwise navigate during slash-by-slash synthetic input.
    const QFileInfo target(path);file->setDirectory(target.absolutePath());
    auto* edit=file->findChild<QLineEdit*>("fileNameEdit");if(!edit) return false;
    QTest::mouseClick(edit,Qt::LeftButton);QTest::keyClick(edit,Qt::Key_A,Qt::ControlModifier);
    QTest::keyClicks(edit,target.fileName());
    if(auto* popup=QApplication::activePopupWidget()) QTest::keyClick(popup,Qt::Key_Escape);
    if(edit->text()!=target.fileName()) return false;
    auto* box=file->findChild<QDialogButtonBox*>();if(!box) return false;
    auto* button=box->button(file->acceptMode()==QFileDialog::AcceptSave ? QDialogButtonBox::Save : QDialogButtonBox::Open);
    if(!button || !button->isEnabled()) return false;
    QTest::mouseClick(button,Qt::LeftButton);
    return !file->isVisible() && file->result()==QDialog::Accepted;
}
}
class RasterDeliveryTest final:public QObject {
    Q_OBJECT
private slots:
    void completeUiWorkflow() {
        QTemporaryDir directory(QDir(buildDirectory).filePath("raster-delivery-XXXXXX"));QVERIFY(directory.isValid());
        QImage input(64,48,QImage::Format_RGB32);
        for(int y=0;y<48;++y) for(int x=0;x<64;++x) input.setPixel(x,y,qRgb(40+x*2,50+y*3,80));
        const auto source=directory.filePath("input.png");QVERIFY(input.save(source));
        io::SpillLimits spillLimits;spillLimits.maxBytes=64*1024*1024;spillLimits.maxPayloadBytes=2*1024*1024;spillLimits.maxEntries=256;spillLimits.maxIoOperations=1;
        auto spill=std::make_shared<io::SpillStore>(directory.path().toStdString(),spillLimits);
        auto resources=std::make_shared<RuntimeResources>(ResourceLimits::forMachine(8ull*1024*1024*1024,2),[]{return MemorySample{4ull*1024*1024*1024,64ull*1024*1024};},spill);
        MainWindow window(nullptr,true,{},resources);window.show();
        QSignalSpy finished(&window,&MainWindow::operationFinished);qsizetype operations=0;
#define COMPLETE(expression) do { ++operations;expression;QTRY_COMPARE_WITH_TIMEOUT(finished.count(),operations,10000);QVERIFY2(finished.last()[0].toBool(),qPrintable(finished.last()[1].toString()));QVERIFY(!window.currentBusy()); } while(false)
        COMPLETE(QVERIFY(dialogCommand(command(window,{},QKeySequence::Open),[&](QDialog* dialog){return fileDialog(dialog,source);} )));
        auto* canvas=window.currentCanvas();QVERIFY(canvas);canvas->fit();
        QTRY_VERIFY_WITH_TIMEOUT(!canvas->captureCpuPresentation().isNull(),10000);
        const auto imported=window.currentDocument();const auto original=canonical(*imported->singleLayer().raster);
        auto* diameter=window.findChild<QDoubleSpinBox*>("brushDiameter");auto* hardness=window.findChild<QDoubleSpinBox*>("brushHardness");QVERIFY(diameter);QVERIFY(hardness);
        auto gesture=[&](engine::Coordinate first,engine::Coordinate last) {
            const auto a=canvas->viewport().toLogical({first.x,first.y}),b=canvas->viewport().toLogical({last.x,last.y});
            QTest::mousePress(canvas,Qt::LeftButton,{},QPoint(qRound(a.x),qRound(a.y)));
            QTest::mouseMove(canvas,QPoint(qRound(b.x),qRound(b.y)),1);
            QTest::mouseRelease(canvas,Qt::LeftButton,{},QPoint(qRound(b.x),qRound(b.y)));
        };
        auto* brush=command(window,"toolBrush");QVERIFY(brush);brush->trigger();diameter->setValue(8);hardness->setValue(100);
        COMPLETE(gesture({24,24},{32,24}));const auto painted=window.currentDocument();QVERIFY(canonical(*painted->singleLayer().raster)!=original);
        auto* erase=command(window,"toolErase");QVERIFY(erase);erase->trigger();
        COMPLETE(gesture({28,24},{28,24}));const auto erased=window.currentDocument();QVERIFY(erased->singleLayer().raster->pixel(28,24).a<painted->singleLayer().raster->pixel(28,24).a);
        auto* undo=command(window,{},QKeySequence::Undo);auto* redo=command(window,{},QKeySequence::Redo);QVERIFY(undo);QVERIFY(redo);
        undo->trigger();QVERIFY(window.currentDocument()==painted);redo->trigger();QVERIFY(window.currentDocument()==erased);
        auto* selection=command(window,"toolRectangleSelection");QVERIFY(selection);selection->trigger();
        COMPLETE(gesture({8,8},{48,36}));QVERIFY(window.currentDocument()->selection);
        auto* mask=command(window,"createSelectionMaskAction");QVERIFY(mask);COMPLETE(mask->trigger());QVERIFY(window.currentDocument()->singleLayer().mask);
        auto* deselect=command(window,"deselectAction");QVERIFY(deselect);COMPLETE(deselect->trigger());
        const auto beforeTransform=window.currentDocument();
        COMPLETE(QVERIFY(dialogCommand(command(window,"transformLayerAction"),[](QDialog* dialog){return acceptForm(dialog,"layerTransformDialog",{{"transformDx",2},{"transformDy",3},{"transformScaleX",90},{"transformScaleY",85},{"transformRotation",12}});} )));
        const auto& matrix=window.currentDocument()->singleLayer().localToDocument;
        const double angle=12*std::numbers::pi/180;
        QVERIFY(std::abs(matrix.a-.9*std::cos(angle))<1e-12);
        QVERIFY(std::abs(matrix.b-.9*std::sin(angle))<1e-12);
        QVERIFY(std::abs(matrix.c+.85*std::sin(angle))<1e-12);
        QVERIFY(std::abs(matrix.d-.85*std::cos(angle))<1e-12);
        const auto oldCenter=beforeTransform->singleLayer().localToDocument.map({32,24});
        const auto newCenter=matrix.map({32,24});
        QVERIFY(std::abs(newCenter.x-oldCenter.x-2)<1e-12);
        QVERIFY(std::abs(newCenter.y-oldCenter.y-3)<1e-12);
        QCOMPARE(canonical(*window.currentDocument()->singleLayer().raster),canonical(*erased->singleLayer().raster));
        COMPLETE(QVERIFY(dialogCommand(command(window,"cropDocumentAction"),[](QDialog* dialog){return acceptForm(dialog,"geometryBoundsDialog",{{"boundsLeft",4},{"boundsTop",4},{"boundsWidth",56},{"boundsHeight",40}});} )));
        QCOMPARE(window.currentDocument()->width,56);QCOMPARE(window.currentDocument()->height,40);
        COMPLETE(QVERIFY(dialogCommand(command(window,"resizeDocumentAction"),[](QDialog* dialog){return acceptForm(dialog,"documentResizeDialog",{{"resizeWidth",48},{"resizeHeight",32}});} )));
        QCOMPARE(window.currentDocument()->width,48);QCOMPARE(window.currentDocument()->height,32);
        COMPLETE(QVERIFY(dialogCommand(command(window,"exposureAdjustmentAction"),[](QDialog* dialog){auto* exposure=qobject_cast<QInputDialog*>(dialog);if(!exposure)return false;auto* field=exposure->findChild<QDoubleSpinBox*>();if(!field)return false;field->setFocus();field->selectAll();QTest::keyClicks(field,"0.5");QTest::keyClick(field,Qt::Key_Tab);exposure->accept();return true;} )));
        const auto edited=window.currentDocument();const auto rasterBytes=canonical(*edited->singleLayer().raster),maskBytes=canonical(*edited->singleLayer().mask);const auto placement=edited->singleLayer().localToDocument;
        const auto project=directory.filePath("edited.cproj"),pngPath=directory.filePath("edited.png"),jpegPath=directory.filePath("edited.jpg");
        COMPLETE(QVERIFY(dialogCommand(command(window,{},QKeySequence::SaveAs),[&](QDialog* dialog){return fileDialog(dialog,project);} )));QVERIFY(!window.currentDirty());
        COMPLETE(QVERIFY(dialogCommand(command(window,"exportImage"),[&](QDialog* dialog){return fileDialog(dialog,pngPath);} )));
        // JPEG adds its quality dialog after the file chooser closes.
        COMPLETE(QVERIFY(dialogCommand(command(window,"exportImage"),[&](QDialog* dialog){if(qobject_cast<QFileDialog*>(dialog))return fileDialog(dialog,jpegPath);auto* quality=qobject_cast<QInputDialog*>(dialog);if(!quality)return false;quality->setIntValue(95);quality->accept();return true;} )));
        const QImage png(pngPath),jpeg(jpegPath);QVERIFY(!png.isNull());QVERIFY(!jpeg.isNull());QCOMPARE(png.size(),QSize(48,32));QCOMPARE(jpeg.size(),png.size());
        bool visible=false,transparent=false;for(int y=0;y<png.height();++y)for(int x=0;x<png.width();++x){visible|=png.pixelColor(x,y).alpha()>0;transparent|=png.pixelColor(x,y).alpha()==0;QCOMPARE(jpeg.pixelColor(x,y).alpha(),255);}QVERIFY(visible);QVERIFY(transparent);
        COMPLETE(QVERIFY(dialogCommand(command(window,{},QKeySequence::Open),[&](QDialog* dialog){return fileDialog(dialog,project);} )));
        const auto reopened=window.currentDocument();QCOMPARE(canonical(*reopened->singleLayer().raster),rasterBytes);QVERIFY(reopened->singleLayer().mask);QCOMPARE(canonical(*reopened->singleLayer().mask),maskBytes);QVERIFY(reopened->singleLayer().localToDocument==placement);QCOMPARE(reopened->width,48);QCOMPARE(reopened->height,32);QVERIFY(!window.currentDirty());
        const auto afterPath=directory.filePath("after.png");COMPLETE(QVERIFY(dialogCommand(command(window,"exportImage"),[&](QDialog* dialog){return fileDialog(dialog,afterPath);} )));
        QCOMPARE(QImage(afterPath).convertToFormat(QImage::Format_RGBA8888),png.convertToFormat(QImage::Format_RGBA8888));
        canvas=window.currentCanvas();const auto frame=canvas->frameCount();canvas->fit();QTRY_VERIFY_WITH_TIMEOUT(canvas->frameCount()>frame,10000);QTRY_VERIFY_WITH_TIMEOUT(!canvas->captureCpuPresentation().isNull(),10000);
#undef COMPLETE
    }
};
int main(int argc,char** argv) {
    if(argc<2){qCritical("Pass an existing disk-backed build directory");return 2;}
    buildDirectory=QDir(QString::fromLocal8Bit(argv[1])).absolutePath();
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);QApplication app(argc,argv);RasterDeliveryTest test;return QTest::qExec(&test,argc-1,argv+1);
}
#include "raster_delivery_test.moc"
