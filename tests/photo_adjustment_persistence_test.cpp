#include "core/editor_commands.h"
#include "io/image_export.h"
#include <QCoreApplication>
#include <QColor>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QTemporaryDir>
#include <lcms2.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace compositor;
using namespace compositor::engine;
namespace {
void expect(bool value,const char* message) {if(!value) throw std::runtime_error(message);}
template<class F> void rejects(F action) {
    bool rejected=false;try {action();}catch(const std::exception&) {rejected=true;}
    expect(rejected,"Invalid, canceled or denied adjustment unexpectedly succeeded");
}
std::shared_ptr<MemoryAdmission> admission(std::uint64_t limit=32*1024*1024) {
    return std::make_shared<MemoryAdmission>(limit,0,[]{return MemorySample{1024*1024*1024,0};});
}
std::shared_ptr<const SourceProfile> profile() {
    const auto native=cmsCreate_sRGBProfile();expect(native!=nullptr,"Cannot create profile fixture");
    const auto owner=std::unique_ptr<void,decltype(&cmsCloseProfile)>(native,cmsCloseProfile);
    cmsUInt32Number size=0;
    expect(cmsSaveProfileToMem(native,nullptr,&size)!=0 && size>0 && size<4096,"Invalid profile fixture size");
    std::vector<std::uint8_t> bytes(size);
    expect(cmsSaveProfileToMem(native,bytes.data(),&size)!=0,"Cannot serialize profile fixture");
    return std::make_shared<const SourceProfile>(SourceProfile{Id::generate(),std::move(bytes)});
}
DocumentPtr fixture(TileStore& tiles) {
    const std::array<Pixel,4> values{fromStraightSrgb({.25f,.25f,.25f,.5f}),
        fromStraightSrgb({.5f,.5f,.5f,.75f}),fromStraightSrgb({.75f,.75f,.75f,1}),Pixel{}};
    const auto sourceProfile=profile();
    TileMap map;map.emplace(TileCoord{0,0},tiles.create(4,1,values));
    auto raster=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,4,1},PackedPixel{},
        std::move(map),0,sourceProfile);
    LayerNode photo{Id::generate()};photo.raster=raster;photo.name="Photo";
    photo.localToDocument.tx=1;photo.sampling=Sampling::Nearest;
    LayerNode other{Id::generate()};other.siblingOrder=1;other.visible=false;
    other.opacity=.375f;other.name="Unmodified hidden layer";other.localToDocument.ty=1;
    other.raster=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,1,1},
        pack(Pixel{.0625f,.125f,.25f,.5f}),TileMap{},0,sourceProfile);
    return std::make_shared<const DocumentSnapshot>(Id::generate(),6,2,150,
        std::vector<LayerNode>{photo,other},0,Selection{Extent{1,0,4,1}});
}
// Independent transfer-function and adjustment oracles use the quantized input.
// The native output is then compared byte-for-byte after save/reopen.
double encoded(double linear) {
    return linear<=.0031308 ? 12.92*linear : 1.055*std::pow(linear,1/2.4)-.055;
}
double linear(double code) {
    return code<=.04045 ? code/12.92 : std::pow((code+.055)/1.055,2.4);
}
Pixel oracle(Pixel input,AdjustmentKind kind) {
    if(input.a==0) return {};
    if(kind==AdjustmentKind::ColorBalance) return {input.r*2,input.g*.5f,input.b*.5f,input.a};
    auto channel=[&](float value) {
        const double code=std::clamp(encoded(static_cast<double>(value)/input.a),0.,1.);
        const double output=kind==AdjustmentKind::Levels ? std::sqrt(code) :
            (code<=.5 ? code*1.5 : .75+(code-.5)*.5);
        return static_cast<float>(linear(output)*input.a);
    };
    return {channel(input.r),channel(input.g),channel(input.b),input.a};
}
void nearPixel(Pixel actual,Pixel expected) {
    expect(actual.a==expected.a,"Adjustment changed canonical alpha");
    // Half-float output rounding and the engine's float transfer functions.
    expect(std::abs(actual.r-expected.r)<.001f && std::abs(actual.g-expected.g)<.001f &&
        std::abs(actual.b-expected.b)<.001f,"Adjustment differs from independent analytic oracle");
}
void unchangedContext(const DocumentPtr& before,const DocumentPtr& after,const Id& target) {
    expect(before->id==after->id && before->width==after->width && before->height==after->height &&
        before->resolution==after->resolution && before->selection==after->selection,
        "Adjustment changed document/selection context");
    for(const auto& original:before->layers()) {
        const auto& edited=after->layer(original.id);
        if(original.id!=target) {expect(edited==original,"Adjustment modified another layer");continue;}
        auto metadata=edited;metadata.raster=original.raster;
        expect(metadata==original,"Adjustment changed target placement or metadata");
        expect(edited.raster->sourceProfile==original.raster->sourceProfile,
            "Adjustment did not retain source profile backing");
    }
}
DocumentPtr roundTrip(const QString& path,const DocumentPtr& document,TileStore& tiles,
                      const std::shared_ptr<MemoryAdmission>& memory) {
    io::SaveOptions options;options.memory=memory;
    const auto identity=io::saveProject(path,document,options);
    expect(QFileInfo(path).size()>0 && QFileInfo(path).size()<512*1024,"Tiny native output exceeds fixture bound");
    const auto loaded=io::loadProject(path,tiles);
    expect(loaded.identity==identity && loaded.document->id==document->id &&
        loaded.document->revision==document->revision && loaded.document->width==document->width &&
        loaded.document->height==document->height && loaded.document->resolution==document->resolution &&
        loaded.document->selection==document->selection,"Native document context changed on reopen");
    expect(loaded.document->layers().size()==document->layers().size(),"Native layer count changed");
    for(const auto& original:document->layers()) {
        const auto& restored=loaded.document->layer(original.id);
        auto metadata=restored;metadata.raster=original.raster;
        expect(metadata==original,"Native layer metadata/placement changed");
        expect(restored.raster->id==original.raster->id && restored.raster->revision==original.raster->revision &&
            restored.raster->extent==original.raster->extent && restored.raster->defaultValue==original.raster->defaultValue &&
            restored.raster->tiles.size()==original.raster->tiles.size(),"Native raster metadata changed");
        expect(restored.raster->sourceProfile && original.raster->sourceProfile &&
            restored.raster->sourceProfile->id==original.raster->sourceProfile->id &&
            restored.raster->sourceProfile->bytes==original.raster->sourceProfile->bytes,
            "Native source profile identity/bytes changed");
        for(const auto& [coordinate,tile]:original.raster->tiles)
            expect(restored.raster->tiles.at(coordinate)->canonicalBytes()==tile->canonicalBytes(),
                "Native adjusted pixels are not an exact canonical round-trip");
    }
    return loaded.document;
}
int byte(double value) {return static_cast<int>(std::lround(std::clamp(value,0.,1.)*255));}
QByteArray fileBytes(const QString& path) {
    QFile file(path);expect(file.open(QIODevice::ReadOnly),"Cannot read export fixture");return file.readAll();
}
void exports(const QString& stem,const DocumentPtr& document,const Id& target,
             const std::shared_ptr<MemoryAdmission>& memory) {
    const auto baseline=memory->snapshot();
    io::ExportOptions options;options.memory=memory;options.mipBytes=1024*1024;options.quality=100;
    const auto png=stem+".png",jpeg=stem+".jpeg";
    io::exportImage(png,document,options);io::exportImage(jpeg,document,options);
    const auto pngImage=QImageReader(png).read(),jpegImage=QImageReader(jpeg).read();
    expect(!pngImage.isNull() && !jpegImage.isNull() && pngImage.size()==QSize(6,2) &&
        jpegImage.size()==QSize(6,2),"Adjusted exports failed decode/dimensions");
    expect(QFileInfo(png).size()<64*1024 && QFileInfo(jpeg).size()<64*1024,"Tiny exports exceed fixture bounds");
    const auto& raster=*document->layer(target).raster;
    for(int x=0;x<4;++x) {
        const auto pixel=raster.pixel(x,0);
        const auto color=pngImage.pixelColor(x+1,0),white=jpegImage.pixelColor(x+1,0);
        expect(std::abs(color.alpha()-byte(pixel.a))<=1,"PNG export changed adjusted alpha");
        const std::array<float,3> channels{pixel.r,pixel.g,pixel.b};
        const std::array<int,3> rgb{color.red(),color.green(),color.blue()};
        const std::array<int,3> flattened{white.red(),white.green(),white.blue()};
        for(std::size_t c=0;c<channels.size();++c) {
            if(pixel.a>0) expect(std::abs(rgb[c]-byte(encoded(channels[c]/pixel.a)))<=2,
                "PNG export changed adjusted straight color");
            expect(std::abs(flattened[c]-byte(encoded(channels[c]+1-pixel.a)))<=8,
                "JPEG export changed adjusted color/white flattening");
        }
        expect(white.alpha()==255,"JPEG export did not flatten alpha");
    }
    expect(pngImage.pixelColor(0,0).alpha()==0 && pngImage.pixelColor(2,1).alpha()==0,
        "Translated photo export changed canvas exterior transparency");
    const auto before=fileBytes(png);options.expected=io::fileIdentity(png);
    options.checkpoint=[](io::ExportStage stage) {
        if(stage==io::ExportStage::Replace) throw std::runtime_error("Injected adjusted export replacement failure");
    };
    rejects([&]{io::exportImage(png,document,options);});
    expect(fileBytes(png)==before,"Failed adjusted export replaced existing output");
    const auto after=memory->snapshot();
    expect(after.committedCpu==baseline.committedCpu && after.pendingCpu==baseline.pendingCpu,
        "Adjusted export/failure retained scratch charges");
}
AdjustmentParameters parameters(AdjustmentKind kind,bool neutral=false) {
    AdjustmentParameters p;p.kind=kind;
    if(!neutral) {
        if(kind==AdjustmentKind::Levels) p.levels.gamma=2;
        if(kind==AdjustmentKind::Curves) p.curve={{0,0},{.5,.75},{1,1}};
        if(kind==AdjustmentKind::ColorBalance) p.colorBalance={1,1};
    }
    return p;
}
void adjustment(const QTemporaryDir& directory,AdjustmentKind kind,const QString& name) {
    const auto memory=admission();TileStore tiles(4*1024*1024,memory);
    const auto original=fixture(tiles);const auto target=original->layers().front().id;
    const auto originalBytes=original->layer(target).raster->tiles.at(TileCoord{0,0})->canonicalBytes();
    DocumentHistory history(original);
    expect(history.commit(adjustLayer(original,target,tiles,parameters(kind)),"Photo adjustment"),
        "Photo adjustment unexpectedly produced no change");
    const auto edited=history.current();unchangedContext(original,edited,target);
    for(int x=0;x<4;++x)
        nearPixel(edited->layer(target).raster->pixel(x,0),oracle(original->layer(target).raster->pixel(x,0),kind));
    expect(history.undo() && history.current()==original && !history.canUndo(),"Adjustment is not exactly one undo step");
    expect(!history.commit(adjustLayer(history.current(),target,tiles,parameters(kind,true)),"Neutral") &&
        history.current()==original && history.canRedo(),"Neutral adjustment changed history or discarded redo");
    expect(history.redo() && history.current()==edited && !history.canRedo(),"Adjustment redo lost immutable edited snapshot");
    const auto reopened=roundTrip(directory.filePath(name+".cproj"),edited,tiles,memory);
    exports(directory.filePath(name),reopened,target,memory);
    // Parameters are destructive commands, not saved editable controls. The
    // reopened canonical raster still supports another independently undoable edit.
    DocumentHistory reopenedHistory(reopened);
    expect(reopenedHistory.commit(adjustLayer(reopened,target,tiles,{AdjustmentKind::Exposure,1}),"Reopened exposure"),
        "Reopened adjusted source cannot be edited");
    const auto second=reopenedHistory.current();
    for(int x=0;x<4;++x) {
        const auto first=reopened->layer(target).raster->pixel(x,0);
        nearPixel(second->layer(target).raster->pixel(x,0),Pixel{first.r*2,first.g*2,first.b*2,first.a});
    }
    expect(reopenedHistory.undo() && reopenedHistory.current()==reopened && reopenedHistory.redo() &&
        reopenedHistory.current()==second,"Reopened source edit lost undo/redo");
    roundTrip(directory.filePath(name+"-edited-again.cproj"),second,tiles,memory);
    expect(original->layer(target).raster->tiles.at(TileCoord{0,0})->canonicalBytes()==originalBytes,
        "Adjustment/save/export mutated original canonical backing");
    const auto beforeFailure=history.current();const auto ledger=memory->snapshot();
    std::stop_source cancellation;cancellation.request_stop();
    rejects([&]{history.commit(adjustLayer(beforeFailure,target,tiles,parameters(kind),cancellation.get_token()),"Canceled");});
    auto invalid=parameters(kind);
    if(kind==AdjustmentKind::Levels) invalid.levels.gamma=0;
    if(kind==AdjustmentKind::Curves) invalid.curve={{0,0},{0,.5},{1,1}};
    if(kind==AdjustmentKind::ColorBalance) invalid.colorBalance.warmth=std::numeric_limits<double>::infinity();
    rejects([&]{history.commit(adjustLayer(beforeFailure,target,tiles,invalid),"Invalid");});
    const auto deniedMemory=admission(1);TileStore denied(1,deniedMemory);
    rejects([&]{history.commit(adjustLayer(beforeFailure,target,denied,parameters(kind)),"Denied");});
    expect(history.current()==beforeFailure && history.canUndo() && !history.canRedo(),
        "Failed adjustment changed document/history publication");
    const auto afterFailure=memory->snapshot(),deniedLedger=deniedMemory->snapshot();
    expect(afterFailure.committedCpu==ledger.committedCpu && afterFailure.pendingCpu==ledger.pendingCpu &&
        deniedLedger.committedCpu==0 && deniedLedger.pendingCpu==0,"Failed adjustment retained admission charges");
}
void selection(const QTemporaryDir& directory) {
    const auto memory=admission();TileStore tiles(4*1024*1024,memory);
    auto original=fixture(tiles);const auto target=original->layers().front().id;
    original=selectDocument(original,Selection{Extent{2,0,1,1}}).finish(original->revision+1);
    auto p=parameters(AdjustmentKind::Curves);
    const auto edited=adjustLayer(original,target,tiles,p).finish(original->revision+1);
    unchangedContext(original,edited,target);
    for(int x=0;x<4;++x) {
        const auto input=original->layer(target).raster->pixel(x,0);
        nearPixel(edited->layer(target).raster->pixel(x,0),x==1 ? oracle(input,p.kind) : input);
        if(x!=1) expect(pack(edited->layer(target).raster->pixel(x,0))==pack(input),
            "Translated selection adjusted an unselected source pixel");
    }
    roundTrip(directory.filePath("selected-curves.cproj"),edited,tiles,memory);
}
}
int main(int argc,char** argv) {
    QCoreApplication application(argc,argv);
    try {
        QTemporaryDir directory(QCoreApplication::applicationDirPath()+"/photo-adjustment-persistence-XXXXXX");
        expect(directory.isValid(),"Cannot create owned build-directory photo adjustment fixtures");
        adjustment(directory,AdjustmentKind::Levels,"levels");
        adjustment(directory,AdjustmentKind::Curves,"curves");
        adjustment(directory,AdjustmentKind::ColorBalance,"color-balance");
        selection(directory);
        std::cout<<"Levels, curves and color balance: analytic pixels, exact native persistence, history, selection and decoded exports\n";
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
