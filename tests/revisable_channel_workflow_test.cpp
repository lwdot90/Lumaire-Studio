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
#include <cmath>
#include <sqlite3.h>
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
   if(!dialog->grab().save(capture.filePath("revisable-channel-dialog.png")) ||
      !window.grab().save(capture.filePath("revisable-channel-workspace.png"))) {
    error="Could not capture ready stack preview";dialog->reject();return;
   }
  }
  done=true;timer.stop();QTest::mouseClick(buttons->button(cancel?QDialogButtonBox::Cancel:QDialogButtonBox::Ok),Qt::LeftButton);
 });timer.start();action.trigger();timer.stop();if(!done && error.isEmpty())error="Draft closed before ready preview";return error;
}
}
namespace {
const std::vector<engine::CurvePoint> lift{{0,0},{.25,.38},{.5,.62},{.75,.82},{1,1}};
const std::vector<engine::CurvePoint> contrast{{0,0},{.25,.18},{.5,.5},{.75,.82},{1,1}};
double encode(double value){return value<=.0031308?12.92*value:1.055*std::pow(value,1/2.4)-.055;}
double decode(double value){return value<=.04045?value/12.92:std::pow((value+.055)/1.055,2.4);}
double mapped(double value,const std::vector<engine::CurvePoint>& curve){for(std::size_t i=1;i<curve.size();++i)if(value<=curve[i].input){const auto& a=curve[i-1];const auto& b=curve[i];return a.output+(b.output-a.output)*(value-a.input)/(b.input-a.input);}return curve.back().output;}
engine::PackedPixel expected(engine::Pixel source,const engine::AdjustmentParameters& adjustment){const std::array<float,3> channels{source.r,source.g,source.b};std::array<float,3> output{};for(int c=0;c<3;++c)output[c]=float(decode(mapped(mapped(encode(channels[c]/source.a),adjustment.curve),adjustment.channelCurves[c]))*source.a);return engine::pack({output[0],output[1],output[2],source.a});}
QString setChannel(QDialog& dialog,int channel,int preset){auto* selector=dialog.findChild<QComboBox*>("curveChannel");auto* presets=dialog.findChild<QComboBox*>("curvePreset");if(!selector || !presets)return "Missing channel controls";selector->setCurrentIndex(channel);presets->setCurrentIndex(preset);return {};}
}
class RevisableChannelWorkflowTest final:public QObject {
 Q_OBJECT
private slots:
 void completeChannelWorkflow();
};
void RevisableChannelWorkflowTest::completeChannelWorkflow(){
 QTemporaryDir directory(QDir(buildDirectory).filePath("revisable-channel-XXXXXX"));QVERIFY(directory.isValid());
 QImage input(12,8,QImage::Format_RGBA8888);input.fill(QColor(128,96,64,128));const auto imagePath=directory.filePath("source.png");QVERIFY(input.save(imagePath));
 MainWindow window(nullptr,true,{},resourcesIn(directory.path()));window.show();QSignalSpy finished(&window,&MainWindow::operationFinished);
 window.openPath(imagePath);QTRY_COMPARE_WITH_TIMEOUT(finished.count(),1,10000);QVERIFY(finished.last()[0].toBool());
 const auto original=window.currentDocument();const auto source=original->singleLayer().raster;const auto sourcePixel=source->pixel(0,0);
 auto* action=window.findChild<QAction*>("revisableAdjustmentsAction");QVERIFY(action);QVERIFY(action->isEnabled());
 const auto captureDirectory=qEnvironmentVariable("LUMAIRE_D2_CHANNEL_CAPTURE_DIR");if(!captureDirectory.isEmpty()){QVERIFY(QDir::isAbsolutePath(captureDirectory));QVERIFY(!QFileInfo::exists(captureDirectory));QVERIFY(QDir().mkpath(captureDirectory));}
 const auto configure=[](QDialog& dialog)->QString{
  auto* list=dialog.findChild<QListWidget*>("revisableAdjustmentsStack");auto* remove=dialog.findChild<QPushButton*>("revisableAdjustmentsRemove");auto* kind=dialog.findChild<QComboBox*>("revisableAdjustmentsKind");auto* add=dialog.findChild<QPushButton*>("revisableAdjustmentsAdd");
  if(!list || !remove || !kind || !add)return "Missing stack controls";
  list->setCurrentRow(0);QTest::mouseClick(remove,Qt::LeftButton);kind->setCurrentIndex(int(engine::AdjustmentKind::Curves));QTest::mouseClick(add,Qt::LeftButton);
  auto error=setChannel(dialog,1,1);if(!error.isEmpty())return error;return setChannel(dialog,3,2);
 };
 auto error=drive(window,*action,configure,false,captureDirectory);QVERIFY2(error.isEmpty(),qPrintable(error));QTRY_COMPARE_WITH_TIMEOUT(finished.count(),2,10000);QVERIFY(finished.last()[0].toBool());
 engine::AdjustmentParameters curves;curves.kind=engine::AdjustmentKind::Curves;curves.channelCurves[0]=lift;curves.channelCurves[2]=contrast;
 const auto verify=[&](const engine::AdjustmentParameters& parameters){const auto& layer=window.currentDocument()->singleLayer();QVERIFY(layer.adjustments);QVERIFY(layer.adjustments->operations==std::vector<engine::AdjustmentParameters>{parameters});QCOMPARE(canonical(*layer.adjustments->source),canonical(*source));QCOMPARE(engine::pack(layer.raster->pixel(0,0)),expected(sourcePixel,parameters));QCOMPARE(layer.raster->pixel(0,0).a,sourcePixel.a);};
 verify(curves);const auto first=window.currentDocument();window.undoCurrent();QVERIFY(window.currentDocument()==original);window.redoCurrent();QVERIFY(window.currentDocument()==first);
 const auto project=directory.filePath("channels.cproj");window.saveCurrentTo(project);QTRY_COMPARE_WITH_TIMEOUT(finished.count(),3,10000);QVERIFY(finished.last()[0].toBool());
 sqlite3* database=nullptr;QCOMPARE(sqlite3_open_v2(QFile::encodeName(project).constData(),&database,SQLITE_OPEN_READONLY,nullptr),SQLITE_OK);sqlite3_stmt* statement=nullptr;QCOMPARE(sqlite3_prepare_v2(database,"PRAGMA user_version",-1,&statement,nullptr),SQLITE_OK);QCOMPARE(sqlite3_step(statement),SQLITE_ROW);const auto version=sqlite3_column_int(statement,0);sqlite3_finalize(statement);sqlite3_close(database);QCOMPARE(version,6);
 window.openPath(project);QTRY_COMPARE_WITH_TIMEOUT(finished.count(),4,10000);QVERIFY(finished.last()[0].toBool());verify(curves);const auto reopened=window.currentDocument();
 error=drive(window,*action,[](QDialog& dialog){return setChannel(dialog,1,2);});QVERIFY2(error.isEmpty(),qPrintable(error));QTRY_COMPARE_WITH_TIMEOUT(finished.count(),5,10000);QVERIFY(finished.last()[0].toBool());curves.channelCurves[0]=contrast;verify(curves);const auto revised=window.currentDocument();window.undoCurrent();QVERIFY(window.currentDocument()==reopened);window.redoCurrent();QVERIFY(window.currentDocument()==revised);
 error=drive(window,*action,[](QDialog& dialog){return setChannel(dialog,3,1);},true);QVERIFY2(error.isEmpty(),qPrintable(error));QCOMPARE(finished.count(),5);QVERIFY(window.currentDocument()==revised);verify(curves);
 for(const auto jpeg:{false,true}){
  const auto outputPath=directory.filePath(jpeg?"channels.jpg":"channels.png");window.exportCurrentTo(outputPath);QTRY_COMPARE_WITH_TIMEOUT(finished.count(),jpeg?7:6,10000);QVERIFY(finished.last()[0].toBool());const QImage output(outputPath);QVERIFY(!output.isNull());QCOMPARE(output.size(),input.size());const auto value=revised->singleLayer().raster->pixel(0,0);const std::array<float,3> channels{value.r,value.g,value.b};
  for(int y=0;y<output.height();++y)for(int x=0;x<output.width();++x){const auto pixel=output.pixelColor(x,y);const std::array<int,3> actual{pixel.red(),pixel.green(),pixel.blue()};for(int c=0;c<3;++c){const auto target=255*encode(jpeg?channels[c]+1-value.a:channels[c]/value.a);QVERIFY2(std::abs(static_cast<double>(actual[c])-target)<=(jpeg?4:1),"Decoded export differs from adjusted pixels");}QVERIFY(std::abs(static_cast<double>(pixel.alpha())-(jpeg?255.0:255.0*static_cast<double>(value.a)))<=1);}
 }
 if(!captureDirectory.isEmpty()){
  const auto finalProject=directory.filePath("channels-editable.cproj");window.saveCurrentTo(finalProject);QTRY_COMPARE_WITH_TIMEOUT(finished.count(),8,10000);QVERIFY(finished.last()[0].toBool());const QDir capture(captureDirectory);QVERIFY(QFile::copy(finalProject,capture.filePath("revisable-channel-editable.cproj")));QVERIFY(QFile::copy(directory.filePath("channels.png"),capture.filePath("revisable-channel-export.png")));QVERIFY(QFile::copy(directory.filePath("channels.jpg"),capture.filePath("revisable-channel-export.jpg")));engine::TileStore tiles(32*1024*1024);const auto retained=io::loadProject(finalProject,tiles).document;QVERIFY(retained->singleLayer().adjustments->operations==std::vector<engine::AdjustmentParameters>{curves});QCOMPARE(canonical(*retained->singleLayer().raster),canonical(*revised->singleLayer().raster));
 }
}
int main(int argc,char** argv){if(argc<2){qCritical("Pass existing disk-backed build directory");return 2;}buildDirectory=QDir(QString::fromLocal8Bit(argv[1])).absolutePath();QApplication app(argc,argv);RevisableChannelWorkflowTest test;return QTest::qExec(&test,argc-1,argv+1);}
#include "revisable_channel_workflow_test.moc"
