#include "core/soft_mask_edits.h"
#include <array>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace compositor;
using namespace compositor::engine;
namespace {
void expect(bool value,const char* message) {if(!value) throw std::runtime_error(message);}
template<class F> void rejects(F operation) {
    bool rejected=false;try {operation();} catch(const std::exception&) {rejected=true;}
    expect(rejected,"Invalid soft mask edit was accepted");
}
DocumentPtr fixture(TileStore& store,Extent extent,float defaultAlpha=1,Affine placement={},
                    const std::function<float(std::int64_t,std::int64_t)>& coverage={}) {
    auto raster=std::make_shared<const RasterSnapshot>(Id::generate(),extent,pack({.125f,.25f,.0625f,1}));
    TileMap tiles;
    if(coverage) for(auto ty=floorTile(extent.y);ty<=floorTile(extent.y+extent.height-1);++ty)
        for(auto tx=floorTile(extent.x);tx<=floorTile(extent.x+extent.width-1);++tx) {
            const TileCoord coordinate{tx,ty};const auto bounds=tileExtent(extent,coordinate);
            std::vector<Pixel> pixels(static_cast<std::size_t>(bounds.width*bounds.height));
            for(int y=0;y<bounds.height;++y) for(int x=0;x<bounds.width;++x)
                pixels[static_cast<std::size_t>(y*bounds.width+x)]={0,0,0,coverage(bounds.x+x,bounds.y+y)};
            tiles.emplace(coordinate,store.create(static_cast<int>(bounds.width),static_cast<int>(bounds.height),pixels));
        }
    auto mask=std::make_shared<const RasterSnapshot>(Id::generate(),extent,pack({0,0,0,defaultAlpha}),std::move(tiles));
    LayerNode layer{Id::generate()};layer.raster=raster;layer.mask=mask;layer.localToDocument=placement;
    return std::make_shared<const DocumentSnapshot>(Id::generate(),600,20,72,std::vector<LayerNode>{layer});
}
float alpha(const DocumentPtr& input,std::int64_t x,std::int64_t y=0) {return input->singleLayer().mask->pixel(x,y).a;}
void softPaintAnalyticAndHistory() {
    TileStore store(8*1024*1024);auto input=fixture(store,{0,0,5,5});const auto id=input->singleLayer().id;
    BrushSettings settings;settings.diameter=4;settings.hardness=0;settings.opacity=.5;settings.color={1,0,0,1};settings.erasing=true;
    const std::array points{Coordinate{2.5,2.5}};
    auto hidden=paintLayerMask(input,id,store,points,settings,false).finish(1);
    const double distance=std::sqrt(.25*.25+.25*.25),u=distance/2;
    const double coverage=(std::exp(-2.5*u*u)-std::exp(-2.5))/(1-std::exp(-2.5));
    expect(std::abs(alpha(hidden,2,2)-(1-.5*coverage))<.0005,"Soft hide coverage differs from independent Gaussian brush sample");
    expect(hidden->singleLayer().raster==input->singleLayer().raster && alpha(input,2,2)==1,"Soft paint changed original mask/color source");
    auto black=fixture(store,{0,0,5,5},0);auto revealed=paintLayerMask(black,black->singleLayer().id,store,points,settings,true).finish(1);
    expect(std::abs(alpha(revealed,2,2)-.5*coverage)<.0003,"Reveal bool did not override color/erasing settings");
    for(int y=0;y<5;++y) for(int x=0;x<5;++x) {
        const auto pixel=hidden->singleLayer().mask->pixel(x,y);expect(pixel.r==0 && pixel.g==0 && pixel.b==0 && pixel.a>=.5f && pixel.a<=1,"Mask paint changed RGB or exceeded stroke opacity");
    }
    DocumentHistory history(input);
    expect(history.commit(paintLayerMask(input,id,store,points,settings,false),"Hide mask") && history.undo() && history.current()==input && history.redo(),"Soft mask edit failed one-step history");
}
void transformedSelectionAndAdjusted() {
    TileStore store(8*1024*1024);Affine placement;placement.a=2;placement.d=2;placement.tx=10;placement.ty=2;
    auto input=fixture(store,{-2,-1,4,3},1,placement);const auto id=input->singleLayer().id;
    input=selectDocument(input,Selection{Extent{6,0,2,2}}).finish(1);
    auto adjusted=setRevisableAdjustments(input,id,store,{{AdjustmentKind::Exposure,1}}).finish(2);
    const auto cache=adjusted->singleLayer().raster,base=adjusted->singleLayer().adjustments->source;
    BrushSettings settings;settings.diameter=8;settings.hardness=1;settings.opacity=.5;
    const std::array points{Coordinate{7,1}};
    auto result=paintLayerMask(adjusted,id,store,points,settings,false).finish(3);
    expect(alpha(result,-2,-1)==.5f && alpha(result,-1,-1)==1,"Transformed selection did not constrain signed source mask painting");
    expect(result->singleLayer().raster==cache && result->singleLayer().adjustments->source==base &&
        result->singleLayer().adjustments==adjusted->singleLayer().adjustments,"Mask painting changed retained adjustment source/cache");
    expect(result->selection==adjusted->selection && alpha(adjusted,-2,-1)==1,"Mask painting changed immutable input or selection");
}
void independentFeatherAndSeams() {
    TileStore store(8*1024*1024);
    auto input=fixture(store,{-2,0,260,1},0,{},[](auto x,auto){return x==255 ? 1.f : 0.f;});const auto id=input->singleLayer().id;
    auto selected=selectDocument(input,Selection{Extent{0,0,1,1}}).finish(1);
    auto result=featherLayerMask(selected,id,store,2).finish(2);
    const double a=std::exp(-1.125),b=std::exp(-4.5),total=1+2*a+2*b;
    for(int offset=-2;offset<=2;++offset) {
        const double expected=(offset==0 ? 1 : std::abs(offset)==1 ? a : b)/total;
        expect(std::abs(alpha(result,255+offset)-expected)<.0004,"Feather differs from independent normalized Gaussian across tile seam");
    }
    expect(alpha(result,252)==0 && alpha(result,257)>0 && alpha(input,255)==1 && alpha(input,256)==0,"Feather halo/source preservation failed");
    expect(result->singleLayer().raster==input->singleLayer().raster && result->selection==selected->selection,"Feather changed color or document selection");
    expect(featherLayerMask(input,id,store,0).finish(1)==input,"Radius zero created a mask edit");
    auto uniform=fixture(store,{0,0,5,3},1);expect(featherLayerMask(uniform,uniform->singleLayer().id,store,2).finish(1)==uniform,"Uniform mask edge clamping changed white coverage");
    auto corner=fixture(store,{0,0,3,1},0,{},[](auto x,auto){return x==0 ? 1.f : 0.f;});
    auto clamped=featherLayerMask(corner,corner->singleLayer().id,store,1).finish(1);
    const double w=std::exp(-4.5);expect(std::abs(alpha(clamped,0)-(1+w)/(1+2*w))<.0004,"Mask edge did not clamp to edge coverage");
}
void invalidCancellationAndRefusal() {
    TileStore store(8*1024*1024);auto input=fixture(store,{0,0,7,1},0,{},[](auto x,auto){return x==3 ? 1.f : 0.f;});const auto id=input->singleLayer().id;
    for(const auto radius:{-1.,33.,std::numeric_limits<double>::quiet_NaN()}) rejects([&]{featherLayerMask(input,id,store,radius);});
    auto disabled=setLayerMaskEnabled(input,id,false).finish(1);rejects([&]{featherLayerMask(disabled,id,store,1);});
    BrushSettings settings;const std::array points{Coordinate{3.5,.5}};
    rejects([&]{paintLayerMask(disabled,id,store,points,settings,false);});
    auto missing=removeLayerMask(input,id).finish(1);rejects([&]{featherLayerMask(missing,id,store,1);});
    std::stop_source canceled;canceled.request_stop();rejects([&]{featherLayerMask(input,id,store,1,canceled.get_token());});
    rejects([&]{paintLayerMask(input,id,store,points,settings,false,canceled.get_token());});
    auto admission=std::make_shared<MemoryAdmission>(1,0,[]{return MemorySample{1024*1024,0};});TileStore refused(8*1024*1024,admission);
    rejects([&]{featherLayerMask(input,id,refused,1);});
    expect(admission->snapshot().pendingCpu==0 && admission->snapshot().committedCpu==0 && alpha(input,3)==1,"Refused feather changed source or leaked charge");
    std::stop_source during;std::size_t calls=0;
    auto cancelAdmission=std::make_shared<MemoryAdmission>(4*1024*1024,0,[&]{if(++calls==3) during.request_stop();return MemorySample{8*1024*1024,0};});
    TileStore interrupted(8*1024*1024,cancelAdmission);
    rejects([&]{featherLayerMask(input,id,interrupted,1,during.get_token());});
    expect(during.stop_requested() && alpha(input,3)==1 && cancelAdmission->snapshot().pendingCpu==0 && cancelAdmission->snapshot().committedCpu==0,"Mid-feather cancellation published output or leaked charges");
}
}
int main() {
    try {softPaintAnalyticAndHistory();transformedSelectionAndAdjusted();independentFeatherAndSeams();invalidCancellationAndRefusal();std::cout<<"All 4 soft mask test groups passed\n";}
    catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
