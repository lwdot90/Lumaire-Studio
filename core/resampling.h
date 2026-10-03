#pragma once
#include "core/affine.h"
#include "core/tiles.h"
#include <functional>
#include <optional>

namespace compositor::engine {
// A reader addresses one shared source grid, never an independently padded
// tile. Mip intermediates are FP32; document tiles remain canonical binary16.
using PixelReader=std::function<Pixel(std::int64_t,std::int64_t)>;
struct FilterTap {std::int64_t index;float weight;};
using FilterAxis=std::vector<FilterTap>;
float lanczos3(float distance);
FilterAxis lanczosAxis(double center,double footprint);
Pixel finishFiltered(Pixel value);
Pixel sampleLanczos(const PixelReader& read,double x,double y,double footprintX,double footprintY);

struct ReductionPlan {
    unsigned level=0;
    bool anisotropic=false;
    Coordinate sourceFootprint{1,1};
    Coordinate residualFootprint{1,1};
};
// Inverse maps document to source; step maps device pixels to document units.
// Only a similarity footprint uses isotropic mips; shear/anisotropy uses direct
// source-axis support. Nearest bypasses all reduction, including mip selection.
ReductionPlan reductionPlan(const Affine& inverse,Coordinate step,int width,int height,bool nearest=false);
inline Coordinate reductionSupport(const ReductionPlan& plan) {
    const double scale=std::ldexp(1.,static_cast<int>(plan.level));
    return {6*(scale-1)+3*plan.residualFootprint.x*scale,6*(scale-1)+3*plan.residualFootprint.y*scale};
}

struct FilteredRegion {
    Extent extent;
    std::vector<Pixel> pixels;
};
// Exact 2x reduction on a stable local grid: output i maps to parent 2*i+1.
// Width/height describe the entire parent, not the requested output piece.
// Exterior is transparent even when the source reader has a nonzero default.
// Bounds are in the ceil-halved output grid, at most 256x256. Scratch and output
// are bounded independently of the parent dimensions. Cancellation returns no
// partial output. The caller publishes/caches only successful complete regions.
std::optional<FilteredRegion> halveRegion(const PixelReader& read,int width,int height,Extent output,
                                         const std::function<bool()>& cancelled={});
}
