#include "app/development_demo.h"
#include "app/window.h"
#include "core/editing_types.h"
#include "io/image_export.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSaveFile>
#include <QStatusBar>
#include <QTimer>
#include <QDebug>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace compositor {
namespace {
void require(bool value,const char* message) {if(!value) throw std::runtime_error(message);}
class Demo final:public QObject {
public:
    Demo(MainWindow& window,QString directory):QObject(&window),window_(window),directory_(std::move(directory)) {
        watchdog_.setSingleShot(true);watchdog_.setInterval(120000);
        connect(&watchdog_,&QTimer::timeout,this,[this]{fail("Demo operation timed out; the window remains open");});
        connect(&window_,&MainWindow::operationFinished,this,[this](bool success,const QString& information) {
            if(!waiting_) return;
            watchdog_.stop();waiting_=false;
            log(success ? "step_complete" : "step_failed",steps_.at(index_).name,information);
            if(!success) {fail(information);return;}
            if(index_==0) page_=window_.tabs()->currentWidget();
            ++index_;QTimer::singleShot(0,this,[this]{next();});
        });
    }
    void start() {
        try {
            require(QDir::isAbsolutePath(directory_),"Development demo requires an absolute output directory");
            require(QDir().mkpath(directory_),"Cannot create development demo directory");
            require(!window_.currentBusy(),"Wait for the current editor operation before starting the demo");
            // Refuse named symlink/nonregular artifacts before repeatable
            // overwrite, and replace any old success report with current state.
            for(const auto* name:{"source.png","edited.cproj","edited.png","edited.jpg","delivery-report.json"})
                (void)io::fileIdentity(path(name));
            writeReport(QJsonObject{{"schema_version",1},{"complete",false},{"qualification",false},{"state","running"}});
            const auto source=path("source.png");
            QImage image(256,192,QImage::Format_ARGB32);
            for(int y=0;y<image.height();++y) for(int x=0;x<image.width();++x) {
                const bool exterior=x<8 || y<8 || x>=248 || y>=184;
                const int checker=((x/24+y/24)&1) ? 20 : 0;
                const int red=45+(x*125)/255+checker;
                const int green=65+(y*100)/191;
                const int blue=190-(x*70)/255;
                image.setPixelColor(x,y,QColor(red,green,blue,exterior ? 0 : 255));
            }
            image.setDotsPerMeterX(3780);image.setDotsPerMeterY(3780);
            QSaveFile input(source);input.setDirectWriteFallback(false);
            require(input.open(QIODevice::WriteOnly),"Cannot create demo source image");
            require(image.save(&input,"PNG") && input.commit(),"Cannot save demo source image");
            steps_.push_back({"Import PNG",[this,source]{window_.openPath(source);}});
            steps_.push_back({"Paint brush stroke",[this] {
                engine::BrushSettings brush;brush.diameter=18;brush.hardness=.6;brush.opacity=.9;
                brush.color={1,.12f,.28f,.85f};
                window_.applyStroke({{35,132},{67,113},{99,93},{132,86},{166,101},{204,128}},brush);
            }});
            steps_.push_back({"Create ellipse selection",[this] {
                window_.setSelectionCurrent(engine::Selection{{16,12,224,164},engine::SelectionShape::Ellipse,false});
            }});
            steps_.push_back({"Create mask from selection",[this]{window_.createMaskCurrent(true);}});
            steps_.push_back({"Adjust exposure",[this] {
                window_.applyAdjustment({engine::AdjustmentKind::Exposure,.4});
            }});
            steps_.push_back({"Adjust levels",[this] {
                engine::AdjustmentParameters levels;levels.kind=engine::AdjustmentKind::Levels;
                levels.levels.gamma=1.1;window_.applyAdjustment(levels);
            }});
            steps_.push_back({"Adjust curves",[this] {
                engine::AdjustmentParameters curves;curves.kind=engine::AdjustmentKind::Curves;
                curves.curve={{0,0},{.25,.28},{.5,.53},{.75,.77},{1,1}};window_.applyAdjustment(curves);
            }});
            steps_.push_back({"Adjust color balance",[this] {
                engine::AdjustmentParameters balance;balance.kind=engine::AdjustmentKind::ColorBalance;
                balance.colorBalance={.08,-.03};window_.applyAdjustment(balance);
            }});
            steps_.push_back({"Clear selection",[this]{window_.setSelectionCurrent(std::nullopt);}});
            steps_.push_back({"Transform layer",[this] {
                engine::TransformParameters transform;transform.dx=5;transform.dy=-2;transform.rotationDegrees=3;
                window_.transformCurrent(transform);
            }});
            steps_.push_back({"Crop canvas",[this]{window_.cropCurrent({2,2,252,188});}});
            steps_.push_back({"Resize canvas",[this]{window_.resizeCurrent(224,168);}});
            steps_.push_back({"Save editable project",[this]{window_.saveCurrentTo(path("edited.cproj"),true);}});
            steps_.push_back({"Export PNG",[this] {
                io::ExportOptions options;options.expected=io::fileIdentity(path("edited.png"));
                window_.exportCurrentTo(path("edited.png"),options);
            }});
            steps_.push_back({"Export JPEG",[this] {
                io::ExportOptions options;options.quality=95;options.expected=io::fileIdentity(path("edited.jpg"));
                window_.exportCurrentTo(path("edited.jpg"),options);
            }});
            next();
        } catch(const std::exception& error) {fail(QString::fromUtf8(error.what()));}
    }
private:
    struct Step {QString name;std::function<void()> run;};
    QString path(const char* name) const {return QDir(directory_).filePath(QString::fromUtf8(name));}
    void log(const char* event,const QString& step,const QString& message) {
        qInfo().noquote()<<QJsonDocument(QJsonObject{{"event",QString::fromUtf8(event)},
            {"step",step},{"message",message},{"directory",directory_}}).toJson(QJsonDocument::Compact);
    }
    void writeReport(const QJsonObject& report) {
        QSaveFile output(path("delivery-report.json"));output.setDirectWriteFallback(false);
        require(output.open(QIODevice::WriteOnly),"Cannot create development demo report");
        const auto bytes=QJsonDocument(report).toJson(QJsonDocument::Indented);
        require(output.write(bytes)==bytes.size() && output.commit(),"Cannot save development demo report");
    }
    void next() {
        if(finished_) return;
        try {
            if(index_==steps_.size()) {complete();return;}
            require(!window_.currentBusy(),"Another operation interrupted the development demo");
            if(index_>0) require(page_ && window_.tabs()->currentWidget()==page_,"The demo tab changed; demonstration stopped");
            window_.statusBar()->showMessage("Development demo: "+steps_.at(index_).name);
            log("step_started",steps_.at(index_).name,{});waiting_=true;watchdog_.start();
            steps_.at(index_).run();
            require(!waiting_ || window_.currentBusy(),"Demo command did not start a worker operation");
        } catch(const std::exception& error) {fail(QString::fromUtf8(error.what()));}
    }
    void complete() {
        const auto document=window_.currentDocument();
        require(bool(document) && !window_.currentDirty(),"Saved demo project is missing or unexpectedly dirty");
        QJsonObject artifacts;
        for(const auto* name:{"source.png","edited.cproj","edited.png","edited.jpg"}) {
            QFile file(path(name));require(file.open(QIODevice::ReadOnly),"Demo output artifact is missing");
            QCryptographicHash hash(QCryptographicHash::Sha256);
            while(!file.atEnd()) {const auto block=file.read(65536);require(!block.isEmpty() || file.atEnd(),"Cannot read demo artifact");hash.addData(block);}
            artifacts.insert(QString::fromUtf8(name),QJsonObject{{"path",path(name)},
                {"bytes",file.size()},{"sha256",QString::fromLatin1(hash.result().toHex())}});
        }
        QJsonObject report{{"schema_version",1},{"complete",true},{"qualification",false},
            {"width",document->width},{"height",document->height},{"revision",static_cast<qint64>(document->revision)},
            {"layers",static_cast<qint64>(document->layers().size())},{"steps",static_cast<qint64>(steps_.size())},
            {"backend",window_.currentCanvas()->cpu() ? "cpu" : "vulkan"},{"artifacts",artifacts}};
        writeReport(report);
        finished_=true;watchdog_.stop();
        window_.statusBar()->showMessage("Demo complete: editable project, PNG and JPEG saved in "+directory_);
        log("development_demo_complete",{},"Saved editable project and PNG/JPEG exports; window remains open");
        deleteLater();
    }
    void fail(const QString& message) {
        if(finished_) return;
        finished_=true;waiting_=false;watchdog_.stop();
        if(QDir::isAbsolutePath(directory_) && QFileInfo(directory_).isDir()) {
            try {
                (void)io::fileIdentity(path("delivery-report.json"));
                writeReport(QJsonObject{{"schema_version",1},{"complete",false},{"qualification",false},
                    {"state","failed"},{"message",message},{"completed_steps",static_cast<qint64>(index_)}});
            } catch(const std::exception& error) {qWarning()<<"Cannot save demo failure report:"<<error.what();}
        }
        window_.statusBar()->showMessage("Development demo stopped: "+message);
        log("development_demo_failed",index_<steps_.size() ? steps_[index_].name : QString{},message);
        deleteLater();
    }
    MainWindow& window_;
    QString directory_;
    QPointer<QWidget> page_;
    QTimer watchdog_;
    std::vector<Step> steps_;
    std::size_t index_=0;
    bool waiting_=false,finished_=false;
};
}
void runDevelopmentDemo(MainWindow& window,const QString& directory) {
    auto* demo=new Demo(window,directory);
    QTimer::singleShot(0,demo,[demo]{demo->start();});
}
}
