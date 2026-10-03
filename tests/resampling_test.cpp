#include "core/resampling.h"
#include "core/raster.h"
#include "rendering/mip_cache.h"
#include <iostream>

using namespace compositor::engine;
namespace {
void expect(bool value,const char* message) {if(!value) throw std::runtime_error(message);}
template<class F> void rejects(F f) {bool caught=false;try{f();}catch(const std::exception&){caught=true;}expect(caught,"Invalid filter input accepted");}
void near(Pixel a,Pixel b,float tolerance=3e-5f) {
    expect(std::abs(a.r-b.r)<tolerance && std::abs(a.g-b.g)<tolerance &&
           std::abs(a.b-b.b)<tolerance && std::abs(a.a-b.a)<tolerance,"Lanczos reference mismatch");
}
// Independent long-double direct convolution: no production tap builder,
// normalization, accumulation or alpha finalizer is used here.
Pixel reference(const PixelReader& read,double x,double y,double fx,double fy) {
    auto axis=[](double center,double footprint) {
        std::vector<std::pair<std::int64_t,long double>> taps;
        long double sum=0;
        for(auto i=static_cast<std::int64_t>(std::floor(center-3*footprint-1));
            i<=static_cast<std::int64_t>(std::ceil(center+3*footprint));++i) {
            const long double t=(static_cast<long double>(i)+.5L-center)/footprint;
            long double w=0;
            if(std::abs(t)<3) {
                if(t==0) w=1;
                else {const auto p=std::numbers::pi_v<long double>*t;w=(std::sin(p)/p)*(std::sin(p/3)/(p/3));}
            }
            w/=footprint;taps.emplace_back(i,w);sum+=w;
        }
        for(auto& tap:taps) tap.second/=sum;
        return taps;
    };
    const auto xs=axis(x,fx),ys=axis(y,fy);std::array<long double,4> value{};
    for(const auto& [iy,wy]:ys) for(const auto& [ix,wx]:xs) {
        const auto p=read(ix,iy);const std::array<float,4> channels{p.r,p.g,p.b,p.a};
        for(std::size_t c=0;c<4;++c) value[c]+=channels[c]*wx*wy;
    }
    if(value[3]<=0) return {};
    const auto alpha=std::min(1.L,value[3]),ratio=alpha/value[3];
    return {static_cast<float>(value[0]*ratio),static_cast<float>(value[1]*ratio),static_cast<float>(value[2]*ratio),static_cast<float>(alpha)};
}
}
int main() {
    try {
        expect(lanczos3(0)==1 && lanczos3(1)==0 && lanczos3(-2)==0 && lanczos3(3)==0,"Lanczos cardinal values");
        for(double center:{-255.875,-.5,0.,.5,1.,255.25,256.5}) for(double width:{1.,1.125,2.,3.7,16.}) {
            const auto taps=lanczosAxis(center,width);float sum=0;
            for(std::size_t i=0;i<taps.size();++i) {
                sum+=taps[i].weight;
                if(i) expect(taps[i].index==taps[i-1].index+1,"Tap order/gap changed");
            }
            expect(std::abs(sum-1)<2e-6f,"Lanczos weights not normalized");
        }
        const PixelReader texture=[](std::int64_t x,std::int64_t y) {
            if(x<-20 || x>=300 || y<-10 || y>=280) return Pixel{};
            const float a=((x+y)%3==0) ? .25f : .875f;
            return Pixel{a*float((x%7)-2)/3,a*float(y%5)/2,-a*.5f,a};
        };
        std::size_t compared=0;
        for(double x:{-22.,-20.1,-.75,.5,127.625,255.75,256.5,299.9,304.})
            for(double y:{-12.,-9.875,.5,255.5,279.5,283.})
                for(auto footprint:std::array<Coordinate,5>{{{1,1},{2,2},{3.5,3.5},{1,8},{4,1.5}}}) {
                    near(sampleLanczos(texture,x,y,footprint.x,footprint.y),reference(texture,x,y,footprint.x,footprint.y));++compared;
                }
        near(sampleLanczos(texture,256.5,12.5,1,1),texture(256,12),1e-7f);
        expect(finishFiltered({-1,2,3,-.1f})==Pixel{},"Negative alpha must canonicalize transparent");
        expect(finishFiltered({-2,4,8,2})==Pixel{-1,2,4,1},"Alpha clamp must preserve extended straight RGB");
        const auto zero=finishFiltered({-0.f,0,-0.f,.5f});expect(!std::signbit(zero.r) && !std::signbit(zero.b),"Filtered negative zero persisted");
        const PixelReader constant=[](auto,auto){return Pixel{.25f,.5f,1,1};};
        const auto edge=halveRegion(constant,1,1,{0,0,1,1});
        expect(edge && edge->pixels[0].a>0 && edge->pixels[0].a<.5f,"Exterior tap normalization made edge opaque");
        const PixelReader onePixel=[&](auto x,auto y){return x==0 && y==0 ? constant(x,y) : Pixel{};};
        near(edge->pixels[0],reference(onePixel,1,1,2,2));

        // A nonuniform signed source crosses native tile boundaries. The mip
        // uses raster-local coordinates while the reader supplies its origin.
        TileStore store(16*1024*1024);const Extent sourceExtent{-7,-3,517,259};TileMap tiles;
        for(auto ty=floorTile(sourceExtent.y);ty<=floorTile(sourceExtent.y+sourceExtent.height-1);++ty)
            for(auto tx=floorTile(sourceExtent.x);tx<=floorTile(sourceExtent.x+sourceExtent.width-1);++tx) {
                const auto area=tileExtent(sourceExtent,{tx,ty});std::vector<Pixel> pixels;
                for(auto y=area.y;y<area.y+area.height;++y) for(auto x=area.x;x<area.x+area.width;++x) pixels.push_back(texture(x,y));
                tiles[{tx,ty}]=store.create(static_cast<int>(area.width),static_cast<int>(area.height),pixels);
            }
        RasterSnapshot source(Id::generate(),sourceExtent,{},tiles);
        const PixelReader reader=[&](auto x,auto y){return source.pixel(x+sourceExtent.x,y+sourceExtent.y);};
        // Reference window straddles output tile x=256. Repartitioning into
        // one-pixel columns must produce bit-identical FP32 intermediates.
        const Extent output{250,0,9,130};const auto whole=halveRegion(reader,517,259,output);expect(bool(whole),"Whole mip region failed");
        for(auto x=output.x;x<output.x+output.width;++x) {
            const auto piece=halveRegion(reader,517,259,{x,0,1,130});expect(bool(piece),"Mip piece failed");
            for(std::int64_t y=0;y<130;++y) {
                const auto actual=piece->pixels[static_cast<std::size_t>(y)];
                expect(actual==whole->pixels[static_cast<std::size_t>(y*9+x-250)],"Mip subdivision introduced a seam");
                near(actual,reference(reader,double(2*x+1),double(2*y+1),2,2));++compared;
            }
        }
        // Odd dimensions, ceil-halving and recursive grids: no half rounding
        // between levels, and parent exterior is transparent at every level.
        const auto first=halveRegion(texture,31,17,{0,0,16,9});
        const PixelReader previous=[&](auto x,auto y){return x>=0 && y>=0 && x<16 && y<9 ? first->pixels[static_cast<std::size_t>(y*16+x)] : Pixel{};};
        const auto second=halveRegion(previous,16,9,{0,0,8,5});
        for(int y=0;y<5;++y) for(int x=0;x<8;++x) near(second->pixels[static_cast<std::size_t>(y*8+x)],reference(previous,2*x+1,2*y+1,2,2));
        auto small=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{-13,7,67,35},pack({.25f,.5f,-.25f,.5f}));
        const PixelReader smallReader=[&](auto x,auto y){return small->pixel(x-13,y+7);};
        const auto smallFirst=halveRegion(smallReader,67,35,{0,0,34,18});
        const PixelReader smallPrevious=[&](auto x,auto y){return x>=0 && y>=0 && x<34 && y<18 ? smallFirst->pixels[static_cast<std::size_t>(y*34+x)] : Pixel{};};
        const auto smallSecond=halveRegion(smallPrevious,34,18,{0,0,17,9});
        for(std::size_t slots:{1u,4u,16u}) {
            compositor::MipCache cache(slots*compositor::MipCache::slotBytes);
            for(int y=0;y<9;++y) for(int x=0;x<17;++x) {
                expect(cache.pixel(small,2,x,y)==smallSecond->pixels[static_cast<std::size_t>(y*17+x)],"Mip LRU pressure changed FP32 pixels");
                expect(cache.residentBytes()<=slots*compositor::MipCache::slotBytes,"Mip payload exceeded budget");
            }
            const auto built=cache.builtPieces();cache.pixel(small,2,0,0);
            expect(cache.builtPieces()==built,"Mip cache failed to reuse immutable source");
            // Deliberately equal ID/revision: pointer identity is authoritative,
            // just as in the original DownsampleCache, not local version IDs.
            auto replacement=std::make_shared<const RasterSnapshot>(small->id,small->extent,pack({0,0,0,0}),TileMap{},small->revision);
            expect(cache.pixel(replacement,2,0,0)==Pixel{},"Mip cache aliased unrelated source pixels");
            expect(!cache.pixel(small,2,0,0,[]{return true;}),"Cancelled mip cache returned a result");
            cache.clear();expect(cache.residentPieces()==0 && cache.residentBytes()==0,"Mip clear retained entries");
            int polls=0;expect(!cache.pixel(small,2,0,0,[&]{return ++polls>20;}),"Recursive cancellation published a partial mip");
            expect(cache.pixel(small,2,8,4)==smallSecond->pixels[4*17+8],"Cancelled mip poisoned later cache result");
        }
        int reads=0;const PixelReader counted=[&](auto x,auto y){++reads;return texture(x,y);};
        expect(!halveRegion(counted,31,17,{0,0,16,9},[]{return true;}) && reads==0,"Cancelled mip read source pixels");
        expect(!halveRegion(counted,31,17,{0,0,16,9},[&]{return reads>100;}),"Partial cancelled mip was published");

        for(double angle:{0.,30.,90.,137.}) {
            const auto placement=Affine::placement(0,0,64,64,512,512,angle);
            const auto plan=reductionPlan(placement.inverse(),{1,1},512,512);
            expect(!plan.anisotropic && plan.level==3,"Rotated isotropic footprint chose wrong mip");
            expect(std::abs(plan.residualFootprint.x-1)<1e-12,"Isotropic mip residual changed scale");
        }
        const auto asymmetric=reductionPlan({8,0,0,2,0,0},{1,1},512,512);
        expect(asymmetric.anisotropic && asymmetric.level==0 && asymmetric.residualFootprint.x==8 && asymmetric.residualFootprint.y==2,"Anisotropy incorrectly used isotropic mip");
        const auto shear=reductionPlan({2,1,1,2,0,0},{1,1},512,512);
        expect(shear.anisotropic && shear.level==0,"Equal-length sheared rows incorrectly used mips");
        expect(reductionPlan({8,0,0,8,0,0},{1,1},512,512,true).level==0,"Nearest selected a mip");
        expect(reductionPlan({1024,0,0,1024,0,0},{1,1},3,3).level==2,"Mip chain exceeded 1x1");
        rejects([]{lanczosAxis(0,.5);});rejects([]{lanczosAxis(0,1e10);});rejects([]{lanczosAxis(1e300,1);});
        rejects([]{lanczosAxis(std::numeric_limits<double>::quiet_NaN(),1);});
        rejects([]{finishFiltered({0,0,0,std::numeric_limits<float>::infinity()});});
        rejects([&]{halveRegion(reader,517,259,{0,0,257,1});});rejects([&]{halveRegion(reader,517,259,{258,129,2,1});});
        rejects([&]{halveRegion(reader,517,259,{-1,0,1,1});});rejects([]{reductionPlan({}, {0,1},32,32);});
        rejects([]{compositor::MipCache invalid(0);});
        compositor::MipCache lifetime(compositor::MipCache::slotBytes);
        std::weak_ptr<const RasterSnapshot> weak;
        {
            auto temporary=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,2,2},pack({1,0,0,1}));weak=temporary;
            lifetime.pixel(temporary,1,0,0);
            rejects([&]{lifetime.pixel(temporary,2,0,0);});
        }
        expect(!weak.expired(),"Cached mip source identity lost ownership");
        lifetime.clear();expect(weak.expired(),"Cleared mip cache retained source ownership");
        std::cout<<compared<<" independent Lanczos comparisons; stable mip pieces, odd edges, cancellation, LRU pressure and affine footprints passed\n";
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
