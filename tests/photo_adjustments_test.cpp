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
    expect(rejected,"Invalid photo adjustment was accepted");
}
AdjustmentParameters parameters(AdjustmentKind kind) {AdjustmentParameters p;p.kind=kind;return p;}
DocumentPtr fixture(TileStore& store,std::span<const Pixel> pixels,std::shared_ptr<const SourceProfile> profile={}) {
    const auto width=static_cast<int>(pixels.size());
    TileMap tiles;tiles.emplace(TileCoord{0,0},store.create(width,1,pixels));
    auto raster=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,width,1},PackedPixel{},std::move(tiles),0,std::move(profile));
    return std::make_shared<const DocumentSnapshot>(Id::generate(),Id::generate(),width,1,144,raster,0,"Photo",true,.75f);
}
Pixel pixel(const DocumentPtr& document,int x) {return document->singleLayer().raster->pixel(x,0);}
void closePixel(Pixel actual,Pixel expected,const char* message) {
    // Inputs and outputs use binary16; the analytic reference is not quantized at every intermediate.
    expect(std::abs(actual.r-expected.r)<.001f && std::abs(actual.g-expected.g)<.001f &&
        std::abs(actual.b-expected.b)<.001f && actual.a==expected.a,message);
}
void levelsAndCurves() {
    TileStore store(1024*1024);
    auto profile=std::make_shared<const SourceProfile>(SourceProfile{Id::generate(),{1,2,3}});
    const std::array<Pixel,3> pixels{fromStraightSrgb({0,.5f,1,.5f}),Pixel{-1,2,0,1},Pixel{}};
    auto document=fixture(store,pixels,profile);
    auto levels=parameters(AdjustmentKind::Levels);levels.levels={.25,.75,2,.1,.9};
    auto output=adjustLayer(document,document->singleLayer().id,store,levels).finish(1);
    // Encoded .5 normalizes to .5; gamma 2 takes sqrt(.5), then maps [.1,.9].
    const float midpoint=.1f+.8f*static_cast<float>(std::sqrt(.5));
    closePixel(pixel(output,0),fromStraightSrgb({.1f,midpoint,.9f,.5f}),"Levels gamma/output range analytic fixture differs");
    closePixel(pixel(output,1),fromStraightSrgb({.1f,.9f,.1f,1}),"Levels failed bounded extended-input endpoints");
    expect(pack(pixel(output,2))==PackedPixel{},"Levels changed transparent pixels");
    expect(output->resolution==144 && output->singleLayer().opacity==.75f && output->singleLayer().name=="Photo" &&
        output->singleLayer().raster->sourceProfile==profile,"Photo adjustment changed profile or metadata");
    auto curve=parameters(AdjustmentKind::Curves);curve.curve={{0,.1},{.25,.2},{.75,.6},{1,.9}};
    output=adjustLayer(document,document->singleLayer().id,store,curve).finish(1);
    closePixel(pixel(output,0),fromStraightSrgb({.1f,.4f,.9f,.5f}),"Curve piecewise-linear analytic fixture differs");
    closePixel(pixel(output,1),fromStraightSrgb({.1f,.9f,.1f,1}),"Curve bounded extended-input endpoints differ");
    curve.curve={{0,1},{1,0}};
    output=adjustLayer(document,document->singleLayer().id,store,curve).finish(1);
    closePixel(pixel(output,0),fromStraightSrgb({1,.5f,0,.5f}),"Curve descending outputs were not interpolated");
    levels.levels={0,1,1,.3,.3};
    output=adjustLayer(document,document->singleLayer().id,store,levels).finish(1);
    closePixel(pixel(output,0),fromStraightSrgb({.3f,.3f,.3f,.5f}),"Equal output levels must produce a constant channel");
}
void balanceAndIdentity() {
    TileStore store(1024*1024);
    const std::array<Pixel,2> pixels{Pixel{.125f,.25f,.0625f,.5f},Pixel{-2,4,3,1}};
    auto document=fixture(store,pixels);
    auto balance=parameters(AdjustmentKind::ColorBalance);balance.colorBalance={1,1};
    auto output=adjustLayer(document,document->singleLayer().id,store,balance).finish(1);
    expect(pack(pixel(output,0))==pack({.25f,.125f,.03125f,.5f}) &&
        pack(pixel(output,1))==pack({-4,2,1.5f,1}),"Relative balance failed independent linear dyadic gains");
    balance.colorBalance={-1,-1};
    output=adjustLayer(document,document->singleLayer().id,store,balance).finish(1);
    expect(pack(pixel(output,0))==pack({.0625f,.5f,.125f,.5f}),"Relative balance reversed gains incorrectly");
    DocumentHistory history(document);const auto bytes=store.usedBytes();
    auto curve=parameters(AdjustmentKind::Curves);curve.curve={{0,0},{.25,.25},{.6,.6},{1,1}};
    for(const auto& p:{parameters(AdjustmentKind::Levels),curve,parameters(AdjustmentKind::ColorBalance)}) {
        expect(!history.commit(adjustLayer(history.current(),document->singleLayer().id,store,p),"Neutral"),"Neutral photo controls created history");
        expect(history.current()==document && pack(pixel(history.current(),1))==pack(pixels[1]),"Neutral controls clipped extended input");
    }
    expect(store.usedBytes()==bytes && !history.canUndo(),"Neutral photo controls allocated backing");
}
void sharingHistoryAndSelection() {
    TileStore store(1024*1024);
    const std::array<Pixel,2> pixels{Pixel{.125f,.0625f,.03125f,.5f},Pixel{.125f,.0625f,.03125f,.5f}};
    auto single=fixture(store,pixels);auto first=single->singleLayer(),second=first;
    first.localToDocument.tx=2;second.id=Id::generate();second.siblingOrder=1;
    auto document=std::make_shared<const DocumentSnapshot>(Id::generate(),4,1,144,std::vector<LayerNode>{first,second});
    auto selected=selectDocument(document,Selection{Extent{2,0,1,1}}).finish(1);
    auto p=parameters(AdjustmentKind::ColorBalance);p.colorBalance={1,0};
    DocumentHistory history(selected);
    expect(history.commit(adjustLayer(selected,first.id,store,p),"Color balance"),"Photo adjustment failed to commit");
    const auto output=history.current();
    expect(pack(output->layer(first.id).raster->pixel(0,0))==pack({.25f,.0625f,.015625f,.5f}) &&
        pack(output->layer(first.id).raster->pixel(1,0))==pack(pixels[1]),"Photo adjustment ignored transformed selection");
    expect(output->layer(second.id).raster==second.raster && output->selection==selected->selection &&
        output->layer(first.id).localToDocument==first.localToDocument,"Photo adjustment lost sharing, selection or transform");
    expect(history.undoName()=="Color balance" && history.undo() && history.current()==selected && !history.canUndo(),"Photo adjustment undo not atomic");
    expect(history.redo() && history.current()==output && !history.canRedo(),"Photo adjustment redo lost output");
    first.localToDocument.tx=.5;
    document=std::make_shared<const DocumentSnapshot>(Id::generate(),3,1,72,std::vector<LayerNode>{first});
    selected=selectDocument(document,Selection{Extent{1,0,1,1}}).finish(1);
    const auto partial=adjustLayer(selected,first.id,store,p).finish(2);
    for(int x=0;x<2;++x) expect(pack(partial->layer(first.id).raster->pixel(x,0))==pack({.1875f,.0625f,.0234375f,.5f}),
        "Photo adjustment fractional selection was not blended once");
}
void invalidControls() {
    TileStore store(1024*1024);const std::array<Pixel,1> pixels{Pixel{.125f,.125f,.125f,.5f}};
    const auto document=fixture(store,pixels);const auto id=document->singleLayer().id;const auto bytes=store.usedBytes();
    const double nan=std::numeric_limits<double>::quiet_NaN(),inf=std::numeric_limits<double>::infinity();
    for(const auto& l:{LevelsParameters{-1,1,1,0,1},LevelsParameters{0,0,1,0,1},LevelsParameters{0,2,1,0,1},
        LevelsParameters{0,1,.09,0,1},LevelsParameters{0,1,11,0,1},LevelsParameters{0,1,1,-.1,1},
        LevelsParameters{0,1,1,.8,.2},LevelsParameters{0,1,1,0,1.1},LevelsParameters{nan,1,1,0,1},
        LevelsParameters{0,1,inf,0,1}}) {
        auto p=parameters(AdjustmentKind::Levels);p.levels=l;rejects([&]{adjustLayer(document,id,store,p);});
    }
    for(const auto& points:std::vector<std::vector<CurvePoint>>{{},{{0,0}},{{.1,0},{1,1}},{{0,0},{.9,1}},
        {{0,0},{.5,.5},{.5,.6},{1,1}},{{0,0},{.7,.5},{.4,.6},{1,1}},{{0,-.1},{1,1}},
        {{0,0},{1,1.1}},{{0,0},{nan,.5},{1,1}},{{0,0},{.5,inf},{1,1}}}) {
        auto p=parameters(AdjustmentKind::Curves);p.curve=points;rejects([&]{adjustLayer(document,id,store,p);});
    }
    auto large=parameters(AdjustmentKind::Curves);large.curve.clear();
    for(int i=0;i<17;++i)large.curve.push_back({i/16.,i/16.});
    rejects([&]{adjustLayer(document,id,store,large);});
    for(const auto& b:{ColorBalanceParameters{1.1,0},ColorBalanceParameters{0,-1.1},ColorBalanceParameters{nan,0},ColorBalanceParameters{0,inf}}) {
        auto p=parameters(AdjustmentKind::ColorBalance);p.colorBalance=b;rejects([&]{adjustLayer(document,id,store,p);});
    }
    expect(store.usedBytes()==bytes && pack(pixel(document,0))==pack(pixels[0]),"Invalid controls changed backing or input");
}
void sparseFailureAndCancellation() {
    TileStore store(1024*1024);
    auto raster=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,257,1},PackedPixel{});
    auto blank=std::make_shared<const DocumentSnapshot>(Id::generate(),Id::generate(),257,1,72,raster);
    for(auto kind:{AdjustmentKind::Levels,AdjustmentKind::Curves,AdjustmentKind::ColorBalance}) {
        auto p=parameters(kind);p.levels.outputBlack=.2;p.curve={{0,.2},{1,1}};p.colorBalance.warmth=1;
        const auto output=adjustLayer(blank,blank->singleLayer().id,store,p).finish(1);
        expect(output->singleLayer().raster->tiles.empty() && store.usedBytes()==0,"Photo adjustment materialized transparent defaults");
    }
    TileMap tiles;tiles.emplace(TileCoord{1,0},store.constant(1,1,{65504,0,0,1}));
    raster=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,257,1},pack({.125f,.125f,.125f,.5f}),std::move(tiles));
    auto document=std::make_shared<const DocumentSnapshot>(Id::generate(),Id::generate(),257,1,72,raster);
    auto p=parameters(AdjustmentKind::ColorBalance);p.colorBalance.warmth=1;
    const auto before=store.usedBytes();
    rejects([&]{adjustLayer(document,document->singleLayer().id,store,p);});
    expect(store.usedBytes()==before && raster->tiles.size()==1 && pack(raster->pixel(0,0))==raster->defaultValue,
        "Late balance overflow leaked earlier output tiles");
    auto memory=std::make_shared<MemoryAdmission>(1,0,[]{return MemorySample{1024*1024,0};});
    TileStore refused(1024*1024,memory);
    for(auto kind:{AdjustmentKind::Levels,AdjustmentKind::Curves,AdjustmentKind::ColorBalance}) {
        p=parameters(kind);p.levels.outputBlack=.2;p.curve={{0,.2},{1,1}};p.colorBalance.warmth=.5;
        rejects([&]{adjustLayer(document,document->singleLayer().id,refused,p);});
        std::stop_source stopped;stopped.request_stop();
        rejects([&]{adjustLayer(document,document->singleLayer().id,store,p,stopped.get_token());});
    }
    expect(refused.usedBytes()==0 && memory->snapshot().pendingCpu==0 && memory->snapshot().committedCpu==0,"Photo refusal leaked admission");
    auto defaultRaster=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,257,1},pack({.125f,.125f,.125f,.5f}));
    auto defaults=std::make_shared<const DocumentSnapshot>(Id::generate(),Id::generate(),257,1,72,defaultRaster);
    p=parameters(AdjustmentKind::Curves);p.curve={{0,0},{1,.5}};
    std::stop_source during;store.setSpillCallbacks({},[&](const TilePtr&){during.request_stop();});
    rejects([&]{adjustLayer(defaults,defaults->singleLayer().id,store,p,during.get_token());});
    expect(store.usedBytes()==before && defaultRaster->tiles.empty(),"Mid-command cancellation retained unpublished photo tiles");
}
}
int main() {
    try {
        levelsAndCurves();balanceAndIdentity();sharingHistoryAndSelection();invalidControls();sparseFailureAndCancellation();
        std::cout<<"Photo adjustments: five analytic, identity, history, selection and failure groups passed\n";
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
