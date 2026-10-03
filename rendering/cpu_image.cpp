#include "rendering/cpu_image.h"
#include "core/sampling.h"
#include "rendering/display_pixel.h"
#include "rendering/layer_sampler.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <limits>

namespace compositor {
std::uint64_t cpuImageAllocationBytes(int width,int height) {
    if(width<=0 || height<=0 || width>32768 || height>32768 || std::int64_t(width)*height>64*1024*1024)
        throw std::length_error("Viewport render exceeds memory budget");
    return static_cast<std::uint64_t>(width)*static_cast<std::uint64_t>(height)*4;
}
std::uint64_t cpuRenderScratchBytes(const engine::DocumentSnapshot& document) {
    constexpr std::uint64_t base=4*1024*1024;
    const auto sources=CpuLayerSampler::scratchBytes(document.stack.prepared().size());
    if(sources>std::numeric_limits<std::uint64_t>::max()-base)
        throw std::length_error("CPU rendering scratch size overflow");
    return base+sources;
}
QImage renderCpuImage(const engine::DocumentSnapshot& document,const Viewport& view,int width,int height,
                      const std::function<bool()>& cancelled,CpuRenderMode mode,CpuRenderStats* stats,MipCache* mipCache,std::stop_token stop) {
    if(stats) *stats={};
    CpuRenderStats measured;
    (void)cpuImageAllocationBytes(width,height);
    for(const auto value:{view.width,view.height,view.dpr,view.zoom,view.center.x,view.center.y})
        if(!std::isfinite(value)) throw std::invalid_argument("Nonfinite viewport");
    if(view.width<=0 || view.height<=0 || view.dpr<=0 || view.zoom<=0)
        throw std::invalid_argument("Invalid viewport geometry");
    if(mode!=CpuRenderMode::Regions && mode!=CpuRenderMode::Reference) throw std::invalid_argument("Invalid CPU render mode");
    if(stop.stop_requested() || (cancelled && cancelled())) return {};
    const auto positionAt=[&](int x,int y){return view.documentPixel(x,y);};
    const auto first=positionAt(0,0),last=positionAt(width-1,height-1);
    const engine::SampleRegion bounds{first.x,first.y,last.x,last.y}; bounds.validate();
    MipCache localCache(16*1024*1024);
    CpuLayerSampler sampler(document.stack,{1/view.zoom,1/view.zoom},mipCache ? *mipCache : localCache,mode!=CpuRenderMode::Reference);
    const auto viewport=mode==CpuRenderMode::Regions ? sampler.region(bounds) : sampler.all();
    measured.viewportLayers=viewport.size();
    QImage image(width,height,QImage::Format_RGB32);
    if(image.isNull()) throw std::bad_alloc();
    const int cell=static_cast<int>(std::clamp(16*view.dpr,1.0,32768.0));
    const float backgrounds[]{engine::decodeSrgb(.85f),engine::decodeSrgb(.7f)};
    // Adapt TiledLayerRenderer's visible-piece clipping. Regions own only
    // immutable references and preserve sampling across source tile boundaries.
    constexpr int blockSize=64;
    for(int top=0;top<height;top+=blockSize) for(int left=0;left<width;left+=blockSize) {
        if(stop.stop_requested() || (cancelled && cancelled())) return {};
        const int right=std::min(left+blockSize,width),bottom=std::min(top+blockSize,height);
        std::vector<std::size_t> region;
        if(mode==CpuRenderMode::Regions) {
            const auto a=positionAt(left,top),b=positionAt(right-1,bottom-1);
            region=sampler.region({a.x,a.y,b.x,b.y},viewport);
        } else region=viewport;
        ++measured.blocks;
        for(int y=top;y<bottom;++y) {
        if(stop.stop_requested() || (cancelled && cancelled())) return {};
        auto* row=reinterpret_cast<QRgb*>(image.scanLine(y));
        for(int x=left;x<right;++x) {
            const auto position=positionAt(x,y);
            if(position.x<0 || position.y<0 || position.x>=document.width || position.y>=document.height) { row[x]=qRgb(31,31,31); continue; }
            const float background=backgrounds[(x/cell+y/cell)&1];
            const auto pixel=sampler.evaluate(region,position.x,position.y,cancelled,stop);
            if(!pixel) return {};
            measured.layerSamples+=region.size();
            const int dx=static_cast<int>(std::floor(position.x)),dy=static_cast<int>(std::floor(position.y));
            row[x]=displayArgb(*pixel,background,dx,dy);
        }
        }
    }
    if(stop.stop_requested() || (cancelled && cancelled())) return {};
    if(stats) *stats=measured;
    return image;
}
}
