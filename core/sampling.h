#pragma once
#include "core/raster.h"
#include "core/resampling.h"
#include <cmath>
#include <limits>
#include <stdexcept>

namespace compositor::engine {
enum class Sampling { Nearest, Bilinear, Lanczos };
// Pixel-center convention from pixel-semantics.md. Read the immutable raster,
// not isolated tiles: bilinear taps cross boundaries with no tile seams.
template<class Reader>
inline Pixel sampleFrom(Reader&& read, double x, double y, Sampling mode) {
    if(!std::isfinite(x) || !std::isfinite(y)) throw std::invalid_argument("Nonfinite sample coordinate");
    // Point sampling has a unit footprint. Viewport reduction uses a prepared
    // filter plan and mip cache, not this footprint-free convenience function.
    if(mode==Sampling::Lanczos) return sampleLanczos(read,x,y,1,1);
    if(mode!=Sampling::Nearest && mode!=Sampling::Bilinear) throw std::invalid_argument("Invalid sampling mode");
    if(mode==Sampling::Bilinear) { x-=0.5; y-=0.5; }
    const double fx=std::floor(x), fy=std::floor(y);
    // Guard float-to-integer conversions and the +1 neighbor before either is evaluated.
    constexpr auto low=std::numeric_limits<std::int64_t>::min(), high=std::numeric_limits<std::int64_t>::max();
    if(static_cast<long double>(fx)<low || static_cast<long double>(fy)<low ||
       static_cast<long double>(fx)>=high || static_cast<long double>(fy)>=high) return {};
    const auto ix=static_cast<std::int64_t>(fx), iy=static_cast<std::int64_t>(fy);
    if(mode==Sampling::Nearest) return read(ix,iy);
    const float wx=static_cast<float>(x-fx), wy=static_cast<float>(y-fy);
    const auto mix=[](Pixel a, Pixel b, float t) {
        return Pixel{a.r*(1-t)+b.r*t,a.g*(1-t)+b.g*t,a.b*(1-t)+b.b*t,a.a*(1-t)+b.a*t};
    };
    return mix(mix(read(ix,iy),read(ix+1,iy),wx),
               mix(read(ix,iy+1),read(ix+1,iy+1),wx),wy);
}
inline Pixel sample(const RasterSnapshot& raster,double x,double y,Sampling mode) {
    return sampleFrom([&](auto px,auto py){return raster.pixel(px,py);},x,y,mode);
}
}
