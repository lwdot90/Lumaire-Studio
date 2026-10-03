#include "core/layer_stack.h"
#include "core/document.h"
#include <iostream>

using namespace compositor::engine;
namespace {
void expect(bool result,const char* message) { if(!result) throw std::runtime_error(message); }
template<class F> void rejects(F f) { bool caught=false;try {f();} catch(const std::exception&) {caught=true;} expect(caught,"Invalid graph or transform accepted"); }
std::shared_ptr<const RasterSnapshot> raster(Pixel pixel,Extent extent={0,0,2,2}) {
    return std::make_shared<const RasterSnapshot>(Id::generate(),extent,pack(pixel));
}
LayerNode layer(std::shared_ptr<const RasterSnapshot> asset,int order=0) {
    LayerNode node{Id::generate()};node.raster=std::move(asset);node.siblingOrder=order;node.sampling=Sampling::Nearest;return node;
}
}
int main() {
    try {
        {
            TileStore store(4*1024*1024);const Extent extent{0,0,1024,256};
            auto first=layer(std::make_shared<const RasterSnapshot>(Id::generate(),extent));
            auto changed=first;TileMap edits{{{1,0},store.constant(256,256,{1,0,0,1})}};
            changed.raster=std::make_shared<const RasterSnapshot>(first.raster->id,extent,PackedPixel{},edits);
            for(auto sampling:{Sampling::Nearest,Sampling::Bilinear,Sampling::Lanczos}) {
                first.sampling=changed.sampling=sampling;
                LayerStack before({first}),after({changed});
                expect(sameRegion(before,after,{10,10,20,20},{4,4}),"Distant pixel edit dirtied unchanged region");
                expect(!sameRegion(before,after,{300,10,310,20},{4,4}),"Pixel edit reused stale region");
                expect(!sameRegion(after,before,{300,10,310,20},{4,4}),"Removed tile reused stale region");
                if(sampling==Sampling::Lanczos)
                    expect(!sameRegion(before,after,{250,10,250,10},{4,4}),"Mip halo omitted from dirty region");
                auto renamed=changed;renamed.name="Metadata only";
                expect(sameRegion(after,LayerStack({renamed}),{0,0,1023,255},{4,4}),"Rename dirtied pixels");
                renamed.opacity=.5;
                expect(!sameRegion(after,LayerStack({renamed}),{300,10,310,20},{4,4}),"Opacity change reused stale region");
                renamed=changed;renamed.visible=false;
                expect(!sameRegion(after,LayerStack({renamed}),{300,10,310,20},{4,4}),"Visibility change reused stale region");
                renamed=changed;renamed.localToDocument.tx=2000;
                expect(!sameRegion(after,LayerStack({renamed}),{300,10,310,20},{4,4}),"Old transformed coverage not invalidated");
                expect(!sameRegion(after,LayerStack({renamed}),{2300,10,2310,20},{4,4}),"New transformed coverage not invalidated");
                expect(sameRegion(before,after,{3000,10,3010,20},{4,4}),"Offscreen edit dirtied empty region");
            }
        }
        {
            auto background=layer(raster({0,0,1,1}));
            LayerNode outer{Id::generate()};outer.folder=true;outer.siblingOrder=1;outer.opacity=.5f;
            LayerNode inner{Id::generate()};inner.folder=true;inner.parent=outer.id;inner.opacity=.5f;
            auto foreground=layer(raster({1,0,0,1}));foreground.parent=inner.id;
            foreground.mask=raster({0,0,0,.25f});
            LayerStack masked({foreground,inner,background,outer});
            expect(masked.evaluate(.5,.5)==Pixel{.0625f,0,.9375f,1},"Nested mask/default coverage applied exactly once");
            foreground.maskEnabled=false;
            expect(LayerStack({foreground,inner,background,outer}).evaluate(.5,.5)==Pixel{.25f,0,.75f,1},"Disabled mask affected output");
            expect(!sameRegion(masked,LayerStack({foreground,inner,background,outer}),{.5,.5,.5,.5},{1,1}),"Mask toggle reused stale pixels");
            foreground.maskEnabled=true;foreground.mask=raster({0,0,0,.5f});
            expect(!sameRegion(masked,LayerStack({foreground,inner,background,outer}),{.5,.5,.5,.5},{1,1}),"Mask default edit reused stale pixels");
            foreground.mask=raster({0,0,0,1},{0,0,1,1});
            rejects([&]{LayerStack invalid({foreground,inner,background,outer});});
            foreground.mask=std::make_shared<const RasterSnapshot>(foreground.raster->id,foreground.raster->extent,pack({0,0,0,1}));
            rejects([&]{LayerStack invalid({foreground,inner,background,outer});});
            foreground.mask=raster({.1f,0,0,1});
            rejects([&]{LayerStack invalid({foreground,inner,background,outer});});
            outer.mask=raster({0,0,0,1});
            rejects([&]{LayerStack invalid({outer});});
        }
        {
            TileStore store(1024*1024);const Extent extent{0,0,512,1};
            auto color=layer(raster({1,0,0,1},extent));color.mask=raster({},extent);
            auto changed=color;TileMap edits{{{1,0},store.constant(256,1,{0,0,0,1})}};
            changed.mask=std::make_shared<const RasterSnapshot>(color.mask->id,extent,PackedPixel{},edits,1);
            const LayerStack before({color}),after({changed});
            expect(sameRegion(before,after,{10,.5,10,.5},{1,1}),"Distant mask edit dirtied unchanged region");
            expect(!sameRegion(before,after,{255.75,.5,255.75,.5},{1,1}),"Bilinear mask halo reused stale region");
            expect(!sameRegion(after,before,{255.75,.5,255.75,.5},{1,1}),"Mask removal omitted bilinear halo");
        }
        {
            auto plain=layer(raster({1,0,0,1}));plain.sampling=Sampling::Bilinear;
            plain.localToDocument={2,0,0,2,3,-2};auto revealed=plain;revealed.mask=raster({0,0,0,1});
            const LayerStack a({plain}),b({revealed});
            for(double y:{-2.25,-2.,-1.5,1.5,2.,2.25}) for(double x:{2.75,3.,3.5,6.5,7.,7.25})
                expect(a.evaluate(x,y)==b.evaluate(x,y),"Reveal-all mask doubled transformed edge fading");
        }
        for(double angle:{0.,35.,90.,180.,-45.,720.}) for(bool flipX:{false,true}) for(bool flipY:{false,true}) {
            const auto transform=Affine::placement(10,20,80,30,8,3,angle,flipX,flipY),inverse=transform.inverse();
            const auto middle=transform.map({4,1.5});expect(std::abs(middle.x-50)<1e-10 && std::abs(middle.y-35)<1e-10,"Rotation and flips preserve source center");
            for(Coordinate point:std::array<Coordinate,3>{{{0,0},{8,3},{2.3,1.1}}}) {
                const auto roundtrip=inverse.map(transform.map(point));
                expect(std::abs(roundtrip.x-point.x)<1e-10 && std::abs(roundtrip.y-point.y)<1e-10,"Affine inverse mapping roundtrip");
            }
        }
        const auto flip=Affine::placement(0,0,2,2,2,2,0,true,false);
        expect(flip.map({0,0}).x==2 && flip.map({2,0}).x==0,"Source-local horizontal flip");
        rejects([]{Affine{1,0,0,0,0,0}.inverse();});
        rejects([]{Affine{1e-20,0,0,1,0,0}.inverse();});
        auto back=layer(raster({0,0,1,1})),front=layer(raster({1,0,0,1}),1);front.opacity=.5f;
        LayerStack stack({front,back}); // Input array need not already be sibling sorted.
        expect(&stack.node(front.id)==&stack.nodes()[0] && &stack.node(back.id)==&stack.nodes()[1],"Layer ID index must use storage order, not render order");
        const auto copiedStack=stack;
        expect(&copiedStack.node(front.id)==&copiedStack.nodes()[0],"Copied layer index refers to original storage");
        rejects([&]{stack.node(Id::generate());});
        expect(stack.evaluate(.5,.5)==Pixel{.5f,0,.5f,1},"Bottom-to-top and opacity exactly once");
        front.localToDocument=Affine::placement(10,0,2,2,2,2);
        LayerStack moved({back,front}); expect(moved.evaluate(.5,.5)==Pixel{0,0,1,1} && moved.evaluate(10.5,.5)==Pixel{.5f,0,0,.5f},"Layer-local pixels are not resampled by placement");
        LayerNode group{Id::generate()};group.folder=true;group.siblingOrder=1;
        front.parent=group.id;front.siblingOrder=0;front.localToDocument={};
        group.localToDocument=Affine::placement(100,100,2,2,2,2);
        LayerStack folder({front,group,back});expect(folder.evaluate(.5,.5)==stack.evaluate(.5,.5),"Pass-through folder does not multiply child transform");
        group.visible=false;LayerStack hidden({front,group,back});expect(hidden.visibleRasterCount()==1 && hidden.evaluate(.5,.5)==Pixel{0,0,1,1},"Inherited visibility");
        group.visible=true;group.opacity=.5f;
        const LayerStack translucent({front,group,back});
        expect(translucent.evaluate(.5,.5)==Pixel{.25f,0,.75f,1},"Folder opacity multiplies child opacity once");
        expect(!sameRegion(folder,translucent,{0,0,1,1},{1,1}),"Inherited opacity invalidates descendant coverage");
        expect(sameRegion(folder,translucent,{10,10,11,11},{1,1}),"Folder opacity does not dirty unrelated regions");
        group.opacity=0;
        expect(LayerStack({front,group,back}).visibleRasterCount()==1,"Zero opacity folder prunes descendants");
        group.opacity=1;group.blend=BlendMode::Multiply;
        rejects([&]{LayerStack invalid({front,group,back});});group.blend=BlendMode::Normal;
        {
            auto lower=layer(raster({0,0,.5f,.5f}));
            auto upper=layer(raster({.5f,0,0,.5f}),1);
            LayerNode outer{Id::generate()};outer.folder=true;outer.opacity=.5f;
            LayerNode inner{Id::generate()};inner.folder=true;inner.opacity=.5f;inner.parent=outer.id;
            lower.parent=upper.parent=inner.id;
            const LayerStack nested({upper,inner,lower,outer});
            expect(nested.prepared().size()==2 && nested.prepared()[0].opacity==.25f && nested.prepared()[1].opacity==.25f,"Nested folder opacity product");
            const auto expected=composite(Pixel{.125f,0,0,.125f},Pixel{0,0,.125f,.125f},BlendMode::Normal);
            expect(nested.evaluate(.5,.5)==expected,"Overlapping translucent children inherit opacity before compositing");
            const auto isolated=composite(Pixel{.125f,0,.0625f,.1875f},{},BlendMode::Normal);
            expect(nested.evaluate(.5,.5)!=isolated,"Pass-through opacity differs from isolated group opacity");
            inner.visible=false;
            expect(LayerStack({upper,inner,lower,outer}).visibleRasterCount()==0,"Nested visibility prunes all descendants");
            inner.visible=true;
            auto document=std::make_shared<const DocumentSnapshot>(Id::generate(),2,2,72,std::vector<LayerNode>{upper,inner,lower,outer});
            DocumentHistory folderHistory(document);
            auto opacityEdit=folderHistory.begin();auto changedNodes=document->layers();changedNodes[3].opacity=.25f;opacityEdit.setLayers(changedNodes);
            expect(folderHistory.commit(opacityEdit,"Folder opacity"),"Folder opacity edit commits");
            const auto changedDocument=folderHistory.current();
            expect(changedDocument->stack.prepared()[0].opacity==.125f,"Committed folder opacity reaches graph");
            folderHistory.undo();expect(folderHistory.current()==document,"Undo restores inherited opacity graph");
            folderHistory.redo();expect(folderHistory.current()==changedDocument,"Redo restores inherited opacity graph");
        }
        group.parent=group.id;rejects([&]{LayerStack invalid({group});});group.parent.reset();
        auto orphan=front;orphan.parent=Id::generate();rejects([&]{LayerStack invalid({orphan});});
        auto duplicate=back;rejects([&]{LayerStack invalid({back,duplicate});});
        auto gap=back;gap.siblingOrder=2;rejects([&]{LayerStack invalid({gap});});
        const auto big=raster({},Extent{0,0,10000,10000});auto one=layer(big),two=layer(big,1);
        expect(LayerStack({one,two}).visibleRasterCount()==2,"Shared asset extent counted once");
        two.raster=raster({},Extent{0,0,1,1});rejects([&]{LayerStack invalid({one,two});});
        // Deep acyclic parents and longer cycles fail before traversal recursion.
        std::vector<LayerNode> deep;
        for(int i=0;i<66;++i) {LayerNode node{Id::generate()};node.folder=true;if(!deep.empty()) node.parent=deep.back().id;deep.push_back(node);}
        rejects([&]{LayerStack invalid(deep);});
        deep.erase(deep.begin()+2,deep.end());deep[0].parent=deep[1].id;rejects([&]{LayerStack invalid(deep);});
        expect(LayerStack({}).evaluate(0,0)==Pixel{},"Empty layer stack is transparent");
        const auto local=moved.region({0,0,2,2}); expect(local.layerCount()==1,"Region excludes translated remote layer");
        expect(local.evaluate(.5,.5)==moved.evaluate(.5,.5),"Culling keeps exact scalar output");
        rejects([&]{local.evaluate(10,.5);}); rejects([&]{local.region({-1,0,1,1});});
        rejects([&]{moved.region({1,0,0,1});});
        rejects([&]{moved.region({0,0,std::numeric_limits<double>::infinity(),1});});
        const auto detached=[] {
            LayerStack temporary({layer(raster({.5f,0,0,.5f}))}); return temporary.region({0,0,2,2});
        }();
        expect(detached.evaluate(.5,.5)==Pixel{.5f,0,0,.5f},"Prepared region owns immutable source lifetime");
        for(const auto sampling:{Sampling::Nearest,Sampling::Bilinear}) {
            std::vector<LayerNode> transformed;
            for(int i=0;i<static_cast<int>(blendModeCount);++i) {
                auto node=layer(raster({.25f,.125f,.5f,.5f},Extent{-1,-2,4,5}),i);
                node.sampling=sampling; node.blend=static_cast<BlendMode>(i); node.opacity=.75f;
                node.localToDocument=Affine::placement(i*3-10,i%3-2,4+i,6,4,5,i*17,i%2==0,i%3==0);
                transformed.push_back(node);
            }
            const LayerStack varied(transformed); const auto whole=varied.region({-30,-30,60,60});
            for(int y=-30;y<60;y+=3) for(int x=-30;x<60;x+=3) {
                const auto part=whole.region({double(x),double(y),x+3.,y+3.});
                for(double dx:{0.,.125,1.5,3.}) for(double dy:{0.,.125,1.5,3.})
                    expect(part.evaluate(x+dx,y+dy)==varied.evaluate(x+dx,y+dy),"Region/reference agreement across transforms and all blend modes");
            }
            auto edge=layer(raster({1,0,0,1},Extent{-256,-256,257,257})); edge.sampling=sampling;
            LayerStack edges({edge});
            for(double x:{-256.50000001,-256.5,-256.49999999,-256.,.99999999,1.,1.49999999,1.5,1.50000001}) {
                const auto point=edges.region({x,0,x,0});
                expect(point.evaluate(x,0)==edges.evaluate(x,0),"Sampling support and negative-grid boundaries remain conservative");
            }
        }
        // Required source coverage contains every CPU sampling tap without
        // reading or flattening raster pixels, including negative edge tiles.
        TileStore coverageStore(4*1024*1024);TileMap coverageTiles;
        const Extent coverageExtent{-259,-3,520,260};
        for(std::int64_t y=-1;y<=1;++y) for(std::int64_t x=-2;x<=1;++x) {
            const auto extent=tileExtent(coverageExtent,{x,y});
            coverageTiles[{x,y}]=coverageStore.constant(static_cast<int>(extent.width),static_cast<int>(extent.height),{1,0,0,1});
        }
        RasterSnapshot coverageRaster(Id::generate(),coverageExtent,{},coverageTiles);
        for(auto sampling:{Sampling::Nearest,Sampling::Bilinear})
            for(const auto inverse:std::array<Affine,3>{{{}, {.8,.6,-.6,.8,-12,3}, {-1,0,0,.5,7,-9}}}) {
                const SampleRegion bounds{-260,-5,260,258};
                const auto required=requiredSourceTiles(coverageRaster,inverse,sampling,bounds);
                for(double y=-5;y<=258;y+=3.125) for(double x=-260;x<=260;x+=2.75) {
                    auto point=inverse.map({x,y});if(sampling==Sampling::Bilinear){point.x-=.5;point.y-=.5;}
                    const auto ix=static_cast<std::int64_t>(std::floor(point.x)),iy=static_cast<std::int64_t>(std::floor(point.y));
                    const int taps=sampling==Sampling::Bilinear ? 2 : 1;
                    for(int dy=0;dy<taps;++dy) for(int dx=0;dx<taps;++dx) if(coverageExtent.contains(ix+dx,iy+dy))
                        expect(required.contains({floorTile(ix+dx),floorTile(iy+dy)}),"Coverage excluded a required source tap");
                }
            }
        expect(requiredSourceTiles(coverageRaster,{},Sampling::Bilinear,{20,20,21,21}).size()==1,"Small region uploads only local tile");
        expect(requiredSourceTiles(coverageRaster,{},Sampling::Bilinear,{1000,1000,1001,1001}).empty(),"Exterior region uploads no tiles");
        // EditorSession/DocumentHistory adaptation: layer metadata shares
        // pixels, and editing a duplicate forks only its immutable asset.
        TileStore tiles(4*1024*1024);
        auto initial=blankDocument(2,2); DocumentHistory history(initial);
        auto paint=history.begin(); paint.replace({0,0},tiles.constant(2,2,{1,0,0,1})); history.commit(paint,"Paint");
        const auto painted=history.current(); auto nodes=painted->layers();
        auto copy=nodes[0]; copy.id=Id::generate(); copy.name="Copy"; copy.siblingOrder=1; nodes.push_back(copy);
        auto edit=history.begin(); edit.setLayers(nodes); expect(history.commit(edit,"Duplicate"),"Duplicate commits");
        const auto duplicated=history.current(); expect(duplicated->layers()[0].raster==duplicated->layers()[1].raster,"Duplicate shares source asset");
        expect(history.retainedBytes()==0,"Live shared tiles excluded from history budget");
        paint=history.begin(copy.id); paint.replace({0,0},tiles.constant(2,2,{0,1,0,1})); history.commit(paint,"Paint copy");
        const auto independent=history.current();
        expect(independent->layers()[0].raster==painted->layers()[0].raster,"Editing duplicate leaves original immutable");
        expect(independent->layers()[0].raster->id!=independent->layers()[1].raster->id,"Duplicate pixel edit forks asset ID");
        expect(independent->stack.evaluate(.5,.5)==Pixel{0,1,0,1},"Document stack evaluates edited top layer");
        history.undo(); expect(history.current()==duplicated,"Undo restores exact graph pointer");
        edit=history.begin(); edit.setLayers(duplicated->layers()); expect(!history.commit(edit,"No-op") && history.canRedo(),"Metadata no-op preserves redo");
        history.redo(); expect(history.current()==independent,"Redo restores exact graph pointer");
        edit=history.begin(); nodes=independent->layers(); nodes[0].parent=copy.id;
        rejects([&]{edit.setLayers(nodes);}); rejects([&]{history.commit(edit,"Invalid graph");});
        expect(history.current()==independent,"Poisoned graph edit never publishes");
        edit=history.begin(); edit.setLayers({}); history.commit(edit,"Delete all");
        expect(history.current()->layers().empty() && history.retainedBytes()>0,"Empty document keeps history-only tiles budgeted");
        history.undo(); expect(history.current()==independent,"Undo deleting all layers");
        rejects([&]{EditTransaction invalid(independent,Id::generate());});
        edit=history.begin(); rejects([&]{edit.replace({0,0},tiles.constant(2,2,{}));});
        std::cout<<"affine placement, immutable layer stacks, hierarchy and aggregate limits passed\n";
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
