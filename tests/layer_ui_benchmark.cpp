#include "app/window.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QScrollBar>
#include <QScreen>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QMetaEnum>
#include <QFile>
#include <QCryptographicHash>
#include <time.h>
#include <sys/resource.h>
#include <algorithm>
#include <map>
#include <cmath>
#include <iostream>

using namespace compositor;
namespace {
std::int64_t rawNanoseconds() {
    timespec value{};
    if(clock_gettime(CLOCK_MONOTONIC_RAW,&value)!=0) throw std::runtime_error("Cannot read monotonic raw clock");
    return static_cast<std::int64_t>(value.tv_sec)*1000000000+value.tv_nsec;
}
class BenchmarkApplication:public QApplication {
public:
    using QApplication::QApplication;
    bool measuring=false;
    int run=0,eventIndex=0;
    struct EventSample {std::string receiver;int type;double milliseconds;std::int64_t receipt,duration;int run,event;};
    std::vector<EventSample> events;
    bool notify(QObject* receiver,QEvent* event) override {
        if(!measuring) return QApplication::notify(receiver,event);
        const std::string name=receiver->metaObject()->className();const int type=static_cast<int>(event->type());
        const auto receipt=rawNanoseconds();const bool result=QApplication::notify(receiver,event);
        const auto duration=rawNanoseconds()-receipt;
        events.push_back({name,type,double(duration)/1e6,receipt,duration,run,eventIndex});return result;
    }
};
QJsonObject statistics(std::vector<double> values) {
    const auto count=values.size();double sum=0;for(auto value:values) sum+=value;
    const double mean=sum/static_cast<double>(count);double variance=0;
    for(auto value:values) variance+=(value-mean)*(value-mean);
    std::sort(values.begin(),values.end());
    const auto percentile=[&](double p){return values[static_cast<std::size_t>(std::ceil(p*static_cast<double>(count)))-1];};
    return {{"samples",static_cast<int>(count)},{"median_ms",percentile(.5)},{"p95_ms",percentile(.95)},
        {"p99_ms",percentile(.99)},{"maximum_ms",values.back()},{"variance_ms2",variance/static_cast<double>(count)}};
}
}
int main(int argc,char** argv) {
    BenchmarkApplication app(argc,argv);
    try {
        QTemporaryDir directory;if(!directory.isValid()) throw std::runtime_error("No benchmark temporary directory");
        const QString fixturePath=argc>1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("linux/benchmarks/fixtures/layers-10000.json");
        QFile fixture(fixturePath);
        if(!fixture.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot open frozen P08 fixture");
        const auto fixtureBytes=fixture.readAll();
        const auto fixtureObject=QJsonDocument::fromJson(fixtureBytes).object();
        const auto records=fixtureObject["layers"].toArray();
        const auto fixtureCanvas=fixtureObject["canvas"].toArray();
        if(records.size()!=10000 || fixtureCanvas.size()!=2) throw std::runtime_error("Wrong P08 fixture shape");
        auto raster=std::make_shared<const engine::RasterSnapshot>(engine::Id::generate(),engine::Extent{0,0,1,1});
        std::vector<engine::LayerNode> layers;layers.reserve(10000);
        std::vector<QString> stableIds;stableIds.reserve(10000);
        int hiddenRecords=0;
        for(const auto& value:records) {
            const auto record=value.toObject();
            if(!record["asset"].isNull() || !record["parent"].isNull() || record["type"].toString()!=QStringLiteral("paint"))
                throw std::runtime_error("Unsupported P08 frozen layer record");
            const auto id=record["id"].toString();
            engine::LayerNode node{engine::Id(id.toStdString())};node.raster=raster;
            node.siblingOrder=record["order"].toInt();node.visible=record["visible"].toBool();
            node.opacity=static_cast<float>(record["opacity"].toDouble());node.name=record["name"].toString().toStdString();
            hiddenRecords+=!node.visible;stableIds.push_back(id);layers.push_back(std::move(node));
        }
        auto document=std::make_shared<const engine::DocumentSnapshot>(engine::Id::generate(),fixtureCanvas[0].toInt(),fixtureCanvas[1].toInt(),96,std::move(layers));
        const auto path=directory.filePath("p08.cproj");io::saveProject(path,document);
        MainWindow window(nullptr,true);window.show();QSignalSpy finished(&window,&MainWindow::operationFinished);
        window.openPath(path);QElapsedTimer opening;opening.start();
        while(finished.empty() && opening.elapsed()<30000) {app.processEvents(QEventLoop::AllEvents,5);QThread::msleep(1);}
        if(finished.empty() || !finished.last()[0].toBool()) throw std::runtime_error("Benchmark fixture failed to open");
        auto* tree=window.findChild<QTreeWidget*>("layerTree");
        if(!tree || tree->topLevelItemCount()!=10000) throw std::runtime_error("Wrong benchmark layer count");
        const auto captured=window.currentDocument();
        const auto* canvas=window.currentCanvas();
        if(!canvas) throw std::runtime_error("No benchmark canvas");
        const int initialCanvasWidth=canvas->width(),initialCanvasHeight=canvas->height();
        const double initialCanvasDpr=canvas->devicePixelRatio();
        bool canvasExtentConstant=true;
        bool prescribedViewportMatch=qRound(initialCanvasWidth*initialCanvasDpr)==1920 &&
            qRound(initialCanvasHeight*initialCanvasDpr)==1080;
        tree->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        std::map<QString,QTreeWidgetItem*> items;
        for(int i=0;i<tree->topLevelItemCount();++i) {
            auto* item=tree->topLevelItem(i);items.emplace(item->data(0,Qt::UserRole).toString(),item);
        }
        if(items.size()!=stableIds.size()) throw std::runtime_error("Missing stable P08 layer IDs");
        std::vector<double> selection,scroll,drain;
        QJsonArray rawSamples,runStatistics;
        // Each repetition replays the whole frozen trace. Warm-up samples are
        // retained with a flag but excluded from scored statistics.
        for(int run=0;run<40;++run) {
            std::vector<double> runSelection,runScroll,runDrain;
            tree->verticalScrollBar()->setValue(0);
            for(int event=0;event<600;++event) {
                const bool measured=run>=10;app.run=run;app.eventIndex=event;app.measuring=measured;
                const int direction=(event/100)%2==0 ? 1 : -1;
                auto* bar=tree->verticalScrollBar();const int before=bar->value();
                const auto receipt=rawNanoseconds();
                bar->setValue(std::clamp(before+direction*120,bar->minimum(),bar->maximum()));
                const auto scrollDone=rawNanoseconds();const int after=bar->value();
                const auto id=stableIds[static_cast<std::size_t>((event*17)%10000)];
                const auto selectionStart=rawNanoseconds();tree->setCurrentItem(items.at(id));
                const auto selectedDone=rawNanoseconds();app.measuring=false;
                if(!tree->currentItem() || tree->currentItem()->data(0,Qt::UserRole).toString()!=id)
                    throw std::runtime_error("P08 selected wrong stable layer ID");
                if(window.currentDocument()!=captured || window.currentDirty())
                    throw std::runtime_error("Selection/scroll mutated document pixels");
                const auto drainStart=rawNanoseconds();app.measuring=measured;
                app.processEvents(QEventLoop::AllEvents,5);app.measuring=false;
                const auto drainedDone=rawNanoseconds();
                canvasExtentConstant=canvasExtentConstant && canvas->width()==initialCanvasWidth &&
                    canvas->height()==initialCanvasHeight && canvas->devicePixelRatio()==initialCanvasDpr;
                prescribedViewportMatch=prescribedViewportMatch &&
                    qRound(canvas->width()*canvas->devicePixelRatio())==1920 &&
                    qRound(canvas->height()*canvas->devicePixelRatio())==1080;
                const auto append=[&](const char* metric,std::int64_t start,std::int64_t finish) {
                    rawSamples.append(QJsonObject{{"run",run},{"event",event},{"warmup",!measured},
                        {"revision",qint64(captured->revision)},{"metric",metric},{"receipt_ns",qint64(start)},
                        {"duration_ns",qint64(finish-start)},{"selected_id",id},{"scroll_before",before},
                        {"scroll_after",after},{"requested_logical_pixels",direction*120}});
                };
                append("scroll_handler",receipt,scrollDone);append("selection_handler",selectionStart,selectedDone);
                append("event_drain",drainStart,drainedDone);
                runSelection.push_back(double(selectedDone-selectionStart)/1e6);
                runScroll.push_back(double(scrollDone-receipt)/1e6);runDrain.push_back(double(drainedDone-drainStart)/1e6);
                if(measured) {selection.push_back(double(selectedDone-selectionStart)/1e6);
                    scroll.push_back(double(scrollDone-receipt)/1e6);drain.push_back(double(drainedDone-drainStart)/1e6);}
            }
            runStatistics.append(QJsonObject{{"run",run},{"warmup",run<10},
                {"selection_handler",statistics(runSelection)},{"scroll_handler",statistics(runScroll)},
                {"event_drain",statistics(runDrain)}});
        }
        struct rusage usage{};getrusage(RUSAGE_SELF,&usage);
        const auto selected=statistics(selection),scrolled=statistics(scroll);
        std::map<std::pair<std::string,int>,std::vector<double>> byEvent;
        QJsonArray rawEventSamples;
        std::map<int,std::map<std::pair<std::string,int>,std::vector<double>>> byRunEvent;
        for(const auto& event:app.events) {
            byEvent[{event.receiver,event.type}].push_back(event.milliseconds);
            byRunEvent[event.run][{event.receiver,event.type}].push_back(event.milliseconds);
            rawEventSamples.append(QJsonObject{{"run",event.run},{"event",event.event},{"revision",qint64(captured->revision)},
                {"receiver_class",QString::fromStdString(event.receiver)},{"event_type",event.type},
                {"receipt_ns",qint64(event.receipt)},{"duration_ns",qint64(event.duration)}});
        }
        QJsonArray eventHandlers;double worstEventP95=0;
        for(const auto& [key,samples]:byEvent) {
            auto entry=statistics(samples);entry["receiver_class"]=QString::fromStdString(key.first);entry["event_type"]=key.second;
            const auto* name=QMetaEnum::fromType<QEvent::Type>().valueToKey(static_cast<quint64>(key.second));
            entry["event_name"]=name ? QString::fromLatin1(name) : QString::number(key.second);
            worstEventP95=std::max(worstEventP95,entry["p95_ms"].toDouble());eventHandlers.append(entry);
        }
        QJsonArray runEventStatistics;
        for(const auto& [run,handlers]:byRunEvent) {
            QJsonArray entries;double worstP95=0;
            for(const auto& [key,samples]:handlers) {
                auto entry=statistics(samples);entry["receiver_class"]=QString::fromStdString(key.first);
                entry["event_type"]=key.second;worstP95=std::max(worstP95,entry["p95_ms"].toDouble());
                entries.append(entry);
            }
            runEventStatistics.append(QJsonObject{{"run",run},{"event_handlers",entries},
                {"worst_event_handler_p95_ms",worstP95},{"diagnostic_handler_target_pass",worstP95<=4}});
        }
        const QJsonObject canvasExtent{{"logical_width",canvas->width()},{"logical_height",canvas->height()},
            {"device_width",qRound(canvas->width()*canvas->devicePixelRatio())},
            {"device_height",qRound(canvas->height()*canvas->devicePixelRatio())},{"dpr",canvas->devicePixelRatio()},
            {"initial_logical_width",initialCanvasWidth},{"initial_logical_height",initialCanvasHeight},
            {"initial_dpr",initialCanvasDpr},{"extent_constant_during_trace",canvasExtentConstant}};
        const auto* screen=window.screen();
        const QJsonObject display{{"name",screen->name()},{"width",screen->size().width()},{"height",screen->size().height()},
            {"refresh_hz",screen->refreshRate()},{"window_dpr",window.devicePixelRatio()},
            {"window_width",window.width()},{"window_height",window.height()}};
        QJsonObject result{{"workload","P08"},{"records",10000},{"hidden_records",hiddenRecords},{"empty_rasters",10000},
            {"warmup_runs",10},{"measured_runs",30},{"events_per_run",600},{"run_statistics",runStatistics},{"run_event_statistics",runEventStatistics},
            {"canvas",canvasExtent},{"prescribed_viewport_device_width",1920},{"prescribed_viewport_device_height",1080},
            {"prescribed_viewport_match",prescribedViewportMatch},
            {"clock","CLOCK_MONOTONIC_RAW"},{"raw_samples",rawSamples},{"raw_event_samples",rawEventSamples},
            {"fixture_sha256",QString::fromLatin1(QCryptographicHash::hash(fixtureBytes,QCryptographicHash::Sha256).toHex())},{"selection_handler",selected},{"scroll_handler",scrolled},
            {"event_drain",statistics(drain)},{"peak_rss_kib",qint64(usage.ru_maxrss)},
            {"event_handlers",eventHandlers},{"worst_event_handler_p95_ms",worstEventP95},
            {"process_user_cpu_seconds",double(usage.ru_utime.tv_sec)+double(usage.ru_utime.tv_usec)/1e6},
            {"process_system_cpu_seconds",double(usage.ru_stime.tv_sec)+double(usage.ru_stime.tv_usec)/1e6},
            {"display",display},{"gpu_memory_bytes",QJsonValue()},{"transfer_bytes",QJsonValue()},
            {"platform",QGuiApplication::platformName()},{"compiler",__VERSION__},{"qt",QT_VERSION_STR},
            {"window_exposed",window.windowHandle() && window.windowHandle()->isExposed()},
            {"build",COMPOSITOR_BENCHMARK_BUILD},{"handler_target_ms",4},
            {"handler_target_pass",selected["p95_ms"].toDouble()<=4 && scrolled["p95_ms"].toDouble()<=4 && worstEventP95<=4},
            {"document_identity_preserved",true},{"hardware_qualification",false}};
        std::cout<<QJsonDocument(result).toJson(QJsonDocument::Indented).constData();
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
