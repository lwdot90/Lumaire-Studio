#include "core/resources.h"
#include "core/spill_coordinator.h"
#include "io/project_store.h"
#include "io/spill_store.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>

using namespace compositor;
namespace {
void expect(bool value,const char* message) {if(!value) throw std::runtime_error(message);}
QByteArray fileBytes(const QString& path) {
    QFile file(path); expect(file.open(QIODevice::ReadOnly),"Read owned project fixture"); return file.readAll();
}
void observe(const char* stage,const RuntimeResources& resources,std::int64_t revision=0) {
    const auto sample=MemorySample::read(); const auto memory=resources.memory->snapshot();
    const auto disk=resources.spill->stats();
    expect(sample.available && sample.resident,"Actual process/system memory samples required");
    std::cout<<"{\"stage\":\""<<stage<<"\",\"revision\":"<<revision
        <<",\"rss\":"<<*sample.resident<<",\"available\":"<<*sample.available
        <<",\"cpu\":"<<memory.committedCpu<<",\"gpu\":"<<memory.committedGpu
        <<",\"pending_cpu\":"<<memory.pendingCpu<<",\"pending_gpu\":"<<memory.pendingGpu
        <<",\"spill_bytes\":"<<disk.bytes<<",\"spill_entries\":"<<disk.entries
        <<",\"spill_active_io\":"<<disk.activeIo<<",\"spill_cleanup_failures\":"<<disk.cleanupFailures<<"}\n";
}
engine::Pixel oracle(int tile,std::size_t index,bool changed=false) {
    if(tile==4) return {.125f,.25f,.375f,.5f};
    return {static_cast<float>((index+static_cast<std::size_t>(tile)*7)%29)/32,
        static_cast<float>((index/29)%17)/32,changed ? .75f : .25f,1};
}
void exactPixels(const engine::DocumentPtr& document,bool changed=false) {
    const auto& raster=*document->singleLayer().raster;
    expect(raster.extent==engine::Extent{0,0,1280,256} && raster.tiles.size()==5,"Complete generated tile extent and count preserved");
    for(int x=0;x<5;++x) {
        const auto tile=raster.tiles.at({x,0});
        expect(tile->width()==256 && tile->height()==256 && tile->uniform()==(x==4),"Generated tile dimensions and representation preserved");
    }
    for(const auto& [coord,tile]:document->singleLayer().raster->tiles) {
        const auto lease=tile->read();
        for(int y=0;y<tile->height();++y) for(int x=0;x<tile->width();++x) {
            const auto index=static_cast<std::size_t>(y)*static_cast<std::size_t>(tile->width())+static_cast<std::size_t>(x);
            expect(lease.pixel(x,y)==engine::pack(oracle(static_cast<int>(coord.x),index,changed && coord.x==0)),"Canonical pixel differs from independent generated oracle");
        }
    }
}
}
int main(int argc,char** argv) {
    QCoreApplication application(argc,argv);
    try {
        expect(argc==2,"Inject one absolute disk-backed fixture directory");
        QTemporaryDir directory(QString::fromLocal8Bit(argv[1])+"/project-observation-XXXXXX");
        expect(directory.isValid(),"Create injected owned fixture directory");
        io::SpillLimits storageLimits; storageLimits.maxBytes=16*1024*1024; storageLimits.maxPayloadBytes=512*1024;
        storageLimits.maxEntries=32; storageLimits.minFreeBytes=64*1024*1024;
        auto storage=std::make_shared<io::SpillStore>(directory.path().toStdString(),storageLimits);
        const auto profile=ResourceLimits::detect(); expect(profile.systemHeadroom>=512*1024*1024,"Preserve LP8 system headroom");
        auto resources=std::make_shared<RuntimeResources>(profile,MemorySample::read,storage);
        const auto baseline=resources->memory->snapshot(); observe("baseline",*resources);
        const auto path=directory.filePath("observation.cproj");
        {
            engine::DocumentHistory history(engine::blankDocument(1280,256));
            {
                auto scratch=resources->memory->require(256*256*sizeof(engine::Pixel));
                std::vector<engine::Pixel> pixels(256*256); scratch.commit();
                auto edit=history.begin();
                for(int tile=0;tile<5;++tile) {
                    if(tile==4) edit.replace({tile,0},resources->tiles->constant(256,256,oracle(tile,0)));
                    else {
                        for(std::size_t i=0;i<pixels.size();++i) pixels[i]=oracle(tile,i);
                        edit.write(*resources->tiles,{tile,0},pixels);
                    }
                }
                expect(history.commit(edit,"Generated pixels"),"Publish source revision");
            }
            auto source=history.current(); auto sourceTile=source->singleLayer().raster->tiles.at({0,0});
            auto retainedLease=sourceTile->read(); const auto retainedPixel=retainedLease.pixel(17,23);
            expect(retainedPixel==engine::pack(oracle(0,23*256+17)),"Initial retained lease exact");
            observe("source_created",*resources,source->revision);
            for(const auto& [coord,tile]:source->singleLayer().raster->tiles) tile->spill();
            expect(!sourceTile->spillStatus().resident,"Spill drops cache despite live read lease");
            expect(retainedLease.pixel(17,23)==retainedPixel,"Independent lease survives source spill");
            observe("source_spilled_with_lease",*resources,source->revision);
            {
                auto scratch=resources->memory->require(256*256*sizeof(engine::Pixel));
                std::vector<engine::Pixel> pixels(256*256); scratch.commit();
                for(std::size_t i=0;i<pixels.size();++i) pixels[i]=oracle(0,i,true);
                auto edit=history.begin(); edit.write(*resources->tiles,{0,0},pixels);
                expect(history.commit(edit,"Retained history version"),"Publish distinct history revision");
            }
            expect(history.current()!=source && source->revision==1 && history.current()->revision==2,"Immutable source revision retained");
            expect(retainedLease.pixel(17,23)==retainedPixel,"History mutation preserves lease bytes");
            observe("history_revision_created",*resources,history.current()->revision);
            io::SaveOptions options; options.memory=resources->memory;
            const auto identity=io::saveProject(path,source,options); observe("source_saved",*resources,source->revision);
            options.expected=identity; options.checkpoint=[](io::SaveStage stage) {
                if(stage==io::SaveStage::Tile) throw std::runtime_error("Injected bounded per-tile save failure");
            };
            expect(identity.size>0 && identity.size<=1024*1024,"Bound rollback byte comparison to one MiB project");
            auto fileScratch=resources->memory->require(static_cast<std::uint64_t>(identity.size)*2);
            const auto before=fileBytes(path); bool rejected=false;
            try {io::saveProject(path,history.current(),options);} catch(const std::exception&) {rejected=true;}
            expect(rejected && io::fileIdentity(path)==identity && fileBytes(path)==before,"Injected save failure preserves exact file identity and bytes");
            options.checkpoint={};
            auto loaded=io::loadProject(path,*resources->tiles).document;
            expect(loaded->id==source->id && loaded->revision==source->revision && loaded->singleLayer().raster->id==source->singleLayer().raster->id,"Project identity and revision preserved");
            for(const auto& [coord,tile]:loaded->singleLayer().raster->tiles) {
                if(tile->spillable()) expect(tile->spillStatus().spilled && !tile->spillStatus().resident,"Reopen keeps each verified dense tile disk-only");
                else expect(tile->uniform() && coord==engine::TileCoord{4,0},"Only inline constant omits disk residency");
            }
            observe("reopened_disk_only",*resources,loaded->revision);
            exactPixels(loaded); exactPixels(source); exactPixels(history.current(),true);
            expect(retainedLease.pixel(17,23)==retainedPixel,"Retained lease remains immutable through save/reopen");
            for(const auto& [coord,tile]:loaded->singleLayer().raster->tiles) tile->spill();
            for(const auto& [coord,tile]:source->singleLayer().raster->tiles) tile->spill();
            for(const auto& [coord,tile]:history.current()->singleLayer().raster->tiles) tile->spill();
            expect(history.undo() && history.current()==source && history.redo(),"History revision roundtrip preserved");
            retainedLease={}; sourceTile.reset(); loaded.reset(); source.reset();
            observe("lease_released_history_retained",*resources,history.current()->revision);
        }
        resources->spillCoordinator->waitIdle(); storage->collectGarbage();
        const auto final=resources->memory->snapshot(); const auto disk=storage->stats();
        expect(resources->tiles->usedBytes()==0 && disk.entries==0 && disk.bytes==0 && disk.activeIo==0,"All source/history/project payloads retired");
        expect(final.pendingCpu==0 && final.pendingGpu==0 && final.committedCpu==baseline.committedCpu && final.committedGpu==baseline.committedGpu,"Only bounded runtime/store metadata remains admitted");
        observe("all_payloads_released",*resources);
        std::cout<<"{\"exact_pixels\":true,\"immutable_history\":true,\"save_rollback\":true,\"canonical_fixture_bytes\":5767168,\"full_lp8_qualification\":false}\n";
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
