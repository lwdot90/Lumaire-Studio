#include "core/editor_commands.h"
#include "io/project_store.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <lcms2.h>
#include <array>
#include <iostream>
#include <stdexcept>

using namespace compositor;
using namespace compositor::engine;
namespace {
void expect(bool condition,const char* message) {if(!condition) throw std::runtime_error(message);}
std::shared_ptr<const SourceProfile> profile() {
    const auto native=cmsCreate_sRGBProfile();
    expect(native!=nullptr,"Could not create small source profile fixture");
    const auto owner=std::unique_ptr<void,decltype(&cmsCloseProfile)>(native,cmsCloseProfile);
    cmsUInt32Number length=0;
    expect(cmsSaveProfileToMem(native,nullptr,&length)!=0 && length>0 && length<4096,
        "Could not size small source profile fixture");
    std::vector<std::uint8_t> bytes(length);
    expect(cmsSaveProfileToMem(native,bytes.data(),&length)!=0,"Could not serialize source profile fixture");
    return std::make_shared<const SourceProfile>(SourceProfile{Id::generate(),std::move(bytes)});
}
DocumentPtr fixture(TileStore& store,std::span<const Pixel> pixels,std::shared_ptr<const SourceProfile> sourceProfile) {
    TileMap map;map.emplace(TileCoord{0,0},store.create(static_cast<int>(pixels.size()),1,pixels));
    auto raster=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,static_cast<std::int64_t>(pixels.size()),1},
        PackedPixel{},std::move(map),0,std::move(sourceProfile));
    return std::make_shared<const DocumentSnapshot>(Id::generate(),Id::generate(),static_cast<int>(pixels.size()),1,300,raster,0,"Edited photo",true,.625f);
}
void exactSamples(const DocumentPtr& document,const Id& layer,std::span<const Pixel> expected) {
    for(std::size_t x=0;x<expected.size();++x)
        expect(pack(document->layer(layer).raster->pixel(static_cast<std::int64_t>(x),0))==pack(expected[x]),
            "Saved adjustment differs from independent expected canonical samples");
}
DocumentPtr roundTrip(const QString& path,const DocumentPtr& document,TileStore& tiles) {
    const auto identity=io::saveProject(path,document);
    auto loaded=io::loadProject(path,tiles);
    expect(loaded.identity==identity && loaded.document->id==document->id && loaded.document->revision==document->revision &&
        loaded.document->resolution==document->resolution && loaded.document->selection==document->selection,
        "Edited document identity/revision/resolution/selection failed native round-trip");
    expect(loaded.document->layers().size()==document->layers().size(),"Edited layer count failed native round-trip");
    for(const auto& original:document->layers()) {
        const auto& restored=loaded.document->layer(original.id);
        expect(restored.name==original.name && restored.opacity==original.opacity && restored.blend==original.blend &&
            restored.localToDocument==original.localToDocument && restored.parent==original.parent,
            "Edited layer metadata or placement failed native round-trip");
        if(!original.raster) continue;
        expect(restored.raster->id==original.raster->id && restored.raster->extent==original.raster->extent &&
            restored.raster->defaultValue==original.raster->defaultValue && restored.raster->revision==original.raster->revision,
            "Edited raster identity/default/revision failed native round-trip");
        expect(original.raster->sourceProfile && restored.raster->sourceProfile &&
            original.raster->sourceProfile->id==restored.raster->sourceProfile->id &&
            original.raster->sourceProfile->bytes==restored.raster->sourceProfile->bytes,
            "Source ICC bytes or identity changed during destructive adjustment save");
        for(const auto& [coordinate,tile]:original.raster->tiles)
            expect(restored.raster->tiles.at(coordinate)->canonicalBytes()==tile->canonicalBytes(),
                "Edited canonical tile bytes changed during save/reopen");
    }
    return loaded.document;
}
void savedAdjustment(const QString& path,TileStore& tiles,std::shared_ptr<const SourceProfile> sourceProfile,
                     std::span<const Pixel> input,std::span<const Pixel> expected,AdjustmentParameters parameters) {
    const auto document=fixture(tiles,input,std::move(sourceProfile));
    const auto target=document->singleLayer().id;
    DocumentHistory history(document);
    expect(history.commit(adjustLayer(document,target,tiles,parameters),"Adjustment"),"Adjustment command was unexpectedly a no-op");
    const auto edited=history.current();
    expect(history.undo() && history.current()==document && !history.canUndo(),"Adjustment was not exactly one undoable command");
    expect(history.redo() && history.current()==edited && !history.canRedo(),"Adjustment redo lost its immutable output");
    exactSamples(edited,target,expected);
    exactSamples(roundTrip(path,edited,tiles),target,expected);
    exactSamples(document,target,input);
}
void allFour(const QTemporaryDir& directory) {
    TileStore tiles(4*1024*1024);const auto sourceProfile=profile();
    const std::array<Pixel,3> exposureInput{Pixel{2,-.25f,.5f,.5f},Pixel{.125f,.0625f,.03125f,.25f},Pixel{}};
    const std::array<Pixel,3> exposureExpected{Pixel{4,-.5f,1,.5f},Pixel{.25f,.125f,.0625f,.25f},Pixel{}};
    savedAdjustment(directory.filePath("exposure.cproj"),tiles,sourceProfile,exposureInput,exposureExpected,{AdjustmentKind::Exposure,1});
    const std::array<Pixel,2> brightnessInput{fromStraightSrgb({.25f,.5f,.75f,.5f}),Pixel{}};
    const std::array<Pixel,2> brightnessExpected{fromStraightSrgb({.5f,.75f,1,.5f}),Pixel{}};
    savedAdjustment(directory.filePath("brightness.cproj"),tiles,sourceProfile,brightnessInput,brightnessExpected,{AdjustmentKind::Brightness,.25});
    const std::array<Pixel,2> contrastInput{fromStraightSrgb({.25f,.25f,.25f,.25f}),fromStraightSrgb({.75f,.75f,.75f,.5f})};
    const std::array<Pixel,2> contrastExpected{Pixel{0,0,0,.25f},Pixel{.5f,.5f,.5f,.5f}};
    savedAdjustment(directory.filePath("contrast.cproj"),tiles,sourceProfile,contrastInput,contrastExpected,{AdjustmentKind::Contrast,1});
    const std::array<Pixel,2> saturationInput{Pixel{.5f,0,0,.5f},Pixel{}};
    const std::array<Pixel,2> saturationExpected{fromStraightSrgb({.2126f,.2126f,.2126f,.5f}),Pixel{}};
    savedAdjustment(directory.filePath("saturation.cproj"),tiles,sourceProfile,saturationInput,saturationExpected,{AdjustmentKind::Saturation,0});
}
void selectedPlacement(const QTemporaryDir& directory) {
    TileStore tiles(4*1024*1024);
    const std::array<Pixel,2> input{Pixel{.125f,0,0,.5f},Pixel{0,.125f,0,.5f}};
    auto document=fixture(tiles,input,profile());
    auto layer=document->singleLayer();layer.localToDocument.tx=.5;
    document=std::make_shared<const DocumentSnapshot>(document->id,3,1,300,std::vector<LayerNode>{layer});
    document=selectDocument(document,Selection{Extent{1,0,1,1}}).finish(document->revision+1);
    DocumentHistory history(document);
    expect(history.commit(adjustLayer(document,layer.id,tiles,{AdjustmentKind::Exposure,1}),"Selected exposure"),
        "Selected adjustment was unexpectedly a no-op");
    const std::array<Pixel,2> expected{Pixel{.1875f,0,0,.5f},Pixel{0,.1875f,0,.5f}};
    const auto reopened=roundTrip(directory.filePath("selected-placement.cproj"),history.current(),tiles);
    exactSamples(reopened,layer.id,expected);
    expect(reopened->selection==std::optional<Selection>{Selection{Extent{1,0,1,1}}} &&
        reopened->layer(layer.id).localToDocument.tx==.5,"Native save lost transformed selection context");
    // A reopened file must remain editable with the original selection/placement.
    auto second=adjustLayer(reopened,layer.id,tiles,{AdjustmentKind::Exposure,1}).finish(reopened->revision+1);
    const std::array<Pixel,2> twice{Pixel{.28125f,0,0,.5f},Pixel{0,.28125f,0,.5f}};
    exactSamples(second,layer.id,twice);
}
}
int main(int argc,char** argv) {
    QCoreApplication application(argc,argv);
    try {
        QTemporaryDir directory(QCoreApplication::applicationDirPath()+"/editing-persistence-XXXXXX");
        expect(directory.isValid(),"Could not create owned build-directory persistence fixture");
        allFour(directory);selectedPlacement(directory);
        std::cout<<"Four adjustments plus transformed selection preserve canonical pixels, alpha, ICC and editing after native reopen\n";
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
