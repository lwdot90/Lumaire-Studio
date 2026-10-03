#include "rendering/layer_sampler.h"
#include <numeric>
#include <limits>

namespace compositor {
using namespace engine;
std::uint64_t CpuLayerSampler::scratchBytes(std::size_t layerCount) {
    constexpr std::uint64_t perLayer=2*sizeof(Plan)+2*sizeof(RenderSource)+256+8*sizeof(std::size_t);
    if(layerCount>std::numeric_limits<std::uint64_t>::max()/perLayer)
        throw std::length_error("Layer sampling scratch size overflow");
    return static_cast<std::uint64_t>(layerCount)*perLayer;
}
CpuLayerSampler::CpuLayerSampler(const LayerStack& stack,Coordinate step,MipCache& cache,bool cachedReads):cache_(cache) {
    (void)cachedReads; // Reference mode differs in culling, never in backing safety.
    if(!std::isfinite(step.x) || !std::isfinite(step.y) || step.x<=0 || step.y<=0)
        throw std::invalid_argument("Invalid layer sampling step");
    plans_.reserve(stack.prepared().size());
    for(const auto& layer:stack.prepared()) {
        ReductionPlan reduction;Coordinate support;
        if(layer.sampling==Sampling::Lanczos) {
            const auto& extent=layer.raster->extent;
            reduction=reductionPlan(layer.inverse,step,static_cast<int>(extent.width),static_cast<int>(extent.height));
            // Successive 2x filters contribute 6*(2^k-1) source pixels;
            // residual Lanczos contributes its full support at mip scale.
            support=reductionSupport(reduction);
        } else support=layer.sampling==Sampling::Bilinear ? Coordinate{.5,.5} : Coordinate{};
        plans_.push_back({layer,reduction,support,RasterReader(layer.raster),{}});
        if(layer.mask && layer.maskEnabled) plans_.back().maskReader.emplace(layer.mask);
    }
}
std::vector<std::size_t> CpuLayerSampler::all() const {
    std::vector<std::size_t> result(plans_.size());std::iota(result.begin(),result.end(),0);return result;
}
std::vector<std::size_t> CpuLayerSampler::region(SampleRegion bounds) const {
    return region(bounds,all());
}
std::vector<std::size_t> CpuLayerSampler::region(SampleRegion bounds,std::span<const std::size_t> candidates) const {
    bounds.validate();std::vector<std::size_t> result;
    for(auto i:candidates) {
        const auto& plan=plans_.at(i);
        if(intersectsSource(plan.layer.raster->extent,plan.layer.inverse,bounds,plan.support)) result.push_back(i);
    }
    return result;
}
std::optional<Pixel> CpuLayerSampler::evaluate(std::span<const std::size_t> layers,double x,double y,const std::function<bool()>& cancelled,std::stop_token stop) {
    if(!std::isfinite(x) || !std::isfinite(y)) throw std::invalid_argument("Invalid layer sample position");
    struct Cancelled {};
    const auto releaseReads=[&] {
        if(activeReader_) {
            auto& plan=plans_.at(*activeReader_);plan.reader.release();
            if(plan.maskReader) plan.maskReader->release();
        }
        cache_.releaseSourceLease();
    };
    if(stop.stop_requested() || (cancelled && cancelled())) {releaseReads();return std::nullopt;}
    Pixel output;
    try {
        for(auto index:layers) {
            if(stop.stop_requested() || (cancelled && cancelled())) {releaseReads();return std::nullopt;}
            auto& plan=plans_.at(index);const auto& layer=plan.layer;
            // Layers share one active resident lease. Release before admitting
            // another source so a deep layer stack cannot pin every tile.
            if(activeReader_ && *activeReader_!=index) {
                auto& previous=plans_.at(*activeReader_);previous.reader.release();
                if(previous.maskReader) previous.maskReader->release();
                cache_.releaseSourceLease();
            }
            activeReader_=index;
            // The previous pixel may leave a mask lease in this same plan.
            // Release it before admitting canonical color/mip source bytes.
            if(plan.maskReader) plan.maskReader->release();
            const auto sourcePixel=[&](auto px,auto py) {
                // Reference rendering retains identical scalar arithmetic while
                // using the same safe backing lease as the planned renderer.
                cache_.releaseSourceLease();
                return plan.reader.pixel(px,py,cancelled,stop);
            };
            const auto position=layer.inverse.map({x,y});Pixel pixel;
            if(layer.sampling==Sampling::Lanczos) {
                const double scale=std::ldexp(1.,static_cast<int>(plan.reduction.level));
                const auto& extent=layer.raster->extent;
                const PixelReader read=[&](auto px,auto py) {
                    if(plan.reduction.level==0) {
                        if(stop.stop_requested() || (cancelled && cancelled())) throw Cancelled{};
                        if(px<0 || py<0 || px>=extent.width || py>=extent.height) return Pixel{};
                        return sourcePixel(extent.x+px,extent.y+py);
                    }
                    plan.reader.release();
                    const auto value=cache_.pixel(layer.raster,plan.reduction.level,px,py,cancelled,stop);
                    if(!value) throw Cancelled{};
                    return *value;
                };
                pixel=sampleLanczos(read,(position.x-static_cast<double>(extent.x))/scale,
                    (position.y-static_cast<double>(extent.y))/scale,plan.reduction.residualFootprint.x,plan.reduction.residualFootprint.y);
            } else pixel=sampleFrom(sourcePixel,position.x,position.y,layer.sampling);
            // The sampled color is a value. Its backing need not remain pinned
            // while the linked mask admits another canonical tile.
            if(plan.maskReader) {plan.reader.release();cache_.releaseSourceLease();}
            float coverage=1;
            if(plan.maskReader) {
                const auto& extent=layer.mask->extent;
                const auto maskPixel=[&](auto px,auto py) {
                    if(px<extent.x || py<extent.y || px>=extent.x+extent.width || py>=extent.y+extent.height)
                        return Pixel{0,0,0,1};
                    return plan.maskReader->pixel(px,py,cancelled,stop);
                };
                coverage=std::clamp(sampleFrom(maskPixel,position.x,position.y,Sampling::Bilinear).a,0.f,1.f);
            }
            const auto opacity=layer.opacity*coverage;
            pixel.r*=opacity;pixel.g*=opacity;pixel.b*=opacity;pixel.a*=opacity;
            output=composite(pixel,output,layer.blend);
        }
    } catch(const Cancelled&) {releaseReads();return std::nullopt;}
      catch(const RasterReadCancelled&) {releaseReads();return std::nullopt;}
      catch(...) {releaseReads();throw;}
    if(stop.stop_requested() || (cancelled && cancelled())) {releaseReads();return std::nullopt;}
    return output;
}
}
