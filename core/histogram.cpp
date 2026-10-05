#include "core/histogram.h"
#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>

namespace compositor::engine {
RgbHistogram histogramLayer(const RasterSnapshot& raster,std::size_t maxSamples,std::stop_token stop) {
    if(maxSamples==0 || maxSamples>65536) throw std::invalid_argument("Histogram sample limit must be 1 to 65536");
    const auto checkpoint=[&] {if(stop.stop_requested()) throw std::runtime_error("Histogram canceled");};
    checkpoint();
    const auto total=static_cast<std::uint64_t>(raster.extent.width)*raster.extent.height;
    const auto count=std::min(total,static_cast<std::uint64_t>(maxSamples));
    RgbHistogram result;result.sampled=count<total;
    std::optional<TileCoord> current;
    Tile::ReadLease lease;
    const auto fallback=unpack(raster.defaultValue);
    const auto code=[](float value,float alpha) {return std::clamp(encodeSrgb(std::clamp(value/alpha,0.f,1.f)),0.f,1.f);};
    const auto bin=[](float value) {return static_cast<std::size_t>(std::clamp(static_cast<int>(std::lround(value*255)),0,255));};
    for(std::uint64_t i=0;i<count;++i) {
        checkpoint();
        // Integer stratum midpoints are distinct when count <= total.
        const auto offset=(2*i+1)*total/(2*count);
        const auto x=raster.extent.x+static_cast<std::int64_t>(offset%raster.extent.width);
        const auto y=raster.extent.y+static_cast<std::int64_t>(offset/raster.extent.width);
        const TileCoord coordinate{floorTile(x),floorTile(y)};
        if(!current || *current!=coordinate) {
            lease={};current=coordinate;
            const auto tile=raster.tiles.find(coordinate);
            if(tile!=raster.tiles.end()) lease=tile->second->read(stop);
        }
        auto pixel=fallback;
        if(lease) {
            const auto left=std::max(raster.extent.x,coordinate.x*tileSide);
            const auto top=std::max(raster.extent.y,coordinate.y*tileSide);
            pixel=lease.linearPixel(static_cast<int>(x-left),static_cast<int>(y-top));
        }
        if(pixel.a<=0) continue;
        const auto red=code(pixel.r,pixel.a),green=code(pixel.g,pixel.a),blue=code(pixel.b,pixel.a);
        ++result.red[bin(red)];++result.green[bin(green)];++result.blue[bin(blue)];
        ++result.luma[bin(.2126f*red+.7152f*green+.0722f*blue)];++result.samples;
    }
    checkpoint();return result;
}
}
