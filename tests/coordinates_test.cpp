#include "core/checkerboard.h"
#include <cstdlib>
#include <iostream>
#include <limits>
using namespace compositor;
void expect(bool pass,const char* message) { if(!pass) { std::cerr<<message<<'\n'; std::exit(1); } }
int main() {
    for(double dpr:{1.0,1.5,2.0}) for(double zoom:{.01,.5,1.0,64.0}) {
        Viewport v{800,600,dpr,zoom,{31,-7}};
        Point p{-12.25,678.5}; const auto logical=v.toLogical(p), roundtrip=v.toDocument(logical), device=v.toDevice(p);
        expect(std::abs(roundtrip.x-p.x)<1e-8 && std::abs(roundtrip.y-p.y)<1e-8,"coordinate round trip");
        expect(device.x==logical.x*dpr,"device scale");
        for(int x:{0,1,255,256,799}) for(int y:{0,1,255,599}) {
            const auto grid=v.documentPixel(x,y),mapped=v.toDocument({(x+.5)/dpr,(y+.5)/dpr});
            expect(std::abs(grid.x-mapped.x)<1e-8 && std::abs(grid.y-mapped.y)<1e-8,"shared CPU/GPU pixel grid preserves viewport geometry");
            const auto origin=v.pixelOrigin();const double step=1/v.zoom;
            expect(grid.x==origin.x+x*step && grid.y==origin.y+y*step,"pixel grid remains absolute across subdivisions");
        }
        const auto before=v.toDocument({73,81}); v.zoomAt({73,81},1.5); const auto after=v.toDocument({73,81});
        expect(std::abs(before.x-after.x)<1e-8 && std::abs(before.y-after.y)<1e-8,"cursor anchor");
        auto rects=checkerboard(v,800,600,{},false);
        for(auto r:rects) expect(r.x>=0 && r.y>=0 && r.width>0 && r.height>0 && r.x+r.width<=800 && r.y+r.height<=600,"clipped checker bounds");
    }
    Viewport v; v.zoomAt({},std::numeric_limits<double>::infinity()); expect(v.zoom==1,"invalid zoom ignored");
    for(double dpr:{1.0,1.5,2.0}) {
        Viewport actual{800,600,dpr,1,{512,512},false};
        const auto a=actual.toDevice({100,100}), b=actual.toDevice({101,101});
        expect(std::abs(b.x-a.x-1)<1e-8 && std::abs(b.y-a.y-1)<1e-8,"100% is one display pixel per document pixel");
        actual.pan({60,30}); const auto center=actual.center;
        actual.resize(900,700,dpr+1);
        expect(actual.center.x==center.x && actual.center.y==center.y && actual.zoom==1,"manual viewport preserves center and zoom across DPI resize");
        actual.fit(); expect(actual.followsFit,"fit mode enabled");
        actual.resize(800,600,dpr);
        expect(std::abs(actual.zoom-504*dpr/1024)<1e-8,"original 96-point fit margin follows resize");
        actual.zoomAt({90,75},1.2); expect(!actual.followsFit,"anchored zoom leaves fit mode");
    }
    expect(monotonicNs()>0,"monotonic clock");
    std::cout<<"coordinate, DPI, anchor and checker clipping tests passed\n";
}
