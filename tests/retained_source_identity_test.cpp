#include "core/editor_commands.h"
#include "core/retouch.h"
#include <array>
#include <iostream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

using namespace compositor;
using namespace compositor::engine;
namespace {
void check(bool value,const char* message) {if(!value) throw std::runtime_error(message);}
DocumentPtr fixture(TileStore& store) {
    const std::array pixels{Pixel{.125f,0,0,1},Pixel{0,.25f,0,1},Pixel{0,0,.5f,1},Pixel{.25f,.25f,.25f,1}};
    TileMap tiles;tiles.emplace(TileCoord{0,0},store.create(4,1,pixels));
    auto raster=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,4,1},PackedPixel{},std::move(tiles));
    return std::make_shared<const DocumentSnapshot>(Id::generate(),Id::generate(),4,1,72,raster);
}
RetouchStroke stroke() {
    RetouchStroke value;value.sourceAnchor={.5,.5};value.points={{2.5,.5}};
    value.diameter=2;value.hardness=1;value.opacity=1;value.canvasWidth=4;value.canvasHeight=1;
    return value;
}
DocumentPtr retained(DocumentPtr input,const Id& target,TileStore& store,bool retouch,bool adjustments) {
    if(retouch) input=setRetouchStrokes(input,target,store,{stroke()}).finish(input->revision+1);
    if(adjustments) input=setRevisableAdjustments(input,target,store,{{AdjustmentKind::Exposure,1}}).finish(input->revision+1);
    return input;
}
void sharedPlainPeer() {
    for(const auto& flags:std::array{std::pair{false,true},std::pair{true,false},std::pair{true,true}}) {
        TileStore store(4*1024*1024);auto original=fixture(store);
        auto a=original->singleLayer(),b=a;b.id=Id::generate();b.siblingOrder=1;
        auto duplicate=std::make_shared<const DocumentSnapshot>(original->id,4,1,72,std::vector<LayerNode>{a,b});
        auto edited=retained(duplicate,a.id,store,flags.first,flags.second);
        const auto beforeA=edited->layer(a.id);
        std::array<PackedPixel,4> preserved{};
        for(int x=0;x<4;++x) preserved[static_cast<std::size_t>(x)]=pack(beforeA.raster->pixel(x,0));
        BrushSettings brush;brush.diameter=2;brush.hardness=1;brush.color={1,1,0,1};
        const std::array points{Coordinate{1.5,.5}};
        auto painted=brushStroke(edited,b.id,store,points,brush).finish(edited->revision+1);
        check(painted->layer(b.id).raster->id!=b.raster->id,"Painting peer of retained source must use fresh asset ID");
        check(painted->layer(b.id).raster->pixel(1,0)!=b.raster->pixel(1,0),"Peer brush must change actual pixels");
        check(painted->layer(a.id).raster==beforeA.raster && painted->layer(a.id).adjustments==beforeA.adjustments &&
            painted->layer(a.id).retouch==beforeA.retouch,"Peer brush changed retained layer backing");
        for(int x=0;x<4;++x) check(pack(painted->layer(a.id).raster->pixel(x,0))==preserved[static_cast<std::size_t>(x)],"Peer brush changed retained pixels");
        if(beforeA.retouch) check(beforeA.retouch->source==b.raster,"Retouch lost exact original shared source");
        else check(beforeA.adjustments->source==b.raster,"Adjustment lost exact original shared source");
        check(pack(b.raster->pixel(1,0))==pack({0,.25f,0,1}),"Original source mutated");
    }
}
void masksAndRasterize() {
    TileStore store(4*1024*1024);auto original=fixture(store);const auto id=original->singleLayer().id;
    auto edited=retained(original,id,store,true,true);
    auto masked=createLayerMask(edited,id,store,false).finish(edited->revision+1);
    BrushSettings brush;brush.paintMask=true;brush.erasing=true;brush.diameter=2;brush.hardness=1;
    const std::array points{Coordinate{1.5,.5}};
    auto painted=brushStroke(masked,id,store,points,brush).finish(masked->revision+1);
    check(painted->singleLayer().mask->pixel(1,0).a<masked->singleLayer().mask->pixel(1,0).a,"Retained layer mask brush did not modify coverage");
    check(painted->singleLayer().adjustments==edited->singleLayer().adjustments && painted->singleLayer().retouch==edited->singleLayer().retouch &&
        painted->singleLayer().raster==edited->singleLayer().raster,"Mask painting changed retained source or raster");
    auto flattened=rasterizeLayerAdjustments(painted,id).finish(painted->revision+1);
    check(!flattened->singleLayer().adjustments && !flattened->singleLayer().retouch,"Rasterize must clear both retained stacks");
    check(flattened->singleLayer().raster->id!=painted->singleLayer().raster->id &&
        flattened->singleLayer().raster->tiles==painted->singleLayer().raster->tiles,"Rasterize must preserve exact tiles with fresh identity");
    check(flattened->singleLayer().mask==painted->singleLayer().mask,"Rasterize changed linked mask");
}
std::size_t metadataBytes(const RetouchStack& stack) {
    std::size_t bytes=sizeof(RetouchStack)+stack.strokes.capacity()*sizeof(RetouchStroke);
    for(const auto& stroke:stack.strokes) bytes+=stroke.points.capacity()*sizeof(Coordinate);
    return bytes;
}
void historySourceAccounting() {
    TileStore store(4*1024*1024);auto original=fixture(store);const auto id=original->singleLayer().id;
    auto edited=retained(original,id,store,true,true);
    DocumentHistory history(edited);
    check(history.commit(rasterizeLayerAdjustments(edited,id),"Rasterize"),"Missing rasterize history entry");
    std::unordered_set<const Tile*> current,old;
    for(const auto& [coordinate,tile]:history.current()->singleLayer().raster->tiles) current.insert(tile.get());
    const auto& layer=edited->singleLayer();
    std::size_t expected=0;
    for(const auto& raster:{layer.raster,layer.adjustments->source,layer.retouch->source})
        for(const auto& [coordinate,tile]:raster->tiles)
            if(!current.contains(tile.get()) && old.insert(tile.get()).second) expected+=tile->retainedBytes();
    check(expected>=original->singleLayer().raster->tiles.begin()->second->retainedBytes(),"Fixture must retain original source outside current raster");
    check(history.retainedBytes()==expected+metadataBytes(*layer.retouch),"History must charge retained source tiles and detached stroke metadata once");
    check(history.undo() && history.current()==edited,"Undo lost retained stacks");
    check(history.redo() && !history.current()->singleLayer().retouch && !history.current()->singleLayer().adjustments,"Redo failed to detach retained stacks");
}
void metadataRevisionAccounting() {
    TileStore store(4*1024*1024);auto original=fixture(store);const auto id=original->singleLayer().id;
    auto firstStroke=stroke();firstStroke.opacity=0;
    auto first=setRetouchStrokes(original,id,store,{firstStroke}).finish(1);
    DocumentHistory history(first);
    auto revisedStroke=firstStroke;revisedStroke.points.push_back({3.5,.5});
    check(history.commit(setRetouchStrokes(first,id,store,{revisedStroke}),"Revise stroke"),"Neutral stroke parameter revision must commit");
    check(history.current()->singleLayer().raster->tiles==first->singleLayer().raster->tiles,"Neutral revisions must share unchanged tile backing");
    check(history.retainedBytes()==metadataBytes(*first->singleLayer().retouch),"History must retain old stroke metadata even when no tile changes");
    const auto revisedBytes=metadataBytes(*history.current()->singleLayer().retouch);
    check(history.undo() && history.retainedBytes()==revisedBytes,"Undo must charge revised metadata capacity without double charging current stack");
    check(history.redo(),"Redo stroke metadata revision failed");
    auto a=first->singleLayer(),b=a;b.id=Id::generate();b.siblingOrder=1;
    auto duplicate=std::make_shared<const DocumentSnapshot>(first->id,4,1,72,std::vector<LayerNode>{a,b},1);
    DocumentHistory shared(duplicate);
    check(shared.commit(rasterizeLayerAdjustments(shared.current(),a.id),"Rasterize first"),"First duplicate rasterization missing");
    check(shared.retainedBytes()==0,"Current duplicate must keep shared metadata out of history-only charge");
    check(shared.commit(rasterizeLayerAdjustments(shared.current(),b.id),"Rasterize second"),"Second duplicate rasterization missing");
    check(shared.retainedBytes()==metadataBytes(*a.retouch),"Detached shared stroke metadata must be charged once across duplicates and history entries");
}
}
int main() {
    try {sharedPlainPeer();masksAndRasterize();historySourceAccounting();metadataRevisionAccounting();std::cout<<"4 retained source identity groups passed\n";return 0;}
    catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
