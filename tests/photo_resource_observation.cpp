#include "core/editor_commands.h"
#include "core/resources.h"
#include "core/spill_coordinator.h"
#include "io/project_store.h"
#include "io/image_export.h"
#include "io/spill_store.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <algorithm>
#include <utility>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace compositor;
namespace {
using Clock=std::chrono::steady_clock;
constexpr std::uint64_t mib=1024*1024,gib=1024*mib;
void require(bool condition,const char* message) {if(!condition) throw std::runtime_error(message);}
void emitRecord(const QJsonObject& object) {std::cout<<QJsonDocument(object).toJson(QJsonDocument::Compact).constData()<<'\n'<<std::flush;}
std::uint64_t highWater() {
    std::ifstream input("/proc/self/status");std::string line;
    while(std::getline(input,line)) {std::istringstream row(line);std::string key,unit;std::uint64_t value;
        if(row>>key>>value>>unit && key=="VmHWM:" && unit=="kB") return value*1024;}
    throw std::runtime_error("Cannot observe Linux VmHWM");
}
void observe(const char* stage,const RuntimeResources& resources,Clock::time_point started) {
    const auto sample=MemorySample::read();const auto admitted=resources.memory->snapshot();const auto disk=resources.spill->stats();
    require(sample.resident && sample.available,"Actual process/system memory samples required");
    emitRecord({{"stage",stage},{"elapsed_ms",static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-started).count())},
        {"rss",static_cast<double>(*sample.resident)},{"vm_hwm",static_cast<double>(highWater())},{"mem_available",static_cast<double>(*sample.available)},
        {"admitted_cpu",static_cast<double>(admitted.committedCpu)},{"pending_cpu",static_cast<double>(admitted.pendingCpu)},
        {"tile_store_used",static_cast<double>(resources.tiles->usedBytes())},{"spill_bytes",static_cast<double>(disk.bytes)},
        {"spill_entries",static_cast<double>(disk.entries)},{"spill_active_io",static_cast<double>(disk.activeIo)},
        {"spill_cleanup_failures",static_cast<double>(disk.cleanupFailures)}});
}
engine::Pixel gradient(int x,int y) {
    return engine::fromStraightSrgb({.05f+.8f*static_cast<float>(x)/3999.f,.05f+.8f*static_cast<float>(y)/2999.f,.1f+.6f*static_cast<float>(x+y)/6998.f,1});
}
engine::PackedPixel sample(const engine::DocumentPtr& document,int x,int y) {
    return engine::pack(document->singleLayer().raster->pixel(x,y));
}
}
int main(int argc,char** argv) {
    QCoreApplication application(argc,argv);const auto started=Clock::now();
    try {
        require(argc<=2,"Supply at most one existing absolute disk-backed output directory");
        const auto base=argc==2 ? QString::fromLocal8Bit(argv[1]) : QCoreApplication::applicationDirPath();
        require(QDir::isAbsolutePath(base) && QFileInfo(base).isDir() && !QFileInfo(base).isSymLink(),"Output directory must be an existing absolute nonsymlink directory");
        QTemporaryDir directory(QDir(base).filePath("photo-observation-XXXXXX"));require(directory.isValid(),"Create private observation directory");
        io::SpillLimits limits;limits.maxBytes=gib;limits.maxPayloadBytes=2*mib;limits.maxEntries=8192;limits.maxIoOperations=1;limits.minFreeBytes=256*mib;
        auto disk=std::make_shared<io::SpillStore>(directory.path().toStdString(),limits);
        auto resources=std::make_shared<RuntimeResources>(ResourceLimits::forMachine(8*gib,2),MemorySample::read,disk);
        emitRecord({{"kind","bounded_development_observation"},{"full_lp8_qualification",false},{"width",4000},{"height",3000},{"output_directory",directory.path()}});
        observe("baseline",*resources,started);
        {
            engine::DocumentHistory history(engine::blankDocument(4000,3000));
            {
                auto permit=resources->compute.acquire(WorkScheduler::Priority::Processing,true);require(permit.has_value(),"Admit generation");
                auto edit=history.begin();
                for(int ty=0;ty<3000;ty+=256) for(int tx=0;tx<4000;tx+=256) {
                    const int width=std::min(256,4000-tx),height=std::min(256,3000-ty);
                    auto charge=resources->memory->require(static_cast<std::uint64_t>(width)*height*sizeof(engine::Pixel));
                    std::vector<engine::Pixel> pixels(static_cast<std::size_t>(width)*height);charge.commit();
                    for(int y=0;y<height;++y) for(int x=0;x<width;++x) pixels[static_cast<std::size_t>(y)*width+x]=gradient(tx+x,ty+y);
                    edit.write(*resources->tiles,{tx/256,ty/256},pixels);
                }
                require(history.commit(edit,"Generated gradient"),"Publish tiled gradient");
            }
            auto source=history.current();
            for(const auto point:{std::pair{0,0},std::pair{1999,1499},std::pair{3999,2999}})
                require(sample(source,point.first,point.second)==engine::pack(gradient(point.first,point.second)),"Generated canonical gradient differs");
            observe("gradient_generated",*resources,started);
            engine::AdjustmentParameters levels;levels.kind=engine::AdjustmentKind::Levels;levels.levels.gamma=1.2;
            engine::AdjustmentParameters curves;curves.kind=engine::AdjustmentKind::Curves;curves.curve={{0,0},{.25,.35},{.5,.55},{1,1}};
            engine::AdjustmentParameters balance;balance.kind=engine::AdjustmentKind::ColorBalance;balance.colorBalance={.1,-.1};
            for(const auto& entry:{std::pair{"levels",levels},std::pair{"curves",curves},std::pair{"color_balance",balance}}) {
                auto permit=resources->compute.acquire(WorkScheduler::Priority::Processing,true);require(permit.has_value(),"Admit adjustment");
                const auto before=history.current();
                auto edit=engine::adjustLayer(before,before->singleLayer().id,*resources->tiles,entry.second);
                require(history.commit(edit,entry.first),"Adjustment produced no change");observe(entry.first,*resources,started);
            }
            const auto final=history.current();require(final!=source,"Source snapshot must remain retained independently");
            require(history.undo() && history.redo() && history.current()==final,"Undo/redo did not restore exact snapshot");
            const auto project=directory.filePath("gradient.cproj");
            {
                auto permit=resources->compute.acquire(WorkScheduler::Priority::Processing,true);require(permit.has_value(),"Admit project save");
                io::SaveOptions options;options.memory=resources->memory;io::saveProject(project,final,options);
            }
            observe("project_saved",*resources,started);
            {
                auto permit=resources->compute.acquire(WorkScheduler::Priority::Processing,true);require(permit.has_value(),"Admit reopen");
                const auto reopened=io::loadProject(project,*resources->tiles).document;
                require(reopened->id==final->id && reopened->revision==final->revision && reopened->singleLayer().raster->id==final->singleLayer().raster->id,"Saved identities differ");
                for(const auto point:{std::pair{0,0},std::pair{255,256},std::pair{1999,1499},std::pair{3999,2999}})
                    require(sample(reopened,point.first,point.second)==sample(final,point.first,point.second),"Reopened canonical sample differs");
            }
            observe("project_reopened",*resources,started);
            for(const auto suffix:{"png","jpg"}) {
                auto permit=resources->compute.acquire(WorkScheduler::Priority::Processing,true);require(permit.has_value(),"Admit streamed export");
                io::ExportOptions options;options.memory=resources->memory;
                io::exportImage(directory.filePath(QString("gradient.")+suffix),final,options);observe(suffix,*resources,started);
            }
            // A tiny independent refusal fixture uses the actual host probe, never a permissive synthetic sample.
            auto tiny=engine::blankDocument(16,16);auto limited=std::make_shared<MemoryAdmission>(1,512*mib,MemorySample::read);
            engine::TileStore refused(4*mib,limited);engine::EditTransaction tinyEdit(tiny);
            tinyEdit.replace({0,0},resources->tiles->constant(16,16,{.2f,.3f,.4f,1}));tiny=tinyEdit.finish(1);
            bool denied=false;try {engine::adjustLayer(tiny,tiny->singleLayer().id,refused,levels);} catch(const std::length_error&) {denied=true;}
            require(denied && refused.usedBytes()==0 && limited->snapshot().pendingCpu==0 && limited->snapshot().committedCpu==0,"Refusal leaked admitted bytes");
            std::stop_source canceled;canceled.request_stop();bool stopped=false;
            try {engine::adjustLayer(tiny,tiny->singleLayer().id,*resources->tiles,levels,canceled.get_token());} catch(const std::runtime_error&) {stopped=true;}
            require(stopped && sample(tiny,0,0)==engine::pack({.2f,.3f,.4f,1}) && history.current()==final,"Cancellation/refusal altered snapshots");
            emitRecord({{"stage","verified_outputs"},{"project_bytes",static_cast<double>(QFileInfo(project).size())},
                {"png_bytes",static_cast<double>(QFileInfo(directory.filePath("gradient.png")).size())},
                {"jpeg_bytes",static_cast<double>(QFileInfo(directory.filePath("gradient.jpg")).size())},
                {"sampled_canonical_roundtrip",true},{"undo_redo",true},{"refusal_preserved",true},{"pre_cancellation_preserved",true}});
        }
        resources->spillCoordinator->waitIdle();disk->collectGarbage();observe("payloads_released",*resources,started);
        require(resources->tiles->usedBytes()==0 && disk->stats().entries==0,"Observation payloads remained retained");
        directory.setAutoRemove(false);emitRecord({{"status","passed"},{"full_lp8_qualification",false}});
    } catch(const std::exception& error) {
        emitRecord({{"status","failed_or_resource_constrained"},{"error",QString::fromUtf8(error.what())},{"full_lp8_qualification",false}});return 1;
    }
}
