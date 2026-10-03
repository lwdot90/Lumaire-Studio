#include "core/editor_commands.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <utility>
using namespace compositor;
using namespace compositor::engine;
namespace {
DocumentPtr selectionCandidate(DocumentPtr input,std::optional<Selection> selection) {
    return selectDocument(input,std::move(selection)).finish(input->revision+1);
}
DocumentPtr maskCandidate(DocumentPtr input,const Id& id,TileStore& store,bool fromSelection,std::stop_token stop) {
    return createLayerMask(input,id,store,fromSelection,stop).finish(input->revision+1);
}
DocumentPtr removedCandidate(DocumentPtr input,const Id& id) {return removeLayerMask(input,id).finish(input->revision+1);}
DocumentPtr enabledCandidate(DocumentPtr input,const Id& id,bool enabled) {return setLayerMaskEnabled(input,id,enabled).finish(input->revision+1);}
void check(bool okay,const char* message) {if(!okay) throw std::runtime_error(message);}
template<class Error,class Action> void rejects(Action action,const char* message) {bool rejected=false;try {action();}catch(const Error&) {rejected=true;}check(rejected,message);}
DocumentPtr document(int width,int height,Affine placement={}) {
    auto raster=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,width,height},pack(Pixel{.25f,.5f,.75f,1}));
    LayerNode layer{Id::generate()};layer.raster=raster;layer.localToDocument=placement;
    return std::make_shared<const DocumentSnapshot>(Id::generate(),1000,1000,72,std::vector<LayerNode>{layer});
}
void defaultsAndHistory() {
    TileStore store(8*1024*1024);auto initial=document(600,260);const auto id=initial->singleLayer().id;
    const auto full=maskCandidate(initial,id,store,false,{});
    const auto mask=full->layer(id).mask;
    check(mask && mask->extent==initial->layer(id).raster->extent && mask->tiles.empty(),"Full white mask has sparse default backing");
    check(mask->defaultValue==pack(Pixel{0,0,0,1}),"White mask canonical RGB zero alpha one");
    check(full->layer(id).raster==initial->layer(id).raster,"Mask command shares color raster");
    DocumentHistory history(initial);auto edit=history.begin();edit.setLayers(full->layers());
    check(history.commit(edit,"Add mask") && history.canUndo(),"Add mask is one undo command");
    check(history.undo() && !history.current()->layer(id).mask,"Undo removes mask");
    check(history.redo() && history.current()->layer(id).mask==mask,"Redo shares immutable mask");
    const auto disabled=enabledCandidate(full,id,false);check(!disabled->layer(id).maskEnabled && disabled->layer(id).mask==mask,"Disable preserves mask backing");
    check(enabledCandidate(disabled,id,false)==disabled,"Repeated disabled state is no-op");
    const auto removed=removedCandidate(disabled,id);check(!removed->layer(id).mask,"Delete removes only mask");
    check(removedCandidate(removed,id)==removed,"Repeated mask deletion no-op");
}
void transformedCoverage() {
    TileStore store(8*1024*1024);
    auto original=document(9,7,Affine{0,2,-1.5,0,30,4});const auto id=original->singleLayer().id;
    for(const auto shape:{SelectionShape::Rectangle,SelectionShape::Ellipse}) for(const bool inverted:{false,true}) {
        auto selected=selectionCandidate(original,Selection{Extent{20,8,8,10},shape,inverted});
        const auto masked=maskCandidate(selected,id,store,true,{});const auto mask=masked->layer(id).mask;
        for(int y=0;y<7;++y) for(int x=0;x<9;++x) {
            // Independent scalar oracle for this rotation/scale; no production
            // selection helper or affine mapping is used here.
            unsigned count=0;
            for(int sy=0;sy<8;++sy) for(int sx=0;sx<8;++sx) {
                const double dx=30-1.5*(y+(sy+.5)/8),dy=4+2*(x+(sx+.5)/8);
                bool inside=dx>=20 && dx<28 && dy>=8 && dy<18;
                if(shape==SelectionShape::Ellipse) inside=inside && std::pow((dx-24)/4,2)+std::pow((dy-13)/5,2)<=1;
                count+=(inside!=inverted)?1u:0u;
            }
            const float alpha=static_cast<float>(count)/64;
            check(mask->pixel(x,y)==Pixel{0,0,0,alpha},"Transformed mask differs from independent pixel oracle");
        }
        check(masked->layer(id).localToDocument==original->layer(id).localToDocument,"Mask remains linked to original layer placement");
    }
}
void sparseBoundariesAndSharing() {
    TileStore store(8*1024*1024);auto initial=document(600,260);const auto id=initial->singleLayer().id;
    for(const bool inverted:{false,true}) {
        const auto selected=selectionCandidate(initial,Selection{Extent{257,257,2,2},SelectionShape::Rectangle,inverted});
        const auto masked=maskCandidate(selected,id,store,true,{});const auto mask=masked->layer(id).mask;
        check(mask->defaultValue==PackedPixel{},"Selection mask sparse default excludes off-canvas source pixels");
        check(inverted ? mask->tiles.size()==6 : mask->tiles.size()==1,"Selection mask omits tiles equal to transparent sparse default");
        for(const auto point:{std::pair{0,0},std::pair{256,257},std::pair{257,257},std::pair{258,258},std::pair{259,259},std::pair{599,259}}) {
            const bool in=point.first>=257 && point.first<259 && point.second>=257 && point.second<259;
            check(mask->pixel(point.first,point.second)==Pixel{0,0,0,(in!=inverted)?1.f:0.f},"Sparse hole or edge coverage incorrect");
        }
    }
    for(const bool inverted:{false,true}) {
        const auto selected=selectionCandidate(initial,Selection{Extent{0,0,0,0},SelectionShape::Ellipse,inverted});
        const auto mask=maskCandidate(selected,id,store,true,{})->layer(id).mask;
        check(mask->defaultValue==PackedPixel{} && (inverted ? mask->tiles.size()==6 : mask->tiles.empty()),"Empty active selection produces empty mask or canvas-bounded inverted mask");
    }
    auto nodes=initial->layers();auto clone=nodes.front();clone.id=Id::generate();clone.siblingOrder=1;nodes.push_back(clone);
    const auto cloned=std::make_shared<const DocumentSnapshot>(Id::generate(),1000,1000,72,std::move(nodes));
    const auto masked=maskCandidate(cloned,id,store,false,{});
    check(masked->layer(id).raster==masked->layer(clone.id).raster && !masked->layer(clone.id).mask,"Adding mask preserves clone color sharing and leaves other mask unchanged");
}
void invertedCanvasClipping() {
    TileStore store(8*1024*1024);
    auto source=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,8,8},pack(Pixel{1,1,1,1}));
    LayerNode layer{Id::generate()};layer.raster=source;layer.localToDocument.tx=-2;layer.localToDocument.ty=-2;
    auto original=std::make_shared<const DocumentSnapshot>(Id::generate(),4,4,72,std::vector<LayerNode>{layer});
    for(const bool empty:{false,true}) {
        const auto selection=Selection{empty ? Extent{0,0,0,0} : Extent{1,1,2,2},SelectionShape::Rectangle,true};
        const auto selected=selectionCandidate(original,selection);
        const auto mask=maskCandidate(selected,layer.id,store,true,{})->layer(layer.id).mask;
        check(mask->defaultValue==PackedPixel{},"Inverted off-canvas source remains sparse transparent");
        for(int y=0;y<8;++y) for(int x=0;x<8;++x) {
            const double dx=x-1.5,dy=y-1.5;
            const bool canvas=dx>=0 && dx<4 && dy>=0 && dy<4;
            const bool selectedPixel=!empty && dx>=1 && dx<3 && dy>=1 && dy<3;
            check(mask->pixel(x,y)==Pixel{0,0,0,(canvas && !selectedPixel)?1.f:0.f},"Inverted mask incorrectly extends beyond document canvas");
        }
    }
}
void rotatedCanvasClipping() {
    TileStore store(8*1024*1024);
    auto source=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,9,7},pack(Pixel{1,1,1,1}));
    LayerNode layer{Id::generate()};layer.raster=source;layer.localToDocument=Affine{0,2,-1.5,0,8,-5};
    auto original=std::make_shared<const DocumentSnapshot>(Id::generate(),6,6,72,std::vector<LayerNode>{layer});
    for(const auto shape:{SelectionShape::Rectangle,SelectionShape::Ellipse}) {
        const auto selected=selectionCandidate(original,Selection{Extent{1,1,4,4},shape,true});
        const auto mask=maskCandidate(selected,layer.id,store,true,{})->layer(layer.id).mask;
        check(mask->defaultValue==PackedPixel{},"Rotated inverted mask retains transparent sparse default");
        for(int y=0;y<7;++y) for(int x=0;x<9;++x) {
            // Independent scalar inverse-footprint oracle, including rotated
            // source centers outside the document's finite canvas.
            unsigned count=0;
            for(int sy=0;sy<8;++sy) for(int sx=0;sx<8;++sx) {
                const double dx=8-1.5*(y+(sy+.5)/8),dy=-5+2*(x+(sx+.5)/8);
                const bool canvas=dx>=0 && dx<6 && dy>=0 && dy<6;
                bool membership=dx>=1 && dx<5 && dy>=1 && dy<5;
                if(shape==SelectionShape::Ellipse) membership=membership && std::pow((dx-3)/2,2)+std::pow((dy-3)/2,2)<=1;
                count+=(canvas && !membership)?1u:0u;
            }
            check(mask->pixel(x,y)==Pixel{0,0,0,static_cast<float>(count)/64},"Rotated nonuniform-scale inverted mask exceeds canvas or misses footprint coverage");
        }
    }
}
void fractionalFootprintCoverage() {
    TileStore store(8*1024*1024);
    auto original=document(4,3,Affine{1.25,0,0,.75,.4,.2});const auto id=original->singleLayer().id;
    for(const auto shape:{SelectionShape::Rectangle,SelectionShape::Ellipse}) {
        const auto selected=selectionCandidate(original,Selection{Extent{1,1,2,2},shape,false});
        const auto mask=maskCandidate(selected,id,store,true,{})->layer(id).mask;bool fractional=false;
        for(int y=0;y<3;++y) for(int x=0;x<4;++x) {
            unsigned count=0;
            for(int sy=0;sy<8;++sy) for(int sx=0;sx<8;++sx) {
                const double dx=.4+1.25*(x+(sx+.5)/8),dy=.2+.75*(y+(sy+.5)/8);
                bool membership=dx>=1 && dx<3 && dy>=1 && dy<3;
                if(shape==SelectionShape::Ellipse) membership=membership && (dx-2)*(dx-2)+(dy-2)*(dy-2)<=1;
                count+=membership?1u:0u;
            }
            fractional=fractional || (count>0 && count<64);
            check(mask->pixel(x,y)==Pixel{0,0,0,static_cast<float>(count)/64},"Fractional mask footprint differs from independent subpixel oracle");
        }
        check(fractional,"Boundary footprint produced fractional coverage");
    }
}
void rejectionAndCancellation() {
    TileStore store(8*1024*1024);const auto initial=document(8,8);const auto id=initial->singleLayer().id;
    rejects<std::invalid_argument>([&] {maskCandidate(initial,id,store,true,{});},"No-selection mask command rejected");
    const auto full=maskCandidate(initial,id,store,false,{});
    rejects<std::invalid_argument>([&] {maskCandidate(full,id,store,false,{});},"Existing mask not silently replaced");
    std::stop_source stop;stop.request_stop();rejects<io::SpillError>([&] {maskCandidate(initial,id,store,false,stop.get_token());},"Canceled creation publishes no candidate");
    auto memory=std::make_shared<MemoryAdmission>(65536,0,[] {return MemorySample{16*1024*1024,0};});TileStore tiny(65536,memory);
    const auto selected=selectionCandidate(initial,Selection{Extent{0,0,4,4},SelectionShape::Rectangle,false});
    rejects<std::length_error>([&] {maskCandidate(selected,id,tiny,true,{});},"Scratch memory denial preserves original document");
    check(!selected->layer(id).mask && !memory->snapshot().pendingCpu && !memory->snapshot().committedCpu,"Failed mask creation rolls back all owned charges");
    std::stop_source midStop;
    auto cancelMemory=std::make_shared<MemoryAdmission>(4*1024*1024,0,[&] {
        midStop.request_stop();return MemorySample{16*1024*1024,0};
    });
    TileStore cancelStore(4*1024*1024,cancelMemory);
    rejects<io::SpillError>([&] {maskCandidate(selected,id,cancelStore,true,midStop.get_token());},"Cancellation during admission publishes no mask");
    check(!selected->layer(id).mask && !cancelMemory->snapshot().pendingCpu && !cancelMemory->snapshot().committedCpu,
        "Mid-command cancellation releases bookkeeping and publishes no candidate");
    LayerNode folder{Id::generate()};folder.folder=true;
    auto grouped=std::make_shared<const DocumentSnapshot>(Id::generate(),10,10,72,std::vector<LayerNode>{folder});
    rejects<std::invalid_argument>([&] {maskCandidate(grouped,folder.id,store,false,{});},"Folder mask explicitly rejected");
}
}
int main() {try {defaultsAndHistory();transformedCoverage();sparseBoundariesAndSharing();invertedCanvasClipping();rotatedCanvasClipping();fractionalFootprintCoverage();rejectionAndCancellation();std::cout<<"7 mask edit groups passed\n";return 0;}catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}}
