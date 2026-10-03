#pragma once
#include "core/layer_stack.h"
#include "rendering/mip_cache.h"
#include "rendering/raster_reader.h"

namespace compositor {
// Per-viewport immutable sampling plans derived from the canonical layer list.
// Reusable source mip storage is supplied by the owning render worker.
class CpuLayerSampler {
public:
    // Includes plan capacity, shared source/control storage and temporary indices.
    static std::uint64_t scratchBytes(std::size_t layerCount);
    CpuLayerSampler(const engine::LayerStack& stack,engine::Coordinate step,MipCache& cache,bool cachedReads=true);
    std::vector<std::size_t> region(engine::SampleRegion bounds) const;
    std::vector<std::size_t> region(engine::SampleRegion bounds,std::span<const std::size_t> candidates) const;
    std::vector<std::size_t> all() const;
    std::optional<engine::Pixel> evaluate(std::span<const std::size_t> layers,double x,double y,
                                        const std::function<bool()>& cancelled={},std::stop_token stop={});
private:
    struct Plan {
        engine::LayerStack::Prepared layer;
        engine::ReductionPlan reduction;
        engine::Coordinate support;
        RasterReader reader;
        std::optional<RasterReader> maskReader;
    };
    std::vector<Plan> plans_;
    MipCache& cache_;
    std::optional<std::size_t> activeReader_;
};
}
