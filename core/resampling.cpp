#include "core/resampling.h"
#include <limits>

namespace compositor::engine {
namespace {
void add(Pixel& into,Pixel value,float weight) {
    into.r+=value.r*weight;into.g+=value.g*weight;
    into.b+=value.b*weight;into.a+=value.a*weight;
}
}
float lanczos3(float distance) {
    const float t=std::abs(distance);
    if(t>=3) return 0;
    if(t==0) return 1;
    // Exact cardinal zeros avoid tiny sin(pi) residues at identity sampling.
    if(t==1 || t==2) return 0;
    const float angle=std::numbers::pi_v<float>*t;
    return (std::sin(angle)/angle)*(std::sin(angle/3)/(angle/3));
}
FilterAxis lanczosAxis(double center,double footprint) {
    if(!std::isfinite(center) || !std::isfinite(footprint) || footprint<1)
        throw std::invalid_argument("Invalid Lanczos geometry");
    // Bound planning memory before allocation. Oversized direct footprints
    // must be handled by a caller's bounded fallback, never silently narrowed.
    constexpr double maxFootprint=32768;
    if(footprint>maxFootprint) throw std::length_error("Lanczos footprint exceeds direct-filter budget");
    const double first=std::ceil(center-3*footprint-.5),last=std::floor(center+3*footprint-.5);
    constexpr auto low=std::numeric_limits<std::int64_t>::min(),high=std::numeric_limits<std::int64_t>::max();
    if(static_cast<long double>(first)<low || static_cast<long double>(last)>=high)
        throw std::out_of_range("Lanczos tap coordinate overflow");
    const auto begin=static_cast<std::int64_t>(first),end=static_cast<std::int64_t>(last);
    if(end<begin || last-first>6*maxFootprint) throw std::out_of_range("Unrepresentable Lanczos tap interval");
    FilterAxis taps; taps.reserve(static_cast<std::size_t>(end-begin+1));
    float sum=0;
    for(auto index=begin;index<=end;++index) {
        const float weight=lanczos3(static_cast<float>((static_cast<double>(index)+.5-center)/footprint))/static_cast<float>(footprint);
        taps.push_back({index,weight});sum+=weight;
    }
    if(!std::isfinite(sum) || sum<=0) throw std::runtime_error("Invalid Lanczos normalization");
    // Includes exterior taps: clipping this normalization to the image would
    // incorrectly make transparent edges opaque.
    for(auto& tap:taps) tap.weight/=sum;
    return taps;
}
Pixel finishFiltered(Pixel value) {
    for(float channel:{value.r,value.g,value.b,value.a})
        if(!std::isfinite(channel)) throw std::overflow_error("Nonfinite filtered pixel");
    if(value.a<=0) return {};
    const float alpha=std::min(value.a,1.f),ratio=alpha/value.a;
    Pixel result{value.r*ratio,value.g*ratio,value.b*ratio,alpha};
    // Preserve extended straight RGB; do not use the original byte-format
    // clamp-to-alpha operation. Canonical positive zero is still required.
    if(result.r==0) result.r=0;
    if(result.g==0) result.g=0;
    if(result.b==0) result.b=0;
    return result;
}
Pixel sampleLanczos(const PixelReader& read,double x,double y,double footprintX,double footprintY) {
    if(!read) throw std::invalid_argument("Missing source reader");
    const auto xs=lanczosAxis(x,footprintX),ys=lanczosAxis(y,footprintY);
    Pixel result;
    for(const auto& yt:ys) {
        Pixel row;
        for(const auto& xt:xs) if(xt.weight!=0 && yt.weight!=0) add(row,read(xt.index,yt.index),xt.weight);
        add(result,row,yt.weight);
    }
    return finishFiltered(result);
}
ReductionPlan reductionPlan(const Affine& inverse,Coordinate step,int width,int height,bool nearest) {
    if(width<1 || height<1 || width>30000 || height>30000 || !std::isfinite(step.x) || !std::isfinite(step.y) || step.x<=0 || step.y<=0)
        throw std::invalid_argument("Invalid reduction geometry");
    for(double value:{inverse.a,inverse.b,inverse.c,inverse.d,inverse.tx,inverse.ty})
        if(!std::isfinite(value)) throw std::invalid_argument("Nonfinite inverse footprint");
    ReductionPlan plan;
    if(nearest) return plan;
    const double ax=inverse.a*step.x,cx=inverse.c*step.y,by=inverse.b*step.x,dy=inverse.d*step.y;
    const double x=std::hypot(ax,cx),y=std::hypot(by,dy);
    if(!std::isfinite(x) || !std::isfinite(y) || x<=0 || y<=0)
        throw std::invalid_argument("Invalid inverse footprint");
    // Normalized row dot product avoids overflow at large finite scales.
    const double dot=(ax/x)*(by/y)+(cx/x)*(dy/y);
    constexpr double tolerance=32*std::numeric_limits<double>::epsilon();
    plan.anisotropic=std::abs(x-y)>tolerance*std::max(x,y) || std::abs(dot)>tolerance;
    plan.sourceFootprint={std::max(1.,x),std::max(1.,y)};
    plan.residualFootprint=plan.sourceFootprint;
    if(plan.anisotropic) return plan;
    unsigned available=0;
    for(int dimension=std::max(width,height);dimension>1;dimension=(dimension+1)/2) ++available;
    const double wanted=std::max(0.,std::floor(std::log2(std::max(x,y))));
    plan.level=static_cast<unsigned>(std::min(wanted,static_cast<double>(available)));
    const double scale=std::ldexp(1.,static_cast<int>(plan.level));
    plan.residualFootprint={std::max(1.,x/scale),std::max(1.,y/scale)};
    return plan;
}
std::optional<FilteredRegion> halveRegion(const PixelReader& read,int width,int height,Extent output,
                                         const std::function<bool()>& cancelled) {
    if(!read || width<1 || height<1 || width>30000 || height>30000)
        throw std::invalid_argument("Invalid mip source");
    output.validate();
    if(output.x<0 || output.y<0 || output.width>256 || output.height>256 ||
       output.x+output.width>(width+1)/2 || output.y+output.height>(height+1)/2)
        throw std::invalid_argument("Invalid mip output region");
    if(cancelled && cancelled()) return std::nullopt;
    std::vector<FilterAxis> xs,ys;
    for(auto x=output.x;x<output.x+output.width;++x) xs.push_back(lanczosAxis(static_cast<double>(2*x+1),2));
    for(auto y=output.y;y<output.y+output.height;++y) ys.push_back(lanczosAxis(static_cast<double>(2*y+1),2));
    const auto first=ys.front().front().index,last=ys.back().back().index;
    // At most (2*256+10)*256 FP32 pixels, about 2.04 MiB, plus output/taps.
    std::vector<Pixel> rows(static_cast<std::size_t>((last-first+1)*output.width));
    for(auto y=first;y<=last;++y) {
        if(cancelled && cancelled()) return std::nullopt;
        if(y<0 || y>=height) continue;
        for(std::size_t x=0;x<xs.size();++x) {
            auto& row=rows[static_cast<std::size_t>(y-first)*xs.size()+x];
            for(const auto& tap:xs[x]) if(tap.index>=0 && tap.index<width && tap.weight!=0)
                add(row,read(tap.index,y),tap.weight);
        }
    }
    FilteredRegion result{output,std::vector<Pixel>(static_cast<std::size_t>(output.width*output.height))};
    for(std::size_t y=0;y<ys.size();++y) {
        if(cancelled && cancelled()) return std::nullopt;
        for(std::size_t x=0;x<xs.size();++x) {
            Pixel pixel;
            for(const auto& tap:ys[y]) add(pixel,rows[static_cast<std::size_t>(tap.index-first)*xs.size()+x],tap.weight);
            result.pixels[y*xs.size()+x]=finishFiltered(pixel);
        }
    }
    return result;
}
}
