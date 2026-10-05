#include "app/window.h"
#include "core/retouch.h"
#include "core/spill_coordinator.h"
#include "io/spill_store.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidgetItemIterator>
#include <QtConcurrent>
#include <QtTest>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>

using namespace compositor;
namespace {
QString buildDirectory;
struct Digest {QByteArray bytes;QString error;};
Digest rasterDigest(const std::shared_ptr<const engine::RasterSnapshot>& raster,const std::shared_ptr<RuntimeResources>& resources) {
    try {
        auto permit=resources->compute.acquire(WorkScheduler::Priority::Processing,true);
        if(!permit) throw std::runtime_error("Digest canceled");
        auto charge=resources->memory->require(256*256*8);charge.commit();
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(QByteArray::number(qlonglong(raster->extent.x))+","+QByteArray::number(qlonglong(raster->extent.y))+","+QByteArray::number(qlonglong(raster->extent.width))+","+QByteArray::number(qlonglong(raster->extent.height)));
        for(auto sample:raster->defaultValue) {const char bytes[]{char(sample&255),char(sample>>8)};hash.addData(QByteArrayView(bytes,2));}
        for(const auto& [coordinate,tile]:raster->tiles) {
            hash.addData(QByteArray::number(qlonglong(coordinate.x))+","+QByteArray::number(qlonglong(coordinate.y))+","+QByteArray::number(tile->width())+","+QByteArray::number(tile->height()));
            const auto lease=tile->read();const auto bytes=lease.canonicalBytes();hash.addData(QByteArrayView(reinterpret_cast<const char*>(bytes.data()),qsizetype(bytes.size())));
        }
        return {hash.result(),{}};
    } catch(const std::exception& error) {return {{},QString::fromUtf8(error.what())};}
}
QString driveDialog(MainWindow& window,QAction* action,const QString& name,const std::function<QString(QDialog&)>& configure,bool draft=false,bool cancel=false) {
    if(!action || !action->isEnabled()) return "Action is unavailable: "+name;
    const auto before=window.currentDocument();const bool dirty=window.currentDirty();
    bool configured=false,done=false,driving=false;QString error;QTimer timer;timer.setInterval(20);QElapsedTimer elapsed;elapsed.start();
    QObject::connect(&timer,&QTimer::timeout,&window,[&] {
        if(driving) return;
        auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());if(!dialog) return;
        if(elapsed.elapsed()>180000) {
            const auto* status=dialog->findChild<QLabel*>("revisableAdjustmentsStatus");
            error="Dialog/preview timed out: "+name+" — "+(status ? status->text() : QString());dialog->reject();return;
        }
        if(dialog->objectName()!=name) {error="Unexpected modal dialog: "+dialog->objectName();dialog->reject();return;}
        auto* buttons=dialog->findChild<QDialogButtonBox*>(draft?"revisableAdjustmentsButtons":QString{});
        if(!buttons) {error="Missing dialog buttons";dialog->reject();return;}
        auto* ok=buttons->button(QDialogButtonBox::Ok);if(!ok) {error="Missing OK control";dialog->reject();return;}
        if(draft && (window.currentDocument()!=before || window.currentDirty()!=dirty)) {error="Draft modified committed history";dialog->reject();return;}
        if(!configured) {
            if(draft && !ok->isEnabled()) return;
            driving=true;
            if(!QTest::qWaitForWindowActive(dialog,1500)) error="Modal window failed to activate";
            else error=configure(*dialog);
            configured=true;driving=false;if(!error.isEmpty()){dialog->reject();return;}
            if(draft) return;
        }
        if(!ok->isEnabled()) {
            if(draft) if(const auto* status=dialog->findChild<QLabel*>("revisableAdjustmentsStatus");status && status->text()!="Updating preview…" && status->text()!="Preview ready") {
                error=status->text();dialog->reject();
            }
            return;
        }
        done=true;timer.stop();QTest::mouseClick(buttons->button(cancel?QDialogButtonBox::Cancel:QDialogButtonBox::Ok),Qt::LeftButton);
    });
    timer.start();action->trigger();timer.stop();if(!done && error.isEmpty()) error="Dialog closed before completion: "+name;return error;
}
QString scalar(QDialog& dialog,const char* name,double value) {
    auto* control=dialog.findChild<QDoubleSpinBox*>(name);if(!control) return "Missing field: "+QString::fromLatin1(name);
    control->setValue(value);return {};
}
QString curvesPoint(QDialog& dialog,int channel,double output) {
    auto* choice=dialog.findChild<QComboBox*>("curveChannel");auto* graph=dialog.findChild<QWidget*>("toneCurveEditor");
    if(!choice || !graph) return "Missing curve controls";
    choice->setCurrentIndex(channel);const auto area=QRectF(graph->rect()).adjusted(18,14,-14,-18);
    QTest::mouseClick(graph,Qt::LeftButton,{},QPointF(area.left()+area.width()*.5,area.bottom()-area.height()*.5).toPoint());
    auto error=scalar(dialog,"curvePointInput",127.5);if(!error.isEmpty()) return error;
    return scalar(dialog,"curvePointOutput",output*255);
}
QString grade(QDialog& dialog,bool revision) {
    auto* list=dialog.findChild<QListWidget*>("revisableAdjustmentsStack");auto* kind=dialog.findChild<QComboBox*>("revisableAdjustmentsKind");auto* add=dialog.findChild<QPushButton*>("revisableAdjustmentsAdd");
    if(!list || !kind || !add) return "Missing retained stack controls";
    const auto addKind=[&](engine::AdjustmentKind value){kind->setCurrentIndex(int(value));QTest::mouseClick(add,Qt::LeftButton);};
    list->setCurrentRow(0);auto error=scalar(dialog,"photoAdjustmentValue",revision?.1:.2);if(!error.isEmpty())return error;
    if(!revision) addKind(engine::AdjustmentKind::Levels);else list->setCurrentRow(1);
    error=scalar(dialog,"levelsInputBlack",revision?3:5);if(!error.isEmpty())return error;
    error=scalar(dialog,"levelsGamma",1.05);if(!error.isEmpty())return error;
    if(!revision) addKind(engine::AdjustmentKind::Curves);else list->setCurrentRow(2);
    if(!revision) {error=curvesPoint(dialog,0,.53);if(!error.isEmpty())return error;}
    error=curvesPoint(dialog,1,revision?.505:.515);if(!error.isEmpty())return error;
    if(!revision) addKind(engine::AdjustmentKind::ColorBalance);else list->setCurrentRow(3);
    error=scalar(dialog,"balanceWarmth",revision?2:3);if(!error.isEmpty())return error;return scalar(dialog,"balanceTint",revision?-1:-2);
}
QByteArray fileHash(const QString& path) {QFile file(path);if(!file.open(QIODevice::ReadOnly)) return {};QCryptographicHash hash(QCryptographicHash::Sha256);if(!hash.addData(&file))return {};return hash.result().toHex();}
}
class D2PhotoWorkflowTest final:public QObject {
    Q_OBJECT
    QString fixtures_,capture_;
private slots:
    void initTestCase() {
        fixtures_=qEnvironmentVariable("LUMAIRE_D2_PHOTO_FIXTURES");capture_=qEnvironmentVariable("LUMAIRE_D2_PHOTO_CAPTURE_DIR");
        if(!fixtures_.isEmpty()) QVERIFY(QDir::isAbsolutePath(fixtures_));
        if(!capture_.isEmpty()) {
            QVERIFY(!fixtures_.isEmpty());QVERIFY(QDir::isAbsolutePath(capture_));QVERIFY(!QFileInfo::exists(capture_));QVERIFY(QDir().mkpath(capture_));
            QVERIFY(QFile::copy(QDir(fixtures_).filePath("ATTRIBUTION.md"),QDir(capture_).filePath("ATTRIBUTION.md")));
            QVERIFY(QFile::copy(QDir(fixtures_).filePath("provenance.json"),QDir(capture_).filePath("provenance.json")));
        }
    }
    void photoRevision_data() {QTest::addColumn<bool>("portrait");QTest::newRow("portrait")<<true;QTest::newRow("product")<<false;}
    void photoRevision() {
        QFETCH(bool,portrait);const QString name=portrait?"portrait":"product";
        QTemporaryDir temporary(QDir(buildDirectory).filePath("d2-photo-XXXXXX"));QVERIFY(temporary.isValid());
        const bool actual=!fixtures_.isEmpty();const int width=actual?4000:96,height=actual?3000:72;
        QString inputPath=actual?QDir(fixtures_).filePath(name+".jpg"):temporary.filePath("input.png");
        if(actual) {
            const auto expected=portrait?QByteArray("84496329b251241f0ed8238c27d1b077ab9b3cf34b85f12893c208b70f1ad0da"):QByteArray("6c9cfc43bfaf2086bfbb085c4a03fc71eba38caabe6fe403a7bc191bd5d0d029");
            QCOMPARE(fileHash(inputPath),expected);
        } else {
            QImage image(width,height,QImage::Format_RGB32);for(int y=0;y<height;++y)for(int x=0;x<width;++x)image.setPixel(x,y,qRgb(30+x*170/width,40+y*150/height,90));QVERIFY(image.save(inputPath));
            qInfo("Synthetic smoke fixture: this run is not actual-photograph D2 acceptance");
        }
        io::SpillLimits spillLimits;spillLimits.maxBytes=1024ull*1024*1024;spillLimits.maxPayloadBytes=2*1024*1024;spillLimits.maxEntries=8192;spillLimits.maxIoOperations=1;spillLimits.minFreeBytes=256*1024*1024;
        auto spill=std::make_shared<io::SpillStore>(temporary.path().toStdString(),spillLimits);
        auto resources=std::make_shared<RuntimeResources>(ResourceLimits::forMachine(8ull*1024*1024*1024,2),MemorySample::read,spill);
        QElapsedTimer elapsed;elapsed.start();std::uint64_t peakRss=0,peakAdmitted=0,peakSpill=0;std::size_t peakEntries=0;QTimer sampler;sampler.setInterval(50);
        const auto sample=[&] {const auto host=MemorySample::read();const auto charged=resources->memory->snapshot();const auto disk=spill->stats();peakRss=std::max(peakRss,host.resident.value_or(0));peakAdmitted=std::max(peakAdmitted,charged.pendingCpu+charged.committedCpu+charged.pendingGpu+charged.committedGpu);peakSpill=std::max(peakSpill,disk.bytes);peakEntries=std::max(peakEntries,disk.entries);};
        connect(&sampler,&QTimer::timeout,this,sample);sampler.start();
        {
        MainWindow window(nullptr,true,{},resources);window.show();QSignalSpy finished(&window,&MainWindow::operationFinished);qsizetype operations=0;
#define COMPLETE(command) do {++operations;command;QTRY_COMPARE_WITH_TIMEOUT(finished.count(),operations,180000);QVERIFY2(finished.last()[0].toBool(),qPrintable(finished.last()[1].toString()));QVERIFY(!window.currentBusy());sample();} while(false)
        const auto digest=[&](const std::shared_ptr<const engine::RasterSnapshot>& raster) {
            if(!raster) {QTest::qFail("Missing expected raster",__FILE__,__LINE__);return Digest{{},"Missing raster"};}
            QFutureWatcher<Digest> watcher;watcher.setFuture(QtConcurrent::run([raster,resources]{return rasterDigest(raster,resources);}));
            const bool finishedDigest=QTest::qWaitFor([&]{return watcher.isFinished();},180000);
            const auto result=finishedDigest?watcher.result():Digest{{},"Digest timed out"};
            if(!result.error.isEmpty()) QTest::qFail(qPrintable(result.error),__FILE__,__LINE__);
            return result;
        };
        COMPLETE(window.openPath(inputPath));QCOMPARE(window.currentDocument()->width,width);QCOMPARE(window.currentDocument()->height,height);
        const auto baseId=*window.activeLayer();const auto original=window.currentDocument()->layer(baseId).raster;const auto originalDigest=digest(original);QVERIFY2(originalDigest.error.isEmpty(),qPrintable(originalDigest.error));
        const auto rename=[&](const engine::Id& id,const QString& newName){auto* tree=window.findChild<QTreeWidget*>("layerTree");if(!tree)return false;for(QTreeWidgetItemIterator it(tree);*it;++it)if((*it)->data(0,Qt::UserRole).toString()==QString::fromStdString(id.text())){(*it)->setText(0,newName);return QTest::qWaitFor([&]{return window.currentDocument()->layer(id).name==newName.toStdString();},3000);}return false;};
        QVERIFY(rename(baseId,"Original photograph"));
        window.duplicateLayer();const auto retouchId=*window.activeLayer();QVERIFY(retouchId!=baseId);QVERIFY(rename(retouchId,"Retouch"));
        const double scale=double(width)/4000;const auto point=[&](double x,double y){return engine::Coordinate{x*scale,y*scale};};
        // Frozen visual-quality recipe: sample matching clean wall/table,
        // cover the actual distraction, and retain both Heal and Clone stages.
        engine::BrushSettings brush;brush.hardness=.25;brush.opacity=1;
        const std::vector<engine::Coordinate> mainPath=portrait?std::vector<engine::Coordinate>{point(3180,1110),point(3180,1253),point(3200,1253),point(3200,1110),point(3220,1110),point(3220,1253)}:std::vector<engine::Coordinate>{point(3700,675),point(3650,720),point(3590,800),point(3500,900),point(3400,1000),point(3350,1100),point(3300,1190),point(3250,1240),point(3200,1280),point(3130,1310),point(3070,1335)};
        const auto donor=portrait?point(2880,1110):point(3100,675);
        brush.diameter=std::max(4.,(portrait?130:150)*scale);
        if(portrait) COMPLETE(window.setSelectionCurrent(engine::Selection{{qRound(3125*scale),qRound(1055*scale),qRound(125*scale),qRound(212*scale)},engine::SelectionShape::Rectangle,false,8*scale}));
        COMPLETE(window.applyRetouchStroke(mainPath,brush,donor,false,std::max(1.,32*scale)));
        if(portrait) COMPLETE(window.setSelectionCurrent({}));
        const std::vector<engine::Coordinate> secondaryPath=portrait?std::vector<engine::Coordinate>{point(600,1435),point(800,1430),point(800,1450),point(600,1455),point(600,1475),point(800,1470)}:std::vector<engine::Coordinate>{point(2960,2150)};
        const auto heal=secondaryPath.front(),healDonor=portrait?point(440,1465):point(2760,2150);
        brush.diameter=std::max(4.,(portrait?100:160)*scale);
        COMPLETE(window.applyRetouchStroke(secondaryPath,brush,healDonor,true,std::max(1.,32*scale)));
        COMPLETE(window.applyRetouchStroke(secondaryPath,brush,healDonor,false,std::max(1.,32*scale)));
        QVERIFY(window.currentDocument()->layer(retouchId).retouch);QCOMPARE(window.currentDocument()->layer(retouchId).retouch->strokes.size(),std::size_t(3));
        QVERIFY(window.currentDocument()->layer(baseId).raster==original);
        const auto retouchedDigest=digest(window.currentDocument()->layer(retouchId).raster);
        QVERIFY2(retouchedDigest.bytes!=originalDigest.bytes,"Retouch records did not produce changed canonical pixels");
        window.duplicateLayer();const auto gradeId=*window.activeLayer();QVERIFY(gradeId!=retouchId);QVERIFY(rename(gradeId,"Grade"));
        engine::Selection selection;selection.shape=engine::SelectionShape::Ellipse;selection.bounds=portrait?engine::Extent{qRound(1450*scale),qRound(160*scale),qRound(1100*scale),qRound(1400*scale)}:engine::Extent{qRound(1330*scale),qRound(180*scale),qRound(1170*scale),qRound(2640*scale)};selection.featherRadius=std::max(1.,32*scale);
        COMPLETE(window.setSelectionCurrent(selection));COMPLETE(window.createMaskCurrent(true));COMPLETE(window.setSelectionCurrent({}));
        auto* gradeAction=window.findChild<QAction*>("revisableAdjustmentsAction");QVERIFY(gradeAction);
        COMPLETE(QVERIFY2(driveDialog(window,gradeAction,"revisableAdjustmentsDialog",[](auto& dialog){return grade(dialog,false);},true).isEmpty(),"Initial retained grade failed"));
        const auto firstGrade=window.currentDocument();window.undoCurrent();QVERIFY(window.currentDocument()!=firstGrade);window.redoCurrent();QVERIFY(window.currentDocument()==firstGrade);
        const auto originalProject=temporary.filePath("original.cproj");COMPLETE(window.saveCurrentTo(originalProject));
        window.closeDocument(window.tabs()->currentIndex());COMPLETE(window.openPath(originalProject));
        QCOMPARE(digest(window.currentDocument()->layer(baseId).raster).bytes,originalDigest.bytes);
        QVERIFY(window.currentDocument()->layer(baseId).name=="Original photograph");QVERIFY(window.currentDocument()->layer(retouchId).name=="Retouch");QVERIFY(window.currentDocument()->layer(gradeId).name=="Grade");
        QVERIFY(window.currentDocument()->layer(gradeId).adjustments);QVERIFY(window.currentDocument()->layer(retouchId).retouch);
        QCOMPARE(window.currentDocument()->layer(gradeId).adjustments->operations.size(),std::size_t(4));QCOMPARE(window.currentDocument()->layer(retouchId).retouch->strokes.size(),std::size_t(3));
        window.selectLayer(retouchId);auto* strength=window.findChild<QAction*>("retouchStrengthAction");QVERIFY(strength);
        COMPLETE(QVERIFY(driveDialog(window,strength,"retouchStrengthDialog",[](QDialog& dialog){auto* choice=dialog.findChild<QComboBox*>("retouchStrokeChoice");if(!choice)return QString("Missing retouch stroke selector");choice->setCurrentIndex(0);return scalar(dialog,"retouchStrength",40);}).isEmpty()));
        QCOMPARE(window.currentDocument()->layer(retouchId).retouch->strokes[0].opacity,.4);
        auto* layerOpacity=window.findChild<QDoubleSpinBox*>("layerOpacity");QVERIFY(layerOpacity);layerOpacity->setValue(65);QVERIFY(std::abs(window.currentDocument()->layer(retouchId).opacity-.65f)<1e-6f);
        brush.opacity=.3;COMPLETE(window.applyRetouchStroke({{heal.x+12*scale,heal.y}},brush,healDonor,true,std::max(1.,16*scale)));
        window.selectLayer(gradeId);const auto previousMask=window.currentDocument()->layer(gradeId).mask;
        COMPLETE(window.removeMaskCurrent());selection.bounds.x+=qRound(40*scale);selection.bounds.width-=qRound(40*scale);selection.featherRadius=std::max(1.,48*scale);
        COMPLETE(window.setSelectionCurrent(selection));COMPLETE(window.createMaskCurrent(true));COMPLETE(window.setSelectionCurrent({}));
        COMPLETE(window.featherMaskCurrent(std::max(1.,8*scale)));const auto changedMask=window.currentDocument();QVERIFY(changedMask->layer(gradeId).mask!=previousMask);
        QVERIFY2(digest(changedMask->layer(gradeId).mask).bytes!=digest(previousMask).bytes,"Mask revision did not change canonical coverage");
        COMPLETE(window.enableMaskCurrent(false));QVERIFY(!window.currentDocument()->layer(gradeId).maskEnabled);window.undoCurrent();QVERIFY(window.currentDocument()==changedMask);window.redoCurrent();QVERIFY(!window.currentDocument()->layer(gradeId).maskEnabled);window.undoCurrent();QVERIFY(window.currentDocument()==changedMask);
        COMPLETE(QVERIFY(driveDialog(window,gradeAction,"revisableAdjustmentsDialog",[](auto& dialog){return grade(dialog,true);},true).isEmpty()));
        const auto committed=window.currentDocument();const auto count=finished.count();
        qInfo()<<"before_cancel"<<name<<elapsed.elapsed()<<qulonglong(MemorySample::read().resident.value_or(0));
        const auto cancelError=driveDialog(window,gradeAction,"revisableAdjustmentsDialog",[](QDialog& dialog){auto* list=dialog.findChild<QListWidget*>("revisableAdjustmentsStack");if(!list)return QString("Missing list");list->setCurrentRow(0);return scalar(dialog,"photoAdjustmentValue",1);},true,true);
        QVERIFY2(cancelError.isEmpty(),qPrintable(cancelError));
        QCOMPARE(finished.count(),count);QVERIFY(window.currentDocument()==committed);
        const auto revisedProject=temporary.filePath("revision.cproj");COMPLETE(window.saveCurrentTo(revisedProject));
        QCOMPARE(digest(original).bytes,originalDigest.bytes);QCOMPARE(window.currentDocument()->width,width);
        // Revision B intentionally requests partial retouch strength. Preserve
        // that evidence, then continue to a separate polished deliverable C.
        window.selectLayer(retouchId);layerOpacity->setValue(100);brush.opacity=1;brush.diameter=std::max(4.,(portrait?130:150)*scale);
        if(portrait) COMPLETE(window.setSelectionCurrent(engine::Selection{{qRound(3125*scale),qRound(1055*scale),qRound(125*scale),qRound(212*scale)},engine::SelectionShape::Rectangle,false,8*scale}));
        COMPLETE(window.applyRetouchStroke(mainPath,brush,donor,false,std::max(1.,32*scale)));
        if(portrait) COMPLETE(window.setSelectionCurrent({}));
        brush.diameter=std::max(4.,(portrait?100:160)*scale);
        COMPLETE(window.applyRetouchStroke(secondaryPath,brush,healDonor,false,std::max(1.,32*scale)));
        const auto polished=window.currentDocument();QCOMPARE(polished->layer(retouchId).retouch->strokes.size(),std::size_t(6));
        const auto polishedProject=temporary.filePath("polished.cproj");COMPLETE(window.saveCurrentTo(polishedProject));
        auto* resize=window.findChild<QAction*>("resampleDocumentAction");QVERIFY(resize);
        COMPLETE(QVERIFY(driveDialog(window,resize,"imageResampleDialog",[&](QDialog& dialog){auto* w=dialog.findChild<QSpinBox*>("resampleWidth");auto* h=dialog.findChild<QSpinBox*>("resampleHeight");auto* filter=dialog.findChild<QComboBox*>("resampleFilter");if(!w || !h || !filter)return QString("Missing image size controls");w->setValue(width/2);h->setValue(height/2);filter->setCurrentIndex(int(actual?engine::Sampling::Lanczos:engine::Sampling::Nearest));return QString();}).isEmpty()));
        QCOMPARE(window.currentDocument()->width,width/2);QCOMPARE(window.currentDocument()->height,height/2);const auto resized=window.currentDocument();
        window.undoCurrent();QVERIFY(window.currentDocument()==polished);window.redoCurrent();QVERIFY(window.currentDocument()==resized);
        const auto deliveryProject=temporary.filePath("delivery.cproj");COMPLETE(window.saveCurrentTo(deliveryProject));
        std::array<Digest,3> rasterHashes{digest(resized->layer(baseId).raster),digest(resized->layer(retouchId).raster),digest(resized->layer(gradeId).raster)};const auto maskHash=digest(resized->layer(gradeId).mask);
        for(const auto& hash:rasterHashes) {QVERIFY2(hash.error.isEmpty(),qPrintable(hash.error));}
        QVERIFY2(maskHash.error.isEmpty(),qPrintable(maskHash.error));
        const auto pngPath=temporary.filePath("delivery.png"),jpegPath=temporary.filePath("delivery.jpg");COMPLETE(window.exportCurrentTo(pngPath));io::ExportOptions options;options.quality=95;COMPLETE(window.exportCurrentTo(jpegPath,options));
        window.closeDocument(window.tabs()->currentIndex());COMPLETE(window.openPath(deliveryProject));
        int index=0;for(const auto& id:{baseId,retouchId,gradeId}) {const auto& old=resized->layer(id);const auto& current=window.currentDocument()->layer(id);QCOMPARE(current.name,old.name);const auto hash=digest(current.raster);QVERIFY2(hash.error.isEmpty(),qPrintable(hash.error));QCOMPARE(hash.bytes,rasterHashes[std::size_t(index++)].bytes);QVERIFY(current.localToDocument==old.localToDocument);QCOMPARE(current.opacity,old.opacity);if(old.retouch){QVERIFY(current.retouch);QVERIFY(current.retouch->strokes==old.retouch->strokes);QCOMPARE(digest(current.retouch->source).bytes,digest(old.retouch->source).bytes);}if(old.adjustments){QVERIFY(current.adjustments);QVERIFY(current.adjustments->operations==old.adjustments->operations);QCOMPARE(digest(current.adjustments->source).bytes,digest(old.adjustments->source).bytes);}}
        QCOMPARE(digest(window.currentDocument()->layer(gradeId).mask).bytes,maskHash.bytes);
        const QImage png(pngPath),jpeg(jpegPath);QVERIFY(!png.isNull());QVERIFY(!jpeg.isNull());QCOMPARE(png.size(),QSize(width/2,height/2));QCOMPARE(jpeg.size(),png.size());
        struct ExportCheck {QString error;qint64 jpegError=0,samples=0;};
        QFutureWatcher<ExportCheck> exportWatcher;
        exportWatcher.setFuture(QtConcurrent::run([accepted=window.currentDocument(),png,jpeg,resources] {
            try {
                auto permit=resources->compute.acquire(WorkScheduler::Priority::Processing,true);
                if(!permit) return ExportCheck{"Export verification canceled"};
                qint64 jpegError=0,samples=0;
        for(int gy=0;gy<64;++gy)for(int gx=0;gx<64;++gx) {const int x=std::min(png.width()-1,(2*gx+1)*png.width()/128),y=std::min(png.height()-1,(2*gy+1)*png.height()/128);const auto pixel=accepted->stack.evaluate(x+.5,y+.5);const std::array<float,3> channels{pixel.r,pixel.g,pixel.b};const auto actualPng=png.pixelColor(x,y),actualJpeg=jpeg.pixelColor(x,y);const std::array<int,3> p{actualPng.red(),actualPng.green(),actualPng.blue()},j{actualJpeg.red(),actualJpeg.green(),actualJpeg.blue()};for(int c=0;c<3;++c){const auto expected=qRound(255*std::clamp(engine::encodeSrgb(pixel.a>0?channels[std::size_t(c)]/pixel.a:0),0.f,1.f));if(std::abs(p[std::size_t(c)]-expected)>1) return ExportCheck{"PNG color differs from accepted document",jpegError,samples};const auto white=qRound(255*std::clamp(engine::encodeSrgb(channels[std::size_t(c)]+1-pixel.a),0.f,1.f));jpegError+=std::abs(j[std::size_t(c)]-white);++samples;}if(std::abs(actualPng.alpha()-qRound(255*pixel.a))>1 || actualJpeg.alpha()!=255) return ExportCheck{"Export alpha differs from accepted document",jpegError,samples};}
                return ExportCheck{{},jpegError,samples};
            } catch(const std::exception& error) {return ExportCheck{QString::fromUtf8(error.what())};}
        }));
        QTRY_VERIFY_WITH_TIMEOUT(exportWatcher.isFinished(),180000);
        const auto exportCheck=exportWatcher.result();QVERIFY2(exportCheck.error.isEmpty(),qPrintable(exportCheck.error));
        const auto jpegError=exportCheck.jpegError,samples=exportCheck.samples;
        QVERIFY2(samples>0 && jpegError<samples*8,"JPEG sampled mean channel error exceeds 8 at quality 95");
        auto* canvas=window.currentCanvas();canvas->fit();QTRY_VERIFY_WITH_TIMEOUT(!canvas->captureCpuPresentation().isNull(),180000);
        if(!capture_.isEmpty()) {const QDir out(capture_);for(const auto& pair:{qMakePair(originalProject,name+"-original.cproj"),qMakePair(revisedProject,name+"-revision.cproj"),qMakePair(polishedProject,name+"-polished.cproj"),qMakePair(deliveryProject,name+"-delivery.cproj"),qMakePair(pngPath,name+"-delivery.png"),qMakePair(jpegPath,name+"-delivery.jpg")})QVERIFY(QFile::copy(pair.first,out.filePath(pair.second)));auto capture=window.grab();QPainter painter(&capture);painter.drawImage(QRect(window.mapFromGlobal(canvas->mapToGlobal(QPoint())),canvas->size()),canvas->captureCpuPresentation());painter.end();QVERIFY(capture.save(out.filePath(name+"-workspace.png")));}
        sample();QCOMPARE(spill->stats().cleanupFailures,std::uint64_t(0));qInfo().noquote()<<QJsonDocument(QJsonObject{{"fixture",name},{"actual_photograph",actual},{"elapsed_ms",elapsed.elapsed()},{"sampled_rss_peak",qint64(peakRss)},{"admitted_peak",qint64(peakAdmitted)},{"spill_peak",qint64(peakSpill)},{"spill_entries_peak",qint64(peakEntries)},{"jpeg_mean_sample_error",double(jpegError)/double(samples)},{"system_floor",qint64(resources->limits.systemHeadroom)}}).toJson(QJsonDocument::Compact);
#undef COMPLETE
        }
        sampler.stop();
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        QFutureWatcher<void> cleanupWatcher;
        cleanupWatcher.setFuture(QtConcurrent::run([resources,spill] {
            resources->spillCoordinator->waitIdle();
            spill->collectGarbage();
        }));
        QTRY_VERIFY_WITH_TIMEOUT(cleanupWatcher.isFinished(),180000);
        const auto cleanup=spill->stats();
        QCOMPARE(cleanup.bytes,std::uint64_t(0));QCOMPARE(cleanup.entries,std::size_t(0));QCOMPARE(cleanup.activeIo,std::size_t(0));QCOMPARE(cleanup.cleanupFailures,std::uint64_t(0));
        qInfo().noquote()<<QJsonDocument(QJsonObject{{"fixture",name},{"cleanup_bytes",qint64(cleanup.bytes)},{"cleanup_entries",qint64(cleanup.entries)},{"cleanup_failures",qint64(cleanup.cleanupFailures)}}).toJson(QJsonDocument::Compact);
    }
};
int main(int argc,char** argv) {if(argc<2){qCritical("Pass the existing disk-backed build directory");return 2;}buildDirectory=QDir(QString::fromLocal8Bit(argv[1])).absolutePath();QApplication app(argc,argv);D2PhotoWorkflowTest test;return QTest::qExec(&test,argc-1,argv+1);}
#include "d2_photo_workflow_test.moc"
