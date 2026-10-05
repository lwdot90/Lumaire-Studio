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
#include <QPainter>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTimer>
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
        if(elapsed.elapsed()>180000) {error="Dialog/preview timed out: "+name;dialog->reject();return;}
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
        if(!ok->isEnabled()) return;
        done=true;timer.stop();QTest::mouseClick(buttons->button(cancel?QDialogButtonBox::Cancel:QDialogButtonBox::Ok),Qt::LeftButton);
    });
    timer.start();action->trigger();timer.stop();if(!done && error.isEmpty()) error="Dialog closed before completion: "+name;return error;
}

}
class D2SavedPhotoReviewTest final:public QObject {
 Q_OBJECT
 QString artifacts_;
private slots:
 void initTestCase(){artifacts_=qEnvironmentVariable("LUMAIRE_D2_REVIEW_ARTIFACTS");QVERIFY(QDir::isAbsolutePath(artifacts_));QVERIFY(QFileInfo(artifacts_).isDir());}
 void savedPhotos_data(){QTest::addColumn<bool>("portrait");QTest::newRow("portrait")<<true;QTest::newRow("product")<<false;}
 void savedPhotos(){
  QFETCH(bool,portrait);const QString name=portrait?"portrait":"product";QDir out(artifacts_);
  QTemporaryDir scratch(QDir(buildDirectory).filePath("d2-review-XXXXXX"));QVERIFY(scratch.isValid());
  io::SpillLimits limits;limits.maxBytes=1024ull*1024*1024;limits.maxPayloadBytes=2*1024*1024;limits.maxEntries=8192;limits.maxIoOperations=1;limits.minFreeBytes=256*1024*1024;
  auto spill=std::make_shared<io::SpillStore>(scratch.path().toStdString(),limits);
  auto resources=std::make_shared<RuntimeResources>(ResourceLimits::forMachine(8ull*1024*1024*1024,2),MemorySample::read,spill);
  {
   MainWindow window(nullptr,true,{},resources);window.show();QSignalSpy finished(&window,&MainWindow::operationFinished);qsizetype operations=0;
#define COMPLETE(command) do{++operations;command;QTRY_COMPARE_WITH_TIMEOUT(finished.count(),operations,180000);QVERIFY2(finished.last()[0].toBool(),qPrintable(finished.last()[1].toString()));}while(false)
   const auto digest=[&](const auto& raster){QFutureWatcher<Digest> watcher;watcher.setFuture(QtConcurrent::run([raster,resources]{return rasterDigest(raster,resources);}));if(!QTest::qWaitFor([&]{return watcher.isFinished();},180000))return Digest{{},"Digest timed out"};return watcher.result();};
   const auto workspace=[&](const QString& suffix){auto* canvas=window.currentCanvas();canvas->fit();if(!QTest::qWaitFor([&]{return !canvas->captureCpuPresentation().isNull();},180000))return false;auto capture=window.grab();QPainter painter(&capture);painter.drawImage(QRect(window.mapFromGlobal(canvas->mapToGlobal(QPoint())),canvas->size()),canvas->captureCpuPresentation());painter.end();return capture.save(out.filePath(name+suffix));};
   COMPLETE(window.openPath(out.filePath(name+"-original.cproj")));auto original=window.currentDocument();QCOMPARE(original->width,4000);QCOMPARE(original->height,3000);QCOMPARE(original->layers().size(),std::size_t(3));
   engine::Id baseId=engine::Id::generate(),retouchId=engine::Id::generate(),gradeId=engine::Id::generate();
   for(const auto& node:original->layers()){if(node.adjustments)gradeId=node.id;else if(node.retouch)retouchId=node.id;else baseId=node.id;}
   const auto& aBase=original->layer(baseId);const auto& aRetouch=original->layer(retouchId);const auto& aGrade=original->layer(gradeId);
   QVERIFY(aBase.name=="Original photograph");QVERIFY(aRetouch.name=="Retouch");QVERIFY(aGrade.name=="Grade");
   QVERIFY(aRetouch.retouch);QCOMPARE(aRetouch.retouch->strokes.size(),std::size_t(3));QVERIFY(aGrade.adjustments);QVERIFY(aGrade.mask);QVERIFY(aGrade.maskEnabled);
   const auto baseHash=digest(aBase.raster);QVERIFY2(baseHash.error.isEmpty(),qPrintable(baseHash.error));QCOMPARE(digest(aRetouch.retouch->source).bytes,baseHash.bytes);
   const auto& aOps=aGrade.adjustments->operations;QCOMPARE(aOps.size(),std::size_t(4));
   QCOMPARE(aOps[0].kind,engine::AdjustmentKind::Exposure);QCOMPARE(aOps[0].value,.2);QCOMPARE(aOps[1].kind,engine::AdjustmentKind::Levels);QCOMPARE(aOps[1].levels.inputBlack,5./255.);QCOMPARE(aOps[1].levels.gamma,1.05);
   QCOMPARE(aOps[2].kind,engine::AdjustmentKind::Curves);QCOMPARE(aOps[2].curve.size(),std::size_t(3));QCOMPARE(aOps[2].curve[1].input,.5);QCOMPARE(aOps[2].curve[1].output,.53);QCOMPARE(aOps[2].channelCurves[0][1].output,.515);
   const engine::AdjustmentParameters identity;QVERIFY(aOps[2].channelCurves[1]==identity.channelCurves[1]);QVERIFY(aOps[2].channelCurves[2]==identity.channelCurves[2]);
   const auto& firstStroke=aRetouch.retouch->strokes[0];const auto& secondStroke=aRetouch.retouch->strokes[1];QCOMPARE(firstStroke.kind,engine::RetouchKind::Clone);QCOMPARE(secondStroke.kind,engine::RetouchKind::Heal);QCOMPARE(firstStroke.opacity,1.);QCOMPARE(firstStroke.hardness,.25);QCOMPARE(secondStroke.hardness,.25);QCOMPARE(secondStroke.opacity,1.);QCOMPARE(firstStroke.diameter,portrait?130.:150.);QCOMPARE(secondStroke.diameter,portrait?100.:160.);QCOMPARE(firstStroke.sourceAnchor.x,portrait?2880.:3100.);QCOMPARE(firstStroke.sourceAnchor.y,portrait?1110.:675.);QCOMPARE(secondStroke.sourceAnchor.x,portrait?440.:2760.);QCOMPARE(secondStroke.sourceAnchor.y,portrait?1465.:2150.);
   if(portrait){QVERIFY(firstStroke.selection);const engine::Selection expectedSelection{{3125,1055,125,212},engine::SelectionShape::Rectangle,false,8};QVERIFY(*firstStroke.selection==expectedSelection);}else QVERIFY(!firstStroke.selection);QVERIFY(!secondStroke.selection);
   const std::vector<engine::Coordinate> mainPath=portrait?std::vector<engine::Coordinate>{{3180,1110},{3180,1253},{3200,1253},{3200,1110},{3220,1110},{3220,1253}}:std::vector<engine::Coordinate>{{3700,675},{3650,720},{3590,800},{3500,900},{3400,1000},{3350,1100},{3300,1190},{3250,1240},{3200,1280},{3130,1310},{3070,1335}};
   QCOMPARE(firstStroke.points.size(),mainPath.size());for(std::size_t i=0;i<mainPath.size();++i){QCOMPARE(firstStroke.points[i].x,mainPath[i].x);QCOMPARE(firstStroke.points[i].y,mainPath[i].y);}
   QCOMPARE(aOps[3].kind,engine::AdjustmentKind::ColorBalance);QCOMPARE(aOps[3].colorBalance.warmth,.03);QCOMPARE(aOps[3].colorBalance.tint,-.02);
   QVERIFY(workspace("-original-workspace.png"));const auto originalPng=out.filePath(name+"-original.png");QVERIFY(!QFileInfo::exists(originalPng));COMPLETE(window.exportCurrentTo(originalPng));
   window.closeDocument(window.tabs()->currentIndex());COMPLETE(window.openPath(out.filePath(name+"-revision.cproj")));auto revised=window.currentDocument();const auto& bBase=revised->layer(baseId);const auto& bRetouch=revised->layer(retouchId);const auto& bGrade=revised->layer(gradeId);
   QCOMPARE(digest(bBase.raster).bytes,baseHash.bytes);QCOMPARE(digest(bRetouch.retouch->source).bytes,baseHash.bytes);QVERIFY(bBase.localToDocument==aBase.localToDocument);QCOMPARE(bBase.opacity,aBase.opacity);QCOMPARE(revised->resolution,original->resolution);
   const auto profileEqual=[](const auto& a,const auto& b){return a==b || (a&&b&&a->id==b->id&&a->bytes==b->bytes);};QVERIFY(profileEqual(aBase.raster->sourceProfile,bBase.raster->sourceProfile));QVERIFY(profileEqual(aRetouch.raster->sourceProfile,bRetouch.raster->sourceProfile));QVERIFY(profileEqual(aGrade.raster->sourceProfile,bGrade.raster->sourceProfile));
   QCOMPARE(bBase.name,aBase.name);QCOMPARE(bRetouch.name,aRetouch.name);QCOMPARE(bGrade.name,aGrade.name);
   QCOMPARE(bRetouch.retouch->strokes.size(),std::size_t(4));auto expected=aRetouch.retouch->strokes[0];expected.opacity=.4;QVERIFY(bRetouch.retouch->strokes[0]==expected);QVERIFY(bRetouch.retouch->strokes[1]==aRetouch.retouch->strokes[1]);QVERIFY(bRetouch.retouch->strokes[2]==aRetouch.retouch->strokes[2]);QVERIFY(std::abs(bRetouch.opacity-.65f)<1e-6f);QVERIFY(bRetouch.localToDocument==aRetouch.localToDocument);
   const auto& bOps=bGrade.adjustments->operations;QCOMPARE(bOps.size(),aOps.size());auto expectedOps=aOps;expectedOps[0].value=.1;expectedOps[1].levels.inputBlack=3./255.;expectedOps[2].channelCurves[0][1].output=.505;expectedOps[3].colorBalance.warmth=.02;expectedOps[3].colorBalance.tint=-.01;QVERIFY(bOps==expectedOps);
   QVERIFY(bGrade.localToDocument==aGrade.localToDocument);QCOMPARE(bGrade.opacity,aGrade.opacity);QVERIFY(bGrade.maskEnabled);const auto aMaskHash=digest(aGrade.mask),bMaskHash=digest(bGrade.mask);QVERIFY(aMaskHash.error.isEmpty());QVERIFY(bMaskHash.error.isEmpty());QVERIFY(aMaskHash.bytes!=bMaskHash.bytes);
   QVERIFY(workspace("-revision-workspace.png"));const auto revisionPng=out.filePath(name+"-revision.png");QVERIFY(!QFileInfo::exists(revisionPng));COMPLETE(window.exportCurrentTo(revisionPng));
   window.selectLayer(gradeId);auto* action=window.findChild<QAction*>("revisableAdjustmentsAction");QVERIFY(action);const auto before=window.currentDocument();const auto count=finished.count();
   QVERIFY2(driveDialog(window,action,"revisableAdjustmentsDialog",[&](QDialog& dialog){auto* list=dialog.findChild<QListWidget*>("revisableAdjustmentsStack");auto* channel=dialog.findChild<QComboBox*>("curveChannel");if(!list)return QString("Missing saved stack");list->setCurrentRow(2);channel=dialog.findChild<QComboBox*>("curveChannel");if(!channel)return QString("Missing saved curve channel");channel->setCurrentIndex(1);return dialog.grab().save(out.filePath(name+"-saved-grade-controls.png"))?QString{}:QString("Capture failed");},true,true).isEmpty(),"Saved grade inspection failed");QCOMPARE(finished.count(),count);QVERIFY(window.currentDocument()==before);QVERIFY(window.currentDocument()->layer(gradeId).adjustments->operations==bOps);
   window.closeDocument(window.tabs()->currentIndex());COMPLETE(window.openPath(out.filePath(name+"-polished.cproj")));auto polished=window.currentDocument();QVERIFY(polished->layer(baseId).name==aBase.name);QVERIFY(polished->layer(gradeId).name==aGrade.name);QVERIFY(polished->layer(retouchId).name==aRetouch.name);const auto& cRetouch=polished->layer(retouchId);QCOMPARE(cRetouch.opacity,1.f);QCOMPARE(cRetouch.retouch->strokes.size(),std::size_t(6));for(std::size_t i=0;i<4;++i)QVERIFY(cRetouch.retouch->strokes[i]==bRetouch.retouch->strokes[i]);auto fullClone=aRetouch.retouch->strokes[0];QVERIFY(cRetouch.retouch->strokes[4]==fullClone);QVERIFY(cRetouch.retouch->strokes[5]==aRetouch.retouch->strokes[2]);QVERIFY(polished->layer(gradeId).adjustments->operations==bOps);QCOMPARE(digest(polished->layer(baseId).raster).bytes,baseHash.bytes);QCOMPARE(digest(polished->layer(gradeId).mask).bytes,bMaskHash.bytes);QVERIFY(workspace("-polished-workspace.png"));const auto polishedPng=out.filePath(name+"-polished.png");QVERIFY(!QFileInfo::exists(polishedPng));COMPLETE(window.exportCurrentTo(polishedPng));
   window.closeDocument(window.tabs()->currentIndex());COMPLETE(window.openPath(out.filePath(name+"-delivery.cproj")));auto delivery=window.currentDocument();QVERIFY(delivery->layer(baseId).name==aBase.name);QVERIFY(delivery->layer(retouchId).name==aRetouch.name);QVERIFY(delivery->layer(gradeId).name==aGrade.name);QCOMPARE(delivery->width,2000);QCOMPARE(delivery->height,1500);QVERIFY(delivery->layer(gradeId).adjustments->operations==bOps);QVERIFY(delivery->layer(gradeId).mask);QVERIFY(delivery->layer(gradeId).maskEnabled);QCOMPARE(delivery->layer(retouchId).retouch->strokes.size(),cRetouch.retouch->strokes.size());QCOMPARE(delivery->layer(retouchId).opacity,cRetouch.opacity);QVERIFY(profileEqual(delivery->layer(baseId).raster->sourceProfile,bBase.raster->sourceProfile));
   window.closeDocument(window.tabs()->currentIndex());COMPLETE(window.openPath(out.filePath(name+"-revision.cproj")));window.selectLayer(gradeId);QCOMPARE(digest(window.currentDocument()->layer(gradeId).mask).bytes,bMaskHash.bytes);
   // Perform an additional real-photo boundary/softness edit and prove exact
   // history restoration. This draft is deliberately never saved over A/B.
   COMPLETE(window.removeMaskCurrent());auto noMask=window.currentDocument();engine::Selection selection{portrait?engine::Extent{1500,160,1050,1400}:engine::Extent{1380,180,1120,2640},engine::SelectionShape::Ellipse,false,56};COMPLETE(window.setSelectionCurrent(selection));auto selected=window.currentDocument();COMPLETE(window.createMaskCurrent(true));auto created=window.currentDocument();const auto maskCreated=digest(created->layer(gradeId).mask);QVERIFY(maskCreated.error.isEmpty());window.undoCurrent();QVERIFY(window.currentDocument()==selected);window.redoCurrent();QVERIFY(window.currentDocument()==created);QCOMPARE(digest(window.currentDocument()->layer(gradeId).mask).bytes,maskCreated.bytes);
   COMPLETE(window.featherMaskCurrent(8));auto softened=window.currentDocument();const auto maskSoft=digest(softened->layer(gradeId).mask);QVERIFY(maskSoft.error.isEmpty());window.undoCurrent();QVERIFY(window.currentDocument()==created);QCOMPARE(digest(window.currentDocument()->layer(gradeId).mask).bytes,maskCreated.bytes);window.redoCurrent();QVERIFY(window.currentDocument()==softened);QCOMPARE(digest(window.currentDocument()->layer(gradeId).mask).bytes,maskSoft.bytes);QCOMPARE(digest(window.currentDocument()->layer(baseId).raster).bytes,baseHash.bytes);QVERIFY(window.currentDocument()->layer(gradeId).adjustments->operations==bOps);
   Q_UNUSED(noMask);
   qInfo().noquote()<<"Saved photograph review passed:"<<name<<"; original/revision full-size PNG, controls and workspace captures retained";
#undef COMPLETE
  }
  QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
  QFutureWatcher<void> cleanupWatcher;cleanupWatcher.setFuture(QtConcurrent::run([resources,spill]{resources->spillCoordinator->waitIdle();spill->collectGarbage();}));QTRY_VERIFY_WITH_TIMEOUT(cleanupWatcher.isFinished(),180000);
  const auto stats=spill->stats();QCOMPARE(stats.bytes,std::uint64_t(0));QCOMPARE(stats.entries,std::size_t(0));QCOMPARE(stats.activeIo,std::size_t(0));QCOMPARE(stats.cleanupFailures,std::uint64_t(0));qInfo().noquote()<<"Review cleanup zero:"<<name;
 }
};
int main(int argc,char** argv){if(argc<2){qCritical("Pass build directory");return 2;}buildDirectory=QDir(QString::fromLocal8Bit(argv[1])).absolutePath();QApplication app(argc,argv);D2SavedPhotoReviewTest test;return QTest::qExec(&test,argc-1,argv+1);}
#include "d2_saved_photo_review_test.moc"
