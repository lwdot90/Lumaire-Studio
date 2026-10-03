#include "rendering/cpu_image.h"
#include "rendering/layer_sampler.h"
#include <QCoreApplication>
#include <iostream>
#include <chrono>

using namespace compositor;
using namespace compositor::engine;
namespace {
void expect(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
template<class F> void rejects(F f) { bool caught=false; try {f();} catch(const std::exception&) {caught=true;} expect(caught,"Invalid render input accepted"); }
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        if(app.arguments().contains("--fallback-probe")) {
            // Informational CPU-only counterpart to the two-layer lifecycle
            // fallback. No presentation, no hardware performance-gate claim.
            TileStore store(4*1024*1024);std::vector<LayerNode> nodes;
            for(int layer=0;layer<2;++layer) {
                TileMap map;
                for(int y=0;y<4;++y) for(int x=0;x<4;++x)
                    map[{x,y}]=store.constant(256,256,layer ? Pixel{0,0,.5f,.5f} : Pixel{.75f,0,0,.75f});
                auto source=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,1024,1024},PackedPixel{},map);
                nodes.push_back({Id::generate(),{},layer,false,true,layer ? .625f : 1.f,
                    layer ? BlendMode::Screen : BlendMode::Normal,{},Sampling::Bilinear,source});
            }
            DocumentSnapshot document(Id::generate(),1024,1024,96,std::move(nodes));
            const Viewport view{677,461,2,1,{512,512},false,1024,1024};
            const auto start=std::chrono::steady_clock::now();
            const auto image=renderCpuImage(document,view,1354,922);
            expect(!image.isNull(),"Fallback probe did not render");
            const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
            std::cout<<"CPU-only bilinear fallback probe: 1354x922, two 1024x1024 uniform-tiled layers, elapsed_ms="<<elapsed<<'\n';
            const auto referenceStart=std::chrono::steady_clock::now();
            const auto reference=renderCpuImage(document,view,1354,922,{},CpuRenderMode::Reference);
            const auto referenceMs=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-referenceStart).count();
            expect(image==reference,"Fallback lookup cache changed display pixels");
            std::cout<<"Scalar culling reference elapsed_ms="<<referenceMs<<"; output byte-identical (single observations, not benchmark qualification)\n";
            return 0;
        }
        TileStore tiles(4*1024*1024);
        {
            LayerNode masked{Id::generate()};masked.sampling=Sampling::Nearest;
            masked.raster=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,2,1},pack({1,0,0,1}));
            TileMap masks;std::array<Pixel,2> coverage{{{0,0,0,0},{0,0,0,1}}};
            masks.emplace(TileCoord{0,0},tiles.create(2,1,coverage));
            masked.mask=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,2,1},PackedPixel{},masks);
            LayerStack maskStack({masked});MipCache maskCache(2*1024*1024);
            CpuLayerSampler sampler(maskStack,{1,1},maskCache);const auto candidates=sampler.all();
            expect(sampler.evaluate(candidates,1,.5)==Pixel{.5f,0,0,.5f},"CPU mask did not interpolate alpha bilinearly");
            for(double x:{.25,.5,.75,1.,1.25,1.5,1.75})
                expect(sampler.evaluate(candidates,x,.5)==maskStack.evaluate(x,.5),"CPU mask and independent scalar output differ");
            std::stop_source stop;stop.request_stop();
            expect(!sampler.evaluate(candidates,1,.5,{},stop.get_token()),"Cancelled masked sample published pixels");
        }

        {
            LayerNode plain{Id::generate()};plain.sampling=Sampling::Bilinear;
            plain.raster=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,2,2},pack({1,0,0,1}));
            plain.localToDocument={2,0,0,2,3,-2};auto masked=plain;
            masked.mask=std::make_shared<const RasterSnapshot>(Id::generate(),plain.raster->extent,pack({0,0,0,1}));
            LayerStack a({plain}),b({masked});MipCache cache(2*1024*1024);
            CpuLayerSampler sampler(b,{1,1},cache);const auto indices=sampler.all();
            for(double y:{-2.25,-2.,-1.5,1.5,2.,2.25}) for(double x:{2.75,3.,3.5,6.5,7.,7.25})
                expect(sampler.evaluate(indices,x,y)==a.evaluate(x,y),"CPU reveal-all mask changed transformed edges");
        }
        // Sparse grid crosses negative and positive 256-tile boundaries. Each
        // block must sample neighboring source tiles, not clipped tile images.
        auto source=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{-1,-1,258,258},pack({.1f,.2f,.3f,.5f}));
        TileMap map;
        map.emplace(TileCoord{0,0},tiles.constant(256,256,{.5f,0,0,.5f}));
        map.emplace(TileCoord{1,1},tiles.constant(1,1,{0,1,0,1}));
        source=std::make_shared<const RasterSnapshot>(source->id,source->extent,source->defaultValue,map);
        RasterReader reader(*source);
        for(int y=-2;y<=258;++y) for(int x=-2;x<=258;++x)
            expect(reader.pixel(x,y)==source->pixel(x,y),"Cached raster lookup changed sparse/boundary pixels");
        for(auto mode:{Sampling::Nearest,Sampling::Bilinear,Sampling::Lanczos})
            for(double y:{-1.75,-.25,0.5,255.75,256.25,257.5})
                for(double x:{-1.75,-.25,0.5,255.75,256.25,257.5})
                    expect(sampleFrom([&](auto px,auto py){return reader.pixel(px,py);},x,y,mode)==sample(*source,x,y,mode),
                        "Cached reader changed interpolation arithmetic");
        RasterReader interior(*source);
        for(int y=10;y<74;++y) for(int x=10;x<74;++x)
            expect(sampleFrom([&](auto px,auto py){return interior.pixel(px,py);},x+.25,y+.75,Sampling::Bilinear)==sample(*source,x+.25,y+.75,Sampling::Bilinear),
                "Cached interior differs from scalar sampling");
        expect(interior.lookups()==1,"Interior bilinear block repeated tile-map lookups");
        expect(interior.leased(),"Active reader did not retain its resident lease");
        interior.release();
        expect(!interior.leased(),"Reader failed to release its resident lease");
        expect(interior.pixel(10,10)==source->pixel(10,10) && interior.lookups()==1,
            "Released reader failed to reacquire its tile without another lookup");
        bool readCancelled=false;
        try {interior.pixel(10,10,[]{return true;});}
        catch(const RasterReadCancelled&) {readCancelled=true;}
        expect(readCancelled && !interior.leased(),"Cancelled reader retained its lease");
        std::stop_source readStop;readStop.request_stop();
        readCancelled=false;
        try {interior.pixel(10,10,{},readStop.get_token());}
        catch(const RasterReadCancelled&) {readCancelled=true;}
        expect(readCancelled && !interior.leased(),"Stopped reader retained its lease");
        std::vector<Pixel> densePixels(7*9);
        for(std::size_t i=0;i<densePixels.size();++i) densePixels[i]={float(i%7)/8,float(i%9)/16,-.125f,.875f};
        auto denseTile=tiles.create(7,9,densePixels);
        for(const auto origin:{std::numeric_limits<std::int64_t>::min(),std::int64_t(-255),std::numeric_limits<std::int64_t>::max()-254}) {
            const Extent extent{origin,origin,7,9};
            RasterSnapshot dense(Id::generate(),extent,{},{{{floorTile(origin),floorTile(origin)},denseTile}});
            RasterReader cached(dense);
            for(int y=0;y<9;++y) for(int x=0;x<7;++x)
                expect(cached.pixel(origin+x,origin+y)==dense.pixel(origin+x,origin+y),"Cached dense signed-origin pixels changed");
            expect(cached.lookups()==1,"Dense tile lookup not retained");
        }
        std::vector<LayerNode> nodes;
        for(std::size_t i=0;i<blendModeCount;++i) {
            LayerNode node{Id::generate()}; node.raster=source; node.siblingOrder=static_cast<int>(i); node.blend=static_cast<BlendMode>(i);
            node.opacity=.125f; node.sampling=i%2 ? Sampling::Bilinear : Sampling::Nearest;
            node.localToDocument=Affine::placement(static_cast<double>(i)*19-80,static_cast<double>(i)*13-50,70+static_cast<double>(i),100-static_cast<double>(i),258,258,static_cast<double>(i)*23,i%2==0,i%3==0);
            nodes.push_back(std::move(node));
        }
        DocumentSnapshot document(Id::generate(),300,300,72,nodes);
        for(double zoom:{.25,1.,2.3}) for(double dpr:{1.,1.5,2.}) {
            Viewport view{137/dpr,131/dpr,dpr,zoom,{120.25,110.75},false,300,300};
            const auto reference=renderCpuImage(document,view,137,131,{},CpuRenderMode::Reference);
            const auto planned=renderCpuImage(document,view,137,131);
            expect(reference==planned,"Viewport blocks must be byte-identical to uncropped reference at fractional DPR/zoom/pan");
        }
        // Production high-quality renderer: isotropic mips and anisotropic
        // direct support must survive both viewport and block culling.
        auto filteredNodes=nodes;
        filteredNodes.erase(filteredNodes.begin()+3,filteredNodes.end());
        for(std::size_t i=0;i<filteredNodes.size();++i) {
            auto& node=filteredNodes[i];node.sampling=Sampling::Lanczos;
            node.localToDocument=Affine::placement(-3+double(i)*11,-5,64,i==1 ? 96 : 64,258,258,double(i)*37);
        }
        DocumentSnapshot filtered(Id::generate(),100,100,72,filteredNodes);
        MipCache mipCache(4*1024*1024);
        for(double zoom:{.5,1.5}) for(double dpr:{1.,1.25}) {
            Viewport highView{31/dpr,27/dpr,dpr,zoom,{30.125,27.75},false,100,100};
            const auto reference=renderCpuImage(filtered,highView,31,27,{},CpuRenderMode::Reference,nullptr,&mipCache);
            const auto planned=renderCpuImage(filtered,highView,31,27,{},CpuRenderMode::Regions,nullptr,&mipCache);
            expect(reference==planned,"High-quality culling changed filtered pixels");
            const auto built=mipCache.builtPieces();
            expect(renderCpuImage(filtered,highView,31,27,{},CpuRenderMode::Regions,nullptr,&mipCache)==planned,"Reused mip changed preview");
            expect(mipCache.builtPieces()==built,"Unchanged render rebuilt resident mips");
        }
        LayerNode halo{Id::generate()};halo.raster=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,8,8},pack({1,0,0,1}));
        halo.sampling=Sampling::Lanczos;halo.localToDocument={.25,0,0,.25,10,10};
        LayerStack haloStack({halo});CpuLayerSampler haloSampler(haloStack,{1,1},mipCache);
        const auto all=haloSampler.all();bool foundExterior=false;
        for(double y=8;y<14;y+=.375) for(double x=8;x<14;x+=.375) {
            const auto selected=haloSampler.region({x,y,x,y});
            const auto value=haloSampler.evaluate(all,x,y);
            expect(value==haloSampler.evaluate(selected,x,y),"Filter halo was culled at raster boundary");
            if((x<10 || x>=12 || y<10 || y>=12) && value->a>0) foundExterior=true;
        }
        expect(foundExterior,"Halo fixture failed to exercise nonzero exterior filter support");
        int filterChecks=0;
        Viewport cancelView{31,27,1,.5,{30,27},false,100,100};
        expect(renderCpuImage(filtered,cancelView,31,27,[&]{return ++filterChecks>200;}).isNull(),"High-quality cancellation published partial pixels");
        // Deterministic work counts, not wall-clock benchmark claims: 256
        // disjoint layers, just one in view. No pixel work for the other 255.
        nodes.clear();
        const auto small=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,16,16},pack({1,0,0,1}));
        for(int i=0;i<256;++i) {
            LayerNode node{Id::generate()}; node.raster=small; node.siblingOrder=i;
            node.localToDocument.tx=i*64; nodes.push_back(std::move(node));
        }
        DocumentSnapshot distant(Id::generate(),16384,128,72,nodes);
        Viewport view{63,63,1,1,{31.5,31.5},false,16384,128};
        CpuRenderStats referenceStats,regionStats;
        const auto reference=renderCpuImage(distant,view,63,63,{},CpuRenderMode::Reference,&referenceStats);
        const auto planned=renderCpuImage(distant,view,63,63,{},CpuRenderMode::Regions,&regionStats);
        expect(planned==reference && regionStats.viewportLayers==1,"Offscreen layers rejected without changing pixels");
        expect(regionStats.layerSamples*256==referenceStats.layerSamples,"Distant-layer fixture reduces layer samples 256-fold");
        view={255,64,1,1,{127.5,32},false,16384,128};
        renderCpuImage(distant,view,255,64,{},CpuRenderMode::Regions,&regionStats);
        expect(regionStats.viewportLayers==4 && regionStats.layerSamples<255*64*4,"Screen-block planning further prunes viewport candidates");
        std::stop_source frameStop;frameStop.request_stop();
        expect(renderCpuImage(document,view,63,63,{},CpuRenderMode::Regions,nullptr,nullptr,frameStop.get_token()).isNull(),
            "Stopped frame published pixels");
        int checks=0;
        const auto cancelled=renderCpuImage(document,view,256,64,[&]{return ++checks>3;},CpuRenderMode::Regions,&regionStats);
        expect(cancelled.isNull() && regionStats.layerSamples==0,"Cancellation never publishes a partial frame or counters");
        view.zoom=0; rejects([&]{renderCpuImage(document,view,64,64);});
        view.zoom=1; view.center.x=std::numeric_limits<double>::quiet_NaN(); rejects([&]{renderCpuImage(document,view,64,64);});
        std::cout<<"region/reference pixels exact; distant-layer samples "<<referenceStats.layerSamples/256<<" versus "<<referenceStats.layerSamples<<"; cancellation and geometry passed\n";
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
