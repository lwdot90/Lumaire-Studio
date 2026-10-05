#include "app/window.h"
#include "io/spill_store.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QInputDialog>
#include <QImage>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <cmath>
#include <functional>

using namespace compositor;
namespace {
QString buildDirectory;
std::shared_ptr<RuntimeResources> resourcesIn(const QString& path) {
    io::SpillLimits limits;limits.maxBytes=64*1024*1024;limits.maxPayloadBytes=2*1024*1024;limits.maxEntries=256;limits.maxIoOperations=1;
    return std::make_shared<RuntimeResources>(ResourceLimits::forMachine(8ull*1024*1024*1024,2),
        []{return MemorySample{4ull*1024*1024*1024,64ull*1024*1024};},std::make_shared<io::SpillStore>(path.toStdString(),limits));
}
QByteArray canonical(const engine::RasterSnapshot& raster) {
    QByteArray bytes;
    for(auto y=raster.extent.y;y<raster.extent.y+raster.extent.height;++y)
        for(auto x=raster.extent.x;x<raster.extent.x+raster.extent.width;++x)
            for(const auto value:engine::pack(raster.pixel(x,y))) {bytes.append(char(value&255));bytes.append(char(value>>8));}
    return bytes;
}
// Every dialog has a bounded driver; returning false rejects an unexpected one.
bool drive(QAction* action,const std::function<bool(QDialog*)>& configure) {
    if(!action || !action->isEnabled()) return false;
    bool success=false;QElapsedTimer elapsed;elapsed.start();QTimer timer;timer.setInterval(10);
    QObject::connect(&timer,&QTimer::timeout,[&] {
        auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());if(!dialog) return;
        if(elapsed.elapsed()>10000) {timer.stop();dialog->reject();return;}
        success=configure(dialog);if(!success) {timer.stop();dialog->reject();}
    });
    timer.start();action->trigger();timer.stop();return success;
}
bool acceptDouble(QDialog* dialog,double value) {
    auto* input=qobject_cast<QInputDialog*>(dialog);if(!input) return false;
    auto* field=input->findChild<QDoubleSpinBox*>();if(!field) return false;
    field->setValue(value);input->accept();return true;
}
bool resizeForm(QDialog* dialog,int width,int height,int filter) {
    if(dialog->objectName()!="imageResampleDialog") return false;
    auto* w=dialog->findChild<QSpinBox*>("resampleWidth");auto* h=dialog->findChild<QSpinBox*>("resampleHeight");
    auto* sampling=dialog->findChild<QComboBox*>("resampleFilter");auto* box=dialog->findChild<QDialogButtonBox*>();
    if(!w || !h || !sampling || !box) return false;
    w->setValue(width);h->setValue(height);sampling->setCurrentIndex(filter);
    QTest::mouseClick(box->button(QDialogButtonBox::Ok),Qt::LeftButton);return true;
}
}
class D2MaskResizeWorkflowTest final:public QObject {
    Q_OBJECT
private slots:
    void masksOnRevisableLayerNearestSaveReopen() {
        QTemporaryDir directory(QDir(buildDirectory).filePath("d2-mask-resize-XXXXXX"));QVERIFY(directory.isValid());
        QImage image(8,4,QImage::Format_RGB32);
        for(int y=0;y<4;++y) for(int x=0;x<8;++x) image.setPixel(x,y,qRgb(40+12*x,60+20*y,100));
        const auto path=directory.filePath("input.png");QVERIFY(image.save(path));
        MainWindow window(nullptr,true,{},resourcesIn(directory.path()));window.show();QSignalSpy finished(&window,&MainWindow::operationFinished);qsizetype operations=0;
#define COMPLETE(expression) do {++operations;expression;QTRY_COMPARE_WITH_TIMEOUT(finished.count(),operations,10000);QVERIFY2(finished.last()[0].toBool(),qPrintable(finished.last()[1].toString()));QVERIFY(!window.currentBusy());} while(false)
        COMPLETE(window.openPath(path));const auto imported=window.currentDocument();const auto original=imported->singleLayer().raster;const auto originalBytes=canonical(*original);
        COMPLETE(window.setSelectionCurrent(engine::Selection{engine::Extent{2,1,4,2}}));
        auto* featherSelection=window.findChild<QAction*>("featherSelectionAction");QVERIFY(featherSelection);
        COMPLETE(QVERIFY(drive(featherSelection,[](QDialog* dialog){return acceptDouble(dialog,1);} )));
        QCOMPARE(window.currentDocument()->selection->featherRadius,1.);
        QVERIFY(std::abs(window.currentDocument()->selection->coverage(1.5,2.5)-.15625f)<.00001f);
        auto* fromSelection=window.findChild<QAction*>("createSelectionMaskAction");QVERIFY(fromSelection);COMPLETE(fromSelection->trigger());
        const auto created=window.currentDocument();double expected=0;
        for(int x=0;x<8;++x) {const double t=.5+(1+(x+.5)/8.-2)/2;expected+=t*t*(3-2*t)/8;}
        QVERIFY(std::abs(created->singleLayer().mask->pixel(1,2).a-expected)<.0003);
        QVERIFY(created->singleLayer().mask->pixel(1,2).a>0 && created->singleLayer().mask->pixel(1,2).a<1);
        // The feather setting remains native-editable, independently of masks.
        const auto selectedProject=directory.filePath("selected.cproj");COMPLETE(window.saveCurrentTo(selectedProject));COMPLETE(window.openPath(selectedProject));
        QCOMPARE(window.currentDocument()->selection->featherRadius,1.);QCOMPARE(canonical(*window.currentDocument()->singleLayer().mask),canonical(*created->singleLayer().mask));
        COMPLETE(window.setSelectionCurrent({}));
        auto* exposure=window.findChild<QAction*>("revisableExposureAction");QVERIFY(exposure);bool configured=false;
        COMPLETE(QVERIFY(drive(exposure,[&](QDialog* dialog) {
            if(dialog->objectName()!="revisableExposureDialog") return false;
            auto* value=dialog->findChild<QDoubleSpinBox*>("revisableExposureValue");auto* box=dialog->findChild<QDialogButtonBox*>();if(!value || !box) return false;
            if(!configured) {if(!box->button(QDialogButtonBox::Ok)->isEnabled()) return true;value->setValue(1);configured=true;return true;}
            if(box->button(QDialogButtonBox::Ok)->isEnabled()) QTest::mouseClick(box->button(QDialogButtonBox::Ok),Qt::LeftButton);
            return true;
        })));
        const auto adjusted=window.currentDocument();QVERIFY(adjusted->singleLayer().adjustments);const auto base=adjusted->singleLayer().adjustments->source,cache=adjusted->singleLayer().raster;
        auto* brush=window.findChild<QAction*>("toolBrush");auto* maskContext=window.findChild<QCheckBox*>("paintMask");
        auto* maskMode=window.findChild<QComboBox*>("maskPaintMode");auto* diameter=window.findChild<QDoubleSpinBox*>("brushDiameter");
        auto* hardness=window.findChild<QDoubleSpinBox*>("brushHardness");auto* opacity=window.findChild<QDoubleSpinBox*>("brushOpacity");
        QVERIFY(brush);QVERIFY(maskContext);QVERIFY(maskMode);QVERIFY(diameter);QVERIFY(hardness);QVERIFY(opacity);
        QVERIFY(brush->isEnabled());brush->trigger();QVERIFY(maskContext->isChecked());maskMode->setCurrentIndex(1);diameter->setValue(2);hardness->setValue(100);opacity->setValue(50);
        auto* canvas=window.currentCanvas();QVERIFY(canvas);canvas->fit();QTRY_VERIFY_WITH_TIMEOUT(!canvas->captureCpuPresentation().isNull(),10000);
        const auto logical=canvas->viewport().toLogical({3.5,2.5});const QPoint point(qRound(logical.x),qRound(logical.y));
        const auto beforePaint=window.currentDocument();const auto oldAlpha=beforePaint->singleLayer().mask->pixel(3,2).a;
        COMPLETE(QTest::mousePress(canvas,Qt::LeftButton,{},point);QTest::mouseRelease(canvas,Qt::LeftButton,{},point));
        const auto painted=window.currentDocument();QVERIFY(std::abs(painted->singleLayer().mask->pixel(3,2).a-oldAlpha*.5f)<.0005f);
        QVERIFY(painted->singleLayer().raster==cache && painted->singleLayer().adjustments->source==base);
        window.undoCurrent();QVERIFY(window.currentDocument()==beforePaint);window.redoCurrent();QVERIFY(window.currentDocument()==painted);
        auto* featherMask=window.findChild<QAction*>("featherLayerMaskAction");QVERIFY(featherMask);
        COMPLETE(QVERIFY(drive(featherMask,[](QDialog* dialog){return acceptDouble(dialog,1);} )));
        const auto softened=window.currentDocument();const double w=std::exp(-4.5),denominator=(1+2*w)*(1+2*w);double blurred=0;
        for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x) blurred+=(x==0?1:w)*(y==0?1:w)*painted->singleLayer().mask->pixel(3+x,2+y).a/denominator;
        QVERIFY(std::abs(softened->singleLayer().mask->pixel(3,2).a-blurred)<.0006);
        QVERIFY(softened->singleLayer().raster==cache && softened->singleLayer().adjustments->source==base);
        window.undoCurrent();QVERIFY(window.currentDocument()==painted);window.redoCurrent();QVERIFY(window.currentDocument()==softened);
        auto* resize=window.findChild<QAction*>("resampleDocumentAction");QVERIFY(resize);
        COMPLETE(QVERIFY(drive(resize,[](QDialog* dialog){return resizeForm(dialog,4,2,0);} )));
        const auto resized=window.currentDocument();QCOMPARE(resized->width,4);QCOMPARE(resized->height,2);
        QCOMPARE(resized->singleLayer().adjustments->source->extent,engine::Extent({0,0,4,2}));
        QCOMPARE(resized->singleLayer().mask->extent,resized->singleLayer().raster->extent);
        for(int y=0;y<2;++y) for(int x=0;x<4;++x) {
            QCOMPARE(engine::pack(resized->singleLayer().adjustments->source->pixel(x,y)),engine::pack(base->pixel(2*x+1,2*y+1)));
            QCOMPARE(engine::pack(resized->singleLayer().mask->pixel(x,y)),engine::pack(softened->singleLayer().mask->pixel(2*x+1,2*y+1)));
        }
        QCOMPARE(canonical(*original),originalBytes);QCOMPARE(canonical(*base),originalBytes);
        window.undoCurrent();QVERIFY(window.currentDocument()==softened);window.redoCurrent();QVERIFY(window.currentDocument()==resized);
        const auto project=directory.filePath("edited.cproj"),pngPath=directory.filePath("edited.png");
        COMPLETE(window.saveCurrentTo(project));COMPLETE(window.exportCurrentTo(pngPath));COMPLETE(window.openPath(project));
        const auto reopened=window.currentDocument();QVERIFY(reopened->singleLayer().adjustments);
        QCOMPARE(reopened->singleLayer().adjustments->operations,resized->singleLayer().adjustments->operations);
        QCOMPARE(canonical(*reopened->singleLayer().adjustments->source),canonical(*resized->singleLayer().adjustments->source));
        QCOMPARE(canonical(*reopened->singleLayer().raster),canonical(*resized->singleLayer().raster));QCOMPARE(canonical(*reopened->singleLayer().mask),canonical(*resized->singleLayer().mask));
        const auto afterPath=directory.filePath("after.png");COMPLETE(window.exportCurrentTo(afterPath));QCOMPARE(QImage(afterPath),QImage(pngPath));
#undef COMPLETE
    }
    void lanczosPreservesConstantAndUndo() {
        QTemporaryDir directory(QDir(buildDirectory).filePath("d2-lanczos-XXXXXX"));QVERIFY(directory.isValid());
        QImage image(8,4,QImage::Format_RGB32);image.fill(QColor(64,96,128));const auto path=directory.filePath("constant.png");QVERIFY(image.save(path));
        MainWindow window(nullptr,true,{},resourcesIn(directory.path()));window.show();QSignalSpy finished(&window,&MainWindow::operationFinished);
        window.openPath(path);QTRY_COMPARE_WITH_TIMEOUT(finished.count(),1,10000);QVERIFY(finished.last()[0].toBool());
        const auto original=window.currentDocument();const auto expected=original->singleLayer().raster->pixel(0,0);
        auto* resize=window.findChild<QAction*>("resampleDocumentAction");QVERIFY(resize);
        QVERIFY(drive(resize,[](QDialog* dialog){return resizeForm(dialog,16,8,2);}));QTRY_COMPARE_WITH_TIMEOUT(finished.count(),2,10000);QVERIFY(finished.last()[0].toBool());
        const auto resized=window.currentDocument();QCOMPARE(resized->width,16);QCOMPARE(resized->height,8);
        QCOMPARE(resized->singleLayer().raster->extent,engine::Extent({0,0,16,8}));
        for(int y=0;y<8;++y) for(int x=0;x<16;++x) {
            const auto actual=resized->singleLayer().raster->pixel(x,y);
            QVERIFY(std::abs(actual.r-expected.r)<.0003 && std::abs(actual.g-expected.g)<.0003 && std::abs(actual.b-expected.b)<.0003);QCOMPARE(actual.a,1.f);
        }
        window.undoCurrent();QVERIFY(window.currentDocument()==original);window.redoCurrent();QVERIFY(window.currentDocument()==resized);
    }
};
int main(int argc,char** argv) {
    if(argc<2) {qCritical("Pass the existing disk-backed build directory");return 2;}
    buildDirectory=QDir(QString::fromLocal8Bit(argv[1])).absolutePath();QApplication app(argc,argv);D2MaskResizeWorkflowTest test;return QTest::qExec(&test,argc-1,argv+1);
}
#include "d2_mask_resize_workflow_test.moc"
