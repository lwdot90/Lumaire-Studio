#include "core/histogram.h"
#include <iostream>
#include <numeric>
#include <stdexcept>

using namespace compositor::engine;
namespace {
void expect(bool value,const char* message) {if(!value) throw std::runtime_error(message);}
template<class F> void rejects(F action) {bool failed=false;try {action();} catch(const std::exception&) {failed=true;}expect(failed,"Histogram should reject invalid input or cancellation");}
void binsAndAlpha() {
    TileStore store(1024*1024);
    const std::array<Pixel,4> pixels{Pixel{1,0,0,1},Pixel{0,.5f,0,.5f},Pixel{-1,2,.5f,.5f},Pixel{}};
    TileMap tiles;tiles.emplace(TileCoord{0,0},store.create(4,1,pixels));
    RasterSnapshot raster(Id::generate(),{0,0,4,1},{},std::move(tiles));
    const auto bytes=store.usedBytes();const auto h=histogramLayer(raster);
    expect(h.samples==3 && !h.sampled,"Transparent pixels must not count");
    expect(h.red[255]==1 && h.red[0]==2 && h.green[255]==2 && h.green[0]==1 && h.blue[255]==1 && h.blue[0]==2,"Straight alpha or extended-channel display bins differ");
    expect(h.luma[54]==1 && h.luma[182]==1 && h.luma[201]==1,"Independent primary/cyan encoded luminance bins differ");
    expect(store.usedBytes()==bytes,"Histogram changed tile backing");
}
void sparseSignedExtent() {
    TileStore store(1024*1024);
    TileMap tiles;tiles.emplace(TileCoord{-1,-1},store.constant(1,1,{1,0,0,1}));
    RasterSnapshot raster(Id::generate(),{-1,-1,2,2},pack({0,0,1,1}),std::move(tiles));
    const auto h=histogramLayer(raster);
    expect(h.samples==4 && h.red[255]==1 && h.red[0]==3 && h.blue[255]==3 && h.blue[0]==1,"Signed source origin or sparse default lost");
    const auto sampled=histogramLayer(raster,2);
    // Stratum midpoint positions 1 and 3 both select the sparse blue default.
    expect(sampled.sampled && sampled.samples==2 && sampled.blue[255]==2,"Uniform bounded positions differ");
}
void boundsAndCancellation() {
    RasterSnapshot large(Id::generate(),{17,-9,1000,1000},pack({1,1,1,1}));
    const auto h=histogramLayer(large);
    expect(h.sampled && h.samples==65536 && h.red[255]==65536,"Default hard sample bound failed");
    const auto limited=histogramLayer(large,7);
    expect(limited.samples==7 && limited.sampled,"Requested sample bound failed");
    rejects([&]{histogramLayer(large,0);});rejects([&]{histogramLayer(large,65537);});
    std::stop_source stop;stop.request_stop();rejects([&]{histogramLayer(large,65536,stop.get_token());});
    RasterSnapshot transparent(Id::generate(),{0,0,3,2});
    const auto empty=histogramLayer(transparent);
    expect(empty.samples==0 && !empty.sampled && std::accumulate(empty.red.begin(),empty.red.end(),std::uint64_t{})==0,"Transparent raster should have empty bins");
}
}
int main() {
    try {binsAndAlpha();sparseSignedExtent();boundsAndCancellation();std::cout<<"Histogram tests passed\n";return 0;}
    catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
