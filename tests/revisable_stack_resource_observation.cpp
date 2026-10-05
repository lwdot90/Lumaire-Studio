#include "core/editor_commands.h"
#include "core/resources.h"
#include "core/spill_coordinator.h"
#include "io/project_store.h"
#include "io/image_export.h"
#include "io/spill_store.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using namespace compositor;
namespace {
using Clock=std::chrono::steady_clock;
constexpr std::uint64_t mib=1024*1024,gib=1024*mib;
void require(bool condition,const char* message) {if(!condition) throw std::runtime_error(message);}
void record(const QJsonObject& object) {std::cout<<QJsonDocument(object).toJson(QJsonDocument::Compact).constData()<<'\n'<<std::flush;}
void raise(std::atomic<std::uint64_t>& value,std::uint64_t candidate) {
    auto old=value.load();while(old<candidate && !value.compare_exchange_weak(old,candidate)) {}
}
class Sampler {
public:
    explicit Sampler(std::shared_ptr<RuntimeResources> resources):resources_(std::move(resources)) {
        take();
        worker_=std::jthread([this](std::stop_token stop) {
            while(!stop.stop_requested()) {take();std::this_thread::sleep_for(std::chrono::milliseconds(50));}
        });
    }
    ~Sampler() {stop();}
    void stop() {if(worker_.joinable()) {worker_.request_stop();worker_.join();}take();}
    void take() noexcept {
        try {
            const auto memory=MemorySample::read();
            if(!memory.resident || !memory.available) {failed=true;return;}
            const auto admitted=resources_->memory->snapshot();const auto disk=resources_->spill->stats();
            raise(maxRss,*memory.resident);raise(maxAdmitted,admitted.pendingCpu+admitted.pendingGpu+admitted.committedCpu+admitted.committedGpu);
            raise(maxSpill,disk.bytes);raise(maxEntries,disk.entries);++samples;
        } catch(...) {failed=true;}
    }
    std::atomic<std::uint64_t> maxRss=0,maxAdmitted=0,maxSpill=0,maxEntries=0,samples=0;
    std::atomic<bool> failed=false;
private:
    std::shared_ptr<RuntimeResources> resources_;
    std::jthread worker_;
};
void observe(const char* stage,const RuntimeResources& resources,Clock::time_point started,Sampler& sampler) {
    sampler.take();const auto memory=MemorySample::read();const auto admitted=resources.memory->snapshot();const auto disk=resources.spill->stats();
    require(memory.resident && memory.available && !sampler.failed,"Actual process/system memory samples required");
    record({{"stage",stage},{"elapsed_ms",static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-started).count())},
        {"rss",static_cast<double>(*memory.resident)},{"mem_available",static_cast<double>(*memory.available)},
        {"admitted_cpu",static_cast<double>(admitted.committedCpu)},{"pending_cpu",static_cast<double>(admitted.pendingCpu)},
        {"tile_store_used",static_cast<double>(resources.tiles->usedBytes())},{"spill_bytes",static_cast<double>(disk.bytes)},
        {"spill_entries",static_cast<double>(disk.entries)},{"spill_cleanup_failures",static_cast<double>(disk.cleanupFailures)}});
}
engine::Pixel gradient(int x,int y) {
    return engine::fromStraightSrgb({.05f+.8f*static_cast<float>(x)/3999.f,.05f+.8f*static_cast<float>(y)/2999.f,
        .1f+.6f*static_cast<float>(x+y)/6998.f,1});
}
void exactRaster(const engine::RasterSnapshot& a,const engine::RasterSnapshot& b) {
    require(a.extent==b.extent && a.defaultValue==b.defaultValue && a.tiles.size()==b.tiles.size(),"Canonical raster metadata differs");
    for(const auto& [coordinate,tile]:a.tiles) {
        const auto found=b.tiles.find(coordinate);require(found!=b.tiles.end(),"Canonical tile missing");
        auto left=tile->read();auto right=found->second->read();
        require(left.width()==right.width() && left.height()==right.height(),"Canonical tile shape differs");
        for(int y=0;y<left.height();++y) for(int x=0;x<left.width();++x)
            require(left.pixel(x,y)==right.pixel(x,y),"Canonical reopened pixel differs");
    }
}
QByteArray fileDigest(const QString& path) {
    QFile input(path);require(input.open(QIODevice::ReadOnly),"Open project for streaming digest");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    require(hash.addData(&input),"Hash project bytes");return hash.result();
}
}
int main(int argc,char** argv) {
    QCoreApplication application(argc,argv);const auto started=Clock::now();
    try {
        require(argc==2,"Supply one existing absolute disk-backed build directory");
        const auto base=QString::fromLocal8Bit(argv[1]);
        require(QDir::isAbsolutePath(base) && QFileInfo(base).isDir() && !QFileInfo(base).isSymLink(),"Output directory must be absolute, existing and nonsymlink");
        QTemporaryDir directory(QDir(base).filePath("revisable-stack-observation-XXXXXX"));require(directory.isValid(),"Create private observation directory");
        io::SpillLimits spillLimits;spillLimits.maxBytes=gib;spillLimits.maxPayloadBytes=2*mib;spillLimits.maxEntries=8192;spillLimits.maxIoOperations=1;spillLimits.minFreeBytes=256*mib;
        auto disk=std::make_shared<io::SpillStore>(directory.path().toStdString(),spillLimits);
        const auto limits=ResourceLimits::detect();require(limits.lowMemory,"This observation requires the detected low-memory machine profile");
        auto resources=std::make_shared<RuntimeResources>(limits,MemorySample::read,disk);Sampler sampler(resources);
        record({{"kind","bounded_revisable_stack_development_observation"},{"full_lp8_qualification",false},{"width",4000},{"height",3000},
            {"output_directory",directory.path()},{"application_envelope",static_cast<double>(limits.applicationMemory)},
            {"system_headroom",static_cast<double>(limits.systemHeadroom)},{"compute_workers",static_cast<int>(limits.computeWorkers)},{"sampling_interval_ms",50}});
        observe("baseline",*resources,started,sampler);
        {
            engine::DocumentHistory history(engine::blankDocument(4000,3000));
            {
                auto permit=resources->compute.acquire(WorkScheduler::Priority::Processing,true);require(permit.has_value(),"Admit tiled source generation");
                auto edit=history.begin();
                for(int ty=0;ty<3000;ty+=256) for(int tx=0;tx<4000;tx+=256) {
                    const int width=std::min(256,4000-tx),height=std::min(256,3000-ty);
                    auto charge=resources->memory->require(static_cast<std::uint64_t>(width)*height*sizeof(engine::Pixel));
                    std::vector<engine::Pixel> pixels(static_cast<std::size_t>(width)*height);charge.commit();
                    for(int y=0;y<height;++y) for(int x=0;x<width;++x) pixels[static_cast<std::size_t>(y)*width+x]=gradient(tx+x,ty+y);
                    edit.write(*resources->tiles,{tx/256,ty/256},pixels);
                }
                require(history.commit(edit,"Generated source"),"Publish tiled source");
            }
            const auto source=history.current();const auto sourceRaster=source->singleLayer().raster;const auto id=source->singleLayer().id;
            observe("source_generated",*resources,started,sampler);
            engine::AdjustmentParameters exposure{engine::AdjustmentKind::Exposure,.5};
            engine::AdjustmentParameters levels;levels.kind=engine::AdjustmentKind::Levels;levels.levels.gamma=1.3;
            engine::AdjustmentParameters curves;curves.kind=engine::AdjustmentKind::Curves;
            curves.channelCurves[0]={{0,0},{.25,.35},{.75,.8},{1,1}};
            curves.channelCurves[2]={{0,0},{.3,.22},{.8,.86},{1,1}};
            engine::AdjustmentParameters balance;balance.kind=engine::AdjustmentKind::ColorBalance;balance.colorBalance={.15,-.1};
            std::vector operations{exposure,levels,curves,balance};
            const auto revise=[&](engine::DocumentHistory& owner,const char* label) {
                auto permit=resources->compute.acquire(WorkScheduler::Priority::Processing,true);require(permit.has_value(),"Admit source-retained stack evaluation");
                require(owner.commit(engine::setRevisableAdjustments(owner.current(),id,*resources->tiles,operations),label),"Revision produced no metadata change");
            };
            revise(history,"Initial four-operation channel stack");observe("initial_stack",*resources,started,sampler);
            operations.front().value=.25;revise(history,"Revise exposure strength");
            const auto saved=history.current();require(saved->singleLayer().adjustments->source==sourceRaster,"Strength revision replaced retained source");
            require(history.undo() && history.redo() && history.current()==saved,"Undo/redo lost exact revision");
            observe("strength_revised",*resources,started,sampler);
            const auto project=directory.filePath("revisable.cproj");
            {
                auto permit=resources->compute.acquire(WorkScheduler::Priority::Processing,true);require(permit.has_value(),"Admit native save");
                io::SaveOptions options;options.memory=resources->memory;io::saveProject(project,saved,options);
            }
            observe("schema_saved",*resources,started,sampler);
            engine::DocumentPtr reopened;
            {
                auto permit=resources->compute.acquire(WorkScheduler::Priority::Processing,true);require(permit.has_value(),"Admit native reopen and derived rebuild");
                reopened=io::loadProject(project,*resources->tiles).document;
                require(reopened->singleLayer().adjustments && reopened->singleLayer().adjustments->operations==operations,"Reopen lost ordered exact controls");
                require(reopened->id==saved->id && reopened->revision==saved->revision,"Reopen changed saved document identity");
                exactRaster(*sourceRaster,*reopened->singleLayer().adjustments->source);
                exactRaster(*saved->singleLayer().raster,*reopened->singleLayer().raster);
            }
            observe("reopened_all_source_cache_pixels_verified",*resources,started,sampler);
            engine::DocumentHistory reopenedHistory(reopened);const auto retained=reopened->singleLayer().adjustments->source;
            operations.front().value=.75;revise(reopenedHistory,"Revise after reopen");
            const auto final=reopenedHistory.current();require(final->singleLayer().adjustments->source==retained,"Post-reopen revision lost retained source");
            observe("post_reopen_revision",*resources,started,sampler);
            const auto revisedProject=directory.filePath("revised.cproj");
            {
                auto permit=resources->compute.acquire(WorkScheduler::Priority::Processing,true);require(permit.has_value(),"Admit revised native save");
                io::SaveOptions options;options.memory=resources->memory;io::saveProject(revisedProject,final,options);
            }
            for(const auto* suffix:{"png","jpg"}) {
                auto permit=resources->compute.acquire(WorkScheduler::Priority::Processing,true);require(permit.has_value(),"Admit streamed export");
                io::ExportOptions options;options.memory=resources->memory;
                io::exportImage(directory.filePath(QString("revised.")+suffix),final,options);observe(suffix,*resources,started,sampler);
            }
            const auto identity=io::fileIdentity(project);const auto digest=fileDigest(project);
            std::stop_source canceled;canceled.request_stop();bool saveCanceled=false,editCanceled=false;
            io::SaveOptions canceledSave;canceledSave.memory=resources->memory;canceledSave.expected=identity;canceledSave.stop=canceled.get_token();
            try {io::saveProject(project,final,canceledSave);} catch(const std::exception&) {saveCanceled=true;}
            try {engine::setRevisableAdjustments(final,id,*resources->tiles,{},canceled.get_token());} catch(const std::exception&) {editCanceled=true;}
            require(saveCanceled && editCanceled && io::fileIdentity(project)==identity && fileDigest(project)==digest && reopenedHistory.current()==final,
                "Pre-canceled write/edit changed existing project or history");
            exactRaster(*sourceRaster,*retained);
            record({{"stage","verified_outputs"},{"all_source_and_cache_pixels_roundtrip",true},{"source_retained_after_revision",true},
                {"undo_redo",true},{"pre_canceled_write_preserved",true},{"project_bytes",static_cast<double>(QFileInfo(project).size())},
                {"revised_project_bytes",static_cast<double>(QFileInfo(revisedProject).size())},
                {"png_bytes",static_cast<double>(QFileInfo(directory.filePath("revised.png")).size())},
                {"jpeg_bytes",static_cast<double>(QFileInfo(directory.filePath("revised.jpg")).size())}});
        }
        resources->spillCoordinator->waitIdle();disk->collectGarbage();observe("payloads_released",*resources,started,sampler);
        require(resources->tiles->usedBytes()==0 && disk->stats().entries==0,"Payloads remain retained after observation");
        sampler.stop();require(!sampler.failed,"Sampling failed");directory.setAutoRemove(false);
        record({{"status","passed"},{"full_lp8_qualification",false},{"duration_ms",static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-started).count())},
            {"maximum_sampled_rss",static_cast<double>(sampler.maxRss.load())},{"maximum_sampled_admitted",static_cast<double>(sampler.maxAdmitted.load())},
            {"maximum_sampled_spill_bytes",static_cast<double>(sampler.maxSpill.load())},{"maximum_sampled_spill_entries",static_cast<double>(sampler.maxEntries.load())},
            {"samples",static_cast<double>(sampler.samples.load())},{"sampling_interval_ms",50}});
    } catch(const std::exception& error) {
        record({{"status","failed_or_resource_constrained"},{"error",QString::fromUtf8(error.what())},{"full_lp8_qualification",false}});return 1;
    }
}
