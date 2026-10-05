#pragma once
#include "core/raster.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <stop_token>

namespace compositor::engine {
struct RgbHistogram {
    std::array<std::uint64_t,256> red{},green{},blue{},luma{};
    std::uint64_t samples=0;
    bool sampled=false;
};
// Display-encoded straight sRGB bins; transparent pixels are excluded.
// Uniform stratified positions cover the entire source extent, with a hard
// 65536-position bound. Read/rehydration failures propagate to the caller.
RgbHistogram histogramLayer(const RasterSnapshot& raster,std::size_t maxSamples=65536,std::stop_token stop={});
}
