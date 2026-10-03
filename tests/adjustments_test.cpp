#include "core/editor_commands.h"
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace compositor;
using namespace compositor::engine;
namespace {
void expect(bool condition,const char* message) {if(!condition) throw std::runtime_error(message);}
template<class F> void rejects(F action) {
    bool rejected=false;try {action();} catch(const std::exception&) {rejected=true;}
    expect(rejected,"Invalid adjustment was accepted");
}
DocumentPtr fixture(TileStore& store,std::span<const Pixel> pixels,int width,std::shared_ptr<const SourceProfile> profile={}) {
    TileMap tiles;tiles.emplace(TileCoord{0,0},store.create(width,1,pixels));
    auto raster=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,width,1},PackedPixel{},std::move(tiles),0,std::move(profile));
    return std::make_shared<const DocumentSnapshot>(Id::generate(),Id::generate(),width,1,144,raster,0,"Pixels",true,.75f);
}
PackedPixel sample(const DocumentPtr& document,int x) {return pack(document->singleLayer().raster->pixel(x,0));}
void analyticAdjustments() {
    TileStore store(4*1024*1024);
    const std::array<Pixel,2> exposed{Pixel{.125f,.0625f,.03125f,.5f},Pixel{}};
    auto profile=std::make_shared<const SourceProfile>(SourceProfile{Id::generate(),{1,2,3}});
    auto document=fixture(store,exposed,2,profile);
    auto output=adjustLayer(document,document->singleLayer().id,store,{AdjustmentKind::Exposure,1}).finish(1);
    expect(sample(output,0)==pack({.25f,.125f,.0625f,.5f}) && sample(output,1)==PackedPixel{},
        "Exposure failed independent linear-light/alpha fixture");
    expect(sample(document,0)==pack(exposed[0]),"Adjustment mutated its immutable input");
    expect(output->resolution==144 && output->singleLayer().opacity==.75f && output->singleLayer().name=="Pixels" && output->singleLayer().raster->sourceProfile==profile,
        "Adjustment changed document/layer metadata");

    const std::array<Pixel,2> tones{fromStraightSrgb({.25f,.25f,.25f,.5f}),fromStraightSrgb({.75f,.75f,.75f,1})};
    document=fixture(store,tones,2);
    output=adjustLayer(document,document->singleLayer().id,store,{AdjustmentKind::Brightness,.25}).finish(1);
    expect(sample(output,0)==pack(fromStraightSrgb({.5f,.5f,.5f,.5f})),"Brightness quarter-offset encoded fixture differs");
    output=adjustLayer(document,document->singleLayer().id,store,{AdjustmentKind::Contrast,1}).finish(1);
    expect(sample(output,0)==pack({0,0,0,.5f}) && sample(output,1)==pack({1,1,1,1}),
        "Contrast failed independent black/white endpoints");
    const std::array<Pixel,1> red{Pixel{.5f,0,0,.5f}};
    document=fixture(store,red,1);
    output=adjustLayer(document,document->singleLayer().id,store,{AdjustmentKind::Saturation,0}).finish(1);
    expect(sample(output,0)==pack(fromStraightSrgb({.2126f,.2126f,.2126f,.5f})),
        "Desaturation failed independent red Rec709 luminance fixture");
}
void historyAndSharing() {
    TileStore store(4*1024*1024);
    const std::array<Pixel,2> pixels{Pixel{.125f,0,0,.5f},Pixel{0,.125f,0,.5f}};
    auto single=fixture(store,pixels,2);
    auto first=single->singleLayer(),second=first;second.id=Id::generate();second.siblingOrder=1;
    auto document=std::make_shared<const DocumentSnapshot>(single->id,2,1,72,std::vector<LayerNode>{first,second});
    DocumentHistory history(document);
    for(const auto parameters:{AdjustmentParameters{AdjustmentKind::Exposure,0},AdjustmentParameters{AdjustmentKind::Brightness,0},
        AdjustmentParameters{AdjustmentKind::Contrast,0},AdjustmentParameters{AdjustmentKind::Saturation,1}})
        expect(!history.commit(adjustLayer(history.current(),first.id,store,parameters),"Neutral"),"Neutral adjustment created an undo entry");
    expect(history.commit(adjustLayer(history.current(),first.id,store,{AdjustmentKind::Exposure,1}),"Exposure"),
        "Adjustment did not commit one history entry");
    const auto adjusted=history.current();
    expect(adjusted->layer(second.id).raster==second.raster && adjusted->layer(first.id).raster!=first.raster,
        "Adjustment changed a duplicate's shared immutable raster");
    expect(history.undoName()=="Exposure" && history.undo() && history.current()==document && !history.canUndo(),
        "Adjustment did not undo as exactly one command");
    expect(history.redo() && history.current()==adjusted && !history.canRedo(),"Adjustment redo lost the immutable output");
}
void selectionsAndTransforms() {
    TileStore store(4*1024*1024);
    const std::array<Pixel,2> pixels{Pixel{.125f,0,0,.5f},Pixel{0,.125f,0,.5f}};
    auto original=fixture(store,pixels,2);
    auto target=original->singleLayer();
    LayerNode folder{Id::generate()};folder.folder=true;folder.localToDocument.tx=15;
    target.parent=folder.id;target.localToDocument.tx=2;
    auto document=std::make_shared<const DocumentSnapshot>(Id::generate(),4,1,72,std::vector<LayerNode>{folder,target});
    auto selected=selectDocument(document,Selection{Extent{2,0,1,1}}).finish(document->revision+1);
    auto output=adjustLayer(selected,target.id,store,{AdjustmentKind::Exposure,1}).finish(selected->revision+1);
    expect(pack(output->layer(target.id).raster->pixel(0,0))==pack({.25f,0,0,.5f}) &&
        pack(output->layer(target.id).raster->pixel(1,0))==pack(pixels[1]),
        "Selection did not follow document-space layer placement");
    selected=selectDocument(document,Selection{Extent{2,0,1,1},SelectionShape::Rectangle,true}).finish(document->revision+1);
    output=adjustLayer(selected,target.id,store,{AdjustmentKind::Exposure,1}).finish(selected->revision+1);
    expect(pack(output->layer(target.id).raster->pixel(0,0))==pack(pixels[0]) &&
        pack(output->layer(target.id).raster->pixel(1,0))==pack({0,.25f,0,.5f}),
        "Inverted document selection affected the wrong transformed source pixel");

    target.parent.reset();target.localToDocument.tx=.5;
    document=std::make_shared<const DocumentSnapshot>(Id::generate(),3,1,72,std::vector<LayerNode>{target});
    selected=selectDocument(document,Selection{Extent{1,0,1,1}}).finish(document->revision+1);
    output=adjustLayer(selected,target.id,store,{AdjustmentKind::Exposure,1}).finish(selected->revision+1);
    expect(pack(output->layer(target.id).raster->pixel(0,0))==pack({.1875f,0,0,.5f}) &&
        pack(output->layer(target.id).raster->pixel(1,0))==pack({0,.1875f,0,.5f}),
        "Fractional source selection edge was not blended once at half coverage");

    auto raster=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,2,2},pack({.125f,.125f,.125f,.5f}));
    document=std::make_shared<const DocumentSnapshot>(Id::generate(),Id::generate(),2,2,72,raster);
    selected=selectDocument(document,Selection{Extent{0,0,2,2},SelectionShape::Ellipse}).finish(document->revision+1);
    output=adjustLayer(selected,document->singleLayer().id,store,{AdjustmentKind::Exposure,1}).finish(selected->revision+1);
    // Each quadrant contains 52 of the fixed 64 sample centers inside the circle.
    expect(pack(output->singleLayer().raster->pixel(0,0))==pack({.2265625f,.2265625f,.2265625f,.5f}),
        "Ellipse adjustment failed the independent 52/64 coverage fixture");
    expect(output->selection==selected->selection,"Adjustment discarded its selection state");
}
void sparseDefaultsAndRefusal() {
    TileStore store(4*1024*1024);
    auto raster=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{-1,0,258,1},pack({.125f,.125f,.125f,.5f}));
    auto document=std::make_shared<const DocumentSnapshot>(Id::generate(),Id::generate(),258,1,72,raster);
    auto output=adjustLayer(document,document->singleLayer().id,store,{AdjustmentKind::Exposure,1}).finish(1);
    for(auto x:{-1,0,255,256}) expect(pack(output->singleLayer().raster->pixel(x,0))==pack({.25f,.25f,.25f,.5f}),
        "Adjustment skipped an implicit default or clipped edge tile");
    expect(document->singleLayer().raster->tiles.empty(),"Adjustment materialized tiles into its input");
    auto memory=std::make_shared<MemoryAdmission>(1,0,[]{return MemorySample{1024*1024,0};});
    TileStore refused(4*1024*1024,memory);
    rejects([&]{adjustLayer(document,document->singleLayer().id,refused,{AdjustmentKind::Exposure,1});});
    expect(refused.usedBytes()==0 && memory->snapshot().pendingCpu==0 && memory->snapshot().committedCpu==0,
        "Refused adjustment leaked allocation/admission");
    for(auto parameters:{AdjustmentParameters{AdjustmentKind::Exposure,9},AdjustmentParameters{AdjustmentKind::Brightness,-2},
        AdjustmentParameters{AdjustmentKind::Contrast,-1},AdjustmentParameters{AdjustmentKind::Saturation,3},
        AdjustmentParameters{AdjustmentKind::Exposure,std::numeric_limits<double>::quiet_NaN()}})
        rejects([&]{adjustLayer(document,document->singleLayer().id,store,parameters);});
    std::stop_source stop;stop.request_stop();
    rejects([&]{adjustLayer(document,document->singleLayer().id,store,{AdjustmentKind::Exposure,1},stop.get_token());});
    TileMap extremeTiles;
    extremeTiles.emplace(TileCoord{1,0},store.constant(1,1,{65504,-65504,1,1}));
    auto extremeRaster=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,257,1},
        pack({.125f,0,0,.5f}),std::move(extremeTiles));
    auto extreme=std::make_shared<const DocumentSnapshot>(Id::generate(),Id::generate(),257,1,72,extremeRaster);
    const auto beforeOverflow=store.usedBytes();
    // First default tile changes successfully, then the final explicit tile
    // exceeds finite binary16. Neither partial output may escape the command.
    rejects([&]{adjustLayer(extreme,extreme->singleLayer().id,store,{AdjustmentKind::Exposure,1});});
    expect(store.usedBytes()==beforeOverflow && extremeRaster->tiles.size()==1 &&
        pack(extremeRaster->pixel(0,0))==pack({.125f,0,0,.5f}) &&
        pack(extremeRaster->pixel(256,0))==pack({65504,-65504,1,1}),
        "Late exposure overflow leaked partial output or changed immutable input");
    const auto before=store.usedBytes();
    std::stop_source during;
    store.setSpillCallbacks({},[&](const TilePtr&){during.request_stop();});
    rejects([&]{adjustLayer(document,document->singleLayer().id,store,{AdjustmentKind::Exposure,1},during.get_token());});
    expect(store.usedBytes()==before && document->singleLayer().raster->tiles.empty(),
        "Canceled adjustment retained unpublished tiles or changed its input");
}
}
int main() {
    try {
        analyticAdjustments();historyAndSharing();selectionsAndTransforms();sparseDefaultsAndRefusal();
        std::cout<<"Basic adjustments analytic pixels, alpha, sparse defaults, history, sharing and refusal passed\n";
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
