#include "app/window.h"
#include "io/spill_store.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <algorithm>
#include <array>
#include <functional>
using namespace compositor;
namespace {
QString buildDirectory;
std::shared_ptr<RuntimeResources> resourcesIn(const QString& path) {
 io::SpillLimits limits;limits.maxBytes=64*1024*1024;limits.maxPayloadBytes=2*1024*1024;limits.maxEntries=256;limits.maxIoOperations=1;
 return std::make_shared<RuntimeResources>(ResourceLimits::forMachine(8ull*1024*1024*1024,2),[]{return MemorySample{4ull*1024*1024*1024,64ull*1024*1024};},std::make_shared<io::SpillStore>(path.toStdString(),limits));
}
QByteArray canonical(const engine::RasterSnapshot& raster){QByteArray result;for(auto y=raster.extent.y;y<raster.extent.y+raster.extent.height;++y)for(auto x=raster.extent.x;x<raster.extent.x+raster.extent.width;++x)for(const auto half:engine::pack(raster.pixel(x,y))){result.append(char(half&255));result.append(char(half>>8));}return result;}
QString drive(MainWindow& window,QAction& action,const std::function<QString(QDialog&)>& configure,bool cancel=false,const QString& captureDirectory={}) {
 const auto before=window.currentDocument();const auto dirty=window.currentDirty();bool configured=false,done=false;QString error;QTimer timer;timer.setInterval(10);QElapsedTimer elapsed;elapsed.start();
 QObject::connect(&timer,&QTimer::timeout,&window,[&]{auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());if(!dialog)return;
  if(elapsed.elapsed()>15000){error="Stack preview timed out";dialog->reject();return;}
  if(dialog->objectName()!="revisableAdjustmentsDialog"){error="Wrong stack dialog";dialog->reject();return;}
  if(window.currentDocument()!=before || window.currentDirty()!=dirty){error="Draft preview changed committed state";dialog->reject();return;}
  auto* buttons=dialog->findChild<QDialogButtonBox*>("revisableAdjustmentsButtons");if(!buttons){error="Missing buttons";dialog->reject();return;}
  if(!configured){if(!buttons->button(QDialogButtonBox::Ok)->isEnabled())return;error=configure(*dialog);configured=true;if(!error.isEmpty()){dialog->reject();return;}return;}
  if(!buttons->button(QDialogButtonBox::Ok)->isEnabled())return;
  if(!captureDirectory.isEmpty()) {
   const QDir capture(captureDirectory);
   if(!dialog->grab().save(capture.filePath("revisable-stack-dialog.png")) ||
      !window.grab().save(capture.filePath("revisable-stack-workspace.png"))) {
    error="Could not capture ready stack preview";dialog->reject();return;
   }
  }
  done=true;timer.stop();QTest::mouseClick(buttons->button(cancel?QDialogButtonBox::Cancel:QDialogButtonBox::Ok),Qt::LeftButton);
 });timer.start();action.trigger();timer.stop();if(!done && error.isEmpty())error="Draft closed before ready preview";return error;
}
}
class RevisableStackWorkflowTest final:public QObject {
 Q_OBJECT
private slots:
 void completeOrderedStackWorkflow();
};
void RevisableStackWorkflowTest::completeOrderedStackWorkflow() {
 QTemporaryDir directory(QDir(buildDirectory).filePath("revisable-stack-XXXXXX"));QVERIFY(directory.isValid());QImage input(12,8,QImage::Format_RGB32);
 for(int y=0;y<input.height();++y)for(int x=0;x<input.width();++x)input.setPixel(x,y,qRgb(35+x*8,60+y*7,105+x*4));
 const auto sourcePath=directory.filePath("source.png");QVERIFY(input.save(sourcePath));MainWindow window(nullptr,true,{},resourcesIn(directory.path()));window.show();QSignalSpy finished(&window,&MainWindow::operationFinished);
 window.openPath(sourcePath);QTRY_COMPARE_WITH_TIMEOUT(finished.count(),1,10000);QVERIFY(finished.last()[0].toBool());const auto original=window.currentDocument();QVERIFY(original);const auto source=original->singleLayer().raster;
 auto* action=window.findChild<QAction*>("revisableAdjustmentsAction");QVERIFY(action);QVERIFY(action->isEnabled());
 engine::AdjustmentParameters levels;levels.kind=engine::AdjustmentKind::Levels;levels.levels.gamma=2;
 engine::AdjustmentParameters balance;balance.kind=engine::AdjustmentKind::ColorBalance;balance.colorBalance={.4,-.2};
 const auto reference=[&](const std::vector<engine::AdjustmentParameters>& operations){engine::TileStore tiles(32*1024*1024);auto document=original;for(const auto& p:operations)document=engine::adjustLayer(document,document->singleLayer().id,tiles,p).finish(document->revision+1);return canonical(*document->singleLayer().raster);};
 const auto verifyStack=[&](const std::vector<engine::AdjustmentParameters>& operations){const auto& layer=window.currentDocument()->singleLayer();QVERIFY(layer.adjustments);QVERIFY(layer.adjustments->operations==operations);QCOMPARE(canonical(*layer.adjustments->source),canonical(*source));QCOMPARE(canonical(*layer.raster),reference(operations));};
 // Embedded control names are frozen with the dialog packet below.
 const auto addAndConfigure=[](QDialog& dialog)->QString {
  auto* list=dialog.findChild<QListWidget*>("revisableAdjustmentsStack");auto* kind=dialog.findChild<QComboBox*>("revisableAdjustmentsKind");auto* add=dialog.findChild<QPushButton*>("revisableAdjustmentsAdd");
  if(!list || !kind || !add)return "Missing stack controls";
  auto* remove=dialog.findChild<QPushButton*>("revisableAdjustmentsRemove");if(!remove)return "Missing Remove control";
  if(list->count()>0){list->setCurrentRow(0);QTest::mouseClick(remove,Qt::LeftButton);}
  if(list->count()!=0)return "Could not remove initial neutral operation";
  kind->setCurrentIndex(static_cast<int>(engine::AdjustmentKind::Levels));QTest::mouseClick(add,Qt::LeftButton);
  auto* gamma=dialog.findChild<QDoubleSpinBox*>("levelsGamma");if(!gamma)return "Missing Levels gamma";gamma->setValue(2);
  kind->setCurrentIndex(static_cast<int>(engine::AdjustmentKind::ColorBalance));QTest::mouseClick(add,Qt::LeftButton);
  auto* warmth=dialog.findChild<QDoubleSpinBox*>("balanceWarmth");auto* tint=dialog.findChild<QDoubleSpinBox*>("balanceTint");if(!warmth || !tint)return "Missing balance controls";warmth->setValue(40);tint->setValue(-20);
  if(list->count()!=2)return "Wrong stack length";
  return {};
 };
 const auto captureDirectory=qEnvironmentVariable("LUMAIRE_D2_STACK_CAPTURE_DIR");
 if(!captureDirectory.isEmpty()) {
  QVERIFY(QDir::isAbsolutePath(captureDirectory));
  QVERIFY(!QFileInfo::exists(captureDirectory));
  QVERIFY(QDir().mkpath(captureDirectory));
 }
 auto error=drive(window,*action,addAndConfigure,false,captureDirectory);QVERIFY2(error.isEmpty(),qPrintable(error));QTRY_COMPARE_WITH_TIMEOUT(finished.count(),2,10000);QVERIFY(finished.last()[0].toBool());verifyStack({levels,balance});const auto first=window.currentDocument();
 window.undoCurrent();QVERIFY(window.currentDocument()==original);window.redoCurrent();QVERIFY(window.currentDocument()==first);
 const auto project=directory.filePath("stack.cproj");window.saveCurrentTo(project);QTRY_COMPARE_WITH_TIMEOUT(finished.count(),3,10000);QVERIFY(finished.last()[0].toBool());window.openPath(project);QTRY_COMPARE_WITH_TIMEOUT(finished.count(),4,10000);QVERIFY(finished.last()[0].toBool());verifyStack({levels,balance});const auto reopened=window.currentDocument();
 const auto changeGamma=[](double value){return [value](QDialog& dialog)->QString{auto* list=dialog.findChild<QListWidget*>("revisableAdjustmentsStack");if(!list || list->count()!=2)return "Missing saved operations";list->setCurrentRow(0);auto* gamma=dialog.findChild<QDoubleSpinBox*>("levelsGamma");if(!gamma)return "Missing saved Levels controls";gamma->setValue(value);return {};};};
 levels.levels.gamma=.7;error=drive(window,*action,changeGamma(.7));QVERIFY2(error.isEmpty(),qPrintable(error));QTRY_COMPARE_WITH_TIMEOUT(finished.count(),5,10000);QVERIFY(finished.last()[0].toBool());verifyStack({levels,balance});const auto revised=window.currentDocument();window.undoCurrent();QVERIFY(window.currentDocument()==reopened);window.redoCurrent();QVERIFY(window.currentDocument()==revised);
 error=drive(window,*action,[](QDialog& dialog)->QString{auto* list=dialog.findChild<QListWidget*>("revisableAdjustmentsStack");auto* up=dialog.findChild<QPushButton*>("revisableAdjustmentsUp");if(!list || !up)return "Missing reorder controls";list->setCurrentRow(1);QTest::mouseClick(up,Qt::LeftButton);return {};});QVERIFY2(error.isEmpty(),qPrintable(error));QTRY_COMPARE_WITH_TIMEOUT(finished.count(),6,10000);QVERIFY(finished.last()[0].toBool());verifyStack({balance,levels});QVERIFY(reference({balance,levels})!=reference({levels,balance}));const auto reordered=window.currentDocument();
 const auto beforeCancel=finished.count();error=drive(window,*action,[](QDialog& dialog)->QString{auto* list=dialog.findChild<QListWidget*>("revisableAdjustmentsStack");if(!list)return "Missing reordered stack";list->setCurrentRow(1);auto* gamma=dialog.findChild<QDoubleSpinBox*>("levelsGamma");if(!gamma)return "Missing reordered Levels controls";gamma->setValue(2.5);return {};},true);QVERIFY2(error.isEmpty(),qPrintable(error));QCOMPARE(finished.count(),beforeCancel);QVERIFY(window.currentDocument()==reordered);window.undoCurrent();QVERIFY(window.currentDocument()==revised);window.redoCurrent();QVERIFY(window.currentDocument()==reordered);
 for(const auto jpeg:{false,true}){const auto outputPath=directory.filePath(jpeg?"ordered.jpg":"ordered.png");io::ExportOptions options;window.exportCurrentTo(outputPath,options);QTRY_COMPARE_WITH_TIMEOUT(finished.count(),beforeCancel+(jpeg?2:1),10000);QVERIFY(finished.last()[0].toBool());const QImage output(outputPath);QVERIFY(!output.isNull());QCOMPARE(output.size(),input.size());double totalError=0;for(int y=0;y<output.height();++y)for(int x=0;x<output.width();++x){const auto value=reordered->singleLayer().raster->pixel(x,y);const auto pixel=output.pixelColor(x,y);const std::array<double,3> expected{255*std::clamp(engine::encodeSrgb(value.r),0.f,1.f),255*std::clamp(engine::encodeSrgb(value.g),0.f,1.f),255*std::clamp(engine::encodeSrgb(value.b),0.f,1.f)};const std::array<int,3> actual{pixel.red(),pixel.green(),pixel.blue()};for(int c=0;c<3;++c){const auto difference=std::abs(actual[c]-expected[c]);if(!jpeg)QVERIFY(difference<=1);totalError+=difference;}}QVERIFY(totalError/(input.width()*input.height()*3)<(jpeg?5:1));}
 if(!captureDirectory.isEmpty()) {
  const auto finalProject=directory.filePath("ordered-editable.cproj");
  window.saveCurrentTo(finalProject);
  QTRY_COMPARE_WITH_TIMEOUT(finished.count(),beforeCancel+3,10000);
  QVERIFY2(finished.last()[0].toBool(),qPrintable(finished.last()[1].toString()));
  QVERIFY(window.currentDocument()==reordered);QVERIFY(!window.currentDirty());
  engine::TileStore reopenedTiles(32*1024*1024);
  const auto retained=io::loadProject(finalProject,reopenedTiles).document;
  QVERIFY(retained->singleLayer().adjustments);
  QVERIFY(retained->singleLayer().adjustments->operations==std::vector<engine::AdjustmentParameters>({balance,levels}));
  QCOMPARE(canonical(*retained->singleLayer().raster),canonical(*reordered->singleLayer().raster));
  const QDir capture(captureDirectory);
  QVERIFY(QFile::copy(finalProject,capture.filePath("revisable-stack-editable.cproj")));
  QVERIFY(QFile::copy(directory.filePath("ordered.png"),capture.filePath("revisable-stack-export.png")));
  QVERIFY(QFile::copy(directory.filePath("ordered.jpg"),capture.filePath("revisable-stack-export.jpg")));
 }
}
int main(int argc,char** argv){if(argc<2){qCritical("Pass existing disk-backed build directory");return 2;}buildDirectory=QDir(QString::fromLocal8Bit(argv[1])).absolutePath();QApplication app(argc,argv);RevisableStackWorkflowTest test;return QTest::qExec(&test,argc-1,argv+1);}
#include "revisable_stack_workflow_test.moc"
