#include "core/blend.h"
#include "core/hard_mix_thresholds.h"
#include <bit>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace compositor::engine {
namespace {
using Color=std::array<float,3>;
constexpr std::array<std::string_view,blendModeCount> names{"normal","multiply","screen","overlay","darken","lighten","difference",
    "color-dodge","color-burn","hue","saturation","color","luminosity",
    "linear-burn","linear-dodge","soft-light","hard-light","vivid-light","linear-light",
    "pin-light","hard-mix","exclusion","subtract","divide"};
void validate(Pixel p) {
    for(float v:{p.r,p.g,p.b,p.a}) if(!std::isfinite(v) || std::abs(v)>65504.f) throw std::domain_error("Invalid blend pixel");
    if(p.a<0 || p.a>1) throw std::domain_error("Invalid blend alpha");
}
float overlapCode(float premultiplied,float alpha) {
    // Clamp before unpremultiplying to avoid overflow at extremely small alpha.
    if(premultiplied<=0) return 0;
    if(premultiplied>=alpha) return 1;
    return std::clamp(encodeSrgb(premultiplied/alpha),0.f,1.f);
}
float component(float d,float s,BlendMode mode) {
    switch(mode) {
        case BlendMode::Multiply:return d*s;
        case BlendMode::Screen:return d+s-d*s;
        case BlendMode::Overlay:return d<=.5f ? 2*d*s : 1-2*(1-d)*(1-s);
        case BlendMode::Darken:return std::min(d,s);
        case BlendMode::Lighten:return std::max(d,s);
        case BlendMode::Difference:return std::abs(d-s);
        case BlendMode::ColorDodge:return d==0 ? 0 : s==1 ? 1 : std::min(1.f,d/(1-s));
        case BlendMode::ColorBurn:return d==1 ? 1 : s==0 ? 0 : 1-std::min(1.f,(1-d)/s);
        case BlendMode::LinearBurn:return std::max(0.f,d+s-1);
        case BlendMode::LinearDodge:return std::min(1.f,d+s);
        case BlendMode::SoftLight: {
            if(s<=.5f) return d-(1-2*s)*d*(1-d);
            const float curve=d<=.25f ? ((16*d-12)*d+4)*d : std::sqrt(d);
            return d+(2*s-1)*(curve-d);
        }
        case BlendMode::HardLight:return s<=.5f ? 2*d*s : 1-2*(1-d)*(1-s);
        case BlendMode::VividLight:return s<=.5f ? component(d,2*s,BlendMode::ColorBurn)
            : component(d,2*s-1,BlendMode::ColorDodge);
        case BlendMode::LinearLight:return std::clamp(d+2*s-1,0.f,1.f);
        case BlendMode::PinLight:return s<=.5f ? std::min(d,2*s) : std::max(d,2*s-1);

        case BlendMode::Exclusion:return d+s-2*d*s;
        case BlendMode::Subtract:return std::max(0.f,d-s);
        case BlendMode::Divide:return s==0 ? 1 : std::min(1.f,d/s);
        default:throw std::invalid_argument("Not a separable artistic blend mode");
    }
}
unsigned hardMixCode(float channel,float alpha) {
    unsigned low=0,high=4096;
    while(low<high) {
        const auto middle=(low+high)/2;
        const float boundary=std::bit_cast<float>(hardMixThresholdBits[middle])*alpha;
        if(channel>=boundary) low=middle+1;
        else high=middle;
    }
    return low;
}
float hardMix(float source,float sourceAlpha,float backdrop,float backdropAlpha) {
    // Preserve the existing Burn/Dodge singular-corner precedence. Classify
    // interior overlap using frozen FP32 thresholds, avoiding CPU/GPU pow
    // variation turning a discontinuity into opposite black/white answers.
    if(backdrop>=backdropAlpha) return 1;
    if(backdrop<=0) return 0;
    if(source<=0) return 0;
    if(source>=sourceAlpha) return 1;
    return hardMixCode(source,sourceAlpha)+hardMixCode(backdrop,backdropAlpha)>=4096 ? 1.f : 0.f;
}
// Nonseparable helpers: W3C Compositing 2024-03-21 §10.2 (the version frozen
// in docs/pixel-semantics.md), not HSL/HSV or linear Rec.709 luminance.
float luminosity(Color c) { return .3f*c[0]+.59f*c[1]+.11f*c[2]; }
float saturation(Color c) { return *std::max_element(c.begin(),c.end())-*std::min_element(c.begin(),c.end()); }
Color withLuminosity(Color c,float target) {
    // Achromatic input remains achromatic. Subtracting its rounded luminance
    // can otherwise leave a tiny equal-channel residual with l==low/high,
    // turning the clipping denominator into zero (notably gray over black).
    if(c[0]==c[1] && c[1]==c[2]) return {target,target,target};
    if(target<=0) return {};
    if(target>=1) return {1,1,1};
    const float delta=target-luminosity(c);
    for(auto& v:c) v+=delta;
    const float l=luminosity(c),low=*std::min_element(c.begin(),c.end()),high=*std::max_element(c.begin(),c.end());
    if(low<0) {
        if(l<=low) return {target,target,target};
        for(auto& v:c) v=l+(v-l)*l/(l-low);
    }
    if(high>1) {
        if(high<=l) return {target,target,target};
        for(auto& v:c) v=l+(v-l)*(1-l)/(high-l);
    }
    return c;
}
Color withSaturation(Color c,float target) {
    std::array<std::size_t,3> order{0,1,2};
    // Stable R,G,B ordering for ties, without per-pixel heap allocation.
    if(c[order[0]]>c[order[1]]) std::swap(order[0],order[1]);
    if(c[order[1]]>c[order[2]]) std::swap(order[1],order[2]);
    if(c[order[0]]>c[order[1]]) std::swap(order[0],order[1]);
    const auto low=order[0],mid=order[1],high=order[2];
    if(c[high]>c[low]) { c[mid]=(c[mid]-c[low])*target/(c[high]-c[low]); c[high]=target; }
    else c[mid]=c[high]=0;
    c[low]=0; return c;
}
Color artistic(Color d,Color s,BlendMode mode) {
    Color out;
    switch(mode) {
        case BlendMode::Hue:out=withLuminosity(withSaturation(s,saturation(d)),luminosity(d));break;
        case BlendMode::Saturation:out=withLuminosity(withSaturation(d,saturation(s)),luminosity(d));break;
        case BlendMode::Color:out=withLuminosity(s,luminosity(d));break;
        case BlendMode::Luminosity:out=withLuminosity(d,luminosity(s));break;
        default:for(std::size_t i=0;i<3;++i) out[i]=component(d[i],s[i],mode);break;
    }
    for(auto& v:out) v=std::clamp(v,0.f,1.f);
    return out;
}
}
std::string_view blendIdentifier(BlendMode mode) {
    const auto index=static_cast<std::size_t>(mode);
    if(index>=names.size()) throw std::invalid_argument("Unknown blend mode");
    return names[index];
}
BlendMode parseBlendMode(std::string_view identifier) {
    const auto found=std::find(names.begin(),names.end(),identifier);
    if(found==names.end()) throw std::invalid_argument("Unknown required blend identifier");
    return static_cast<BlendMode>(found-names.begin());
}
Pixel composite(Pixel s,Pixel b,BlendMode mode) {
    blendIdentifier(mode); validate(s); validate(b);
    if(s.a==0) return b.a==0 ? Pixel{} : b;
    if(b.a==0) return s;
    if(mode==BlendMode::Normal) return sourceOver(s,b);
    const Color source{s.r,s.g,s.b},backdrop{b.r,b.g,b.b}; Color cs{},cb{};
    Color overlap{};
    if(mode==BlendMode::HardMix) {
        for(std::size_t i=0;i<3;++i) overlap[i]=hardMix(source[i],s.a,backdrop[i],b.a);
    } else {
        for(std::size_t i=0;i<3;++i) { cs[i]=overlapCode(source[i],s.a); cb[i]=overlapCode(backdrop[i],b.a); }
        overlap=artistic(cb,cs,mode);
    }
    Color out{};
    for(std::size_t i=0;i<3;++i) out[i]=source[i]*(1-b.a)+(s.a*b.a)*decodeSrgb(overlap[i])+backdrop[i]*(1-s.a);
    return {out[0],out[1],out[2],s.a+b.a*(1-s.a)};
}
}
