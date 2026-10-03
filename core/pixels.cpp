#include "core/pixels.h"
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace compositor::engine {
static_assert(sizeof(float)==4 && std::numeric_limits<float>::is_iec559);
namespace {
std::uint32_t roundShift(std::uint32_t value, unsigned shift) {
    const auto whole=value>>shift, mask=(1u<<shift)-1, remainder=value&mask;
    const auto midpoint=1u<<(shift-1);
    return whole+(remainder>midpoint || (remainder==midpoint && (whole&1)));
}
void validate(Pixel p) {
    for(float v:{p.r,p.g,p.b,p.a})
        if(!std::isfinite(v) || std::abs(v)>65504.f) throw std::domain_error("Nonfinite or overflowing pixel");
    if(p.a<0 || p.a>1) throw std::domain_error("Alpha outside [0,1]");
}
}
Half toHalf(float value) {
    if(!std::isfinite(value) || std::abs(value)>65504.f) throw std::domain_error("Invalid binary16 output");
    const auto bits=std::bit_cast<std::uint32_t>(value);
    const auto sign=(bits>>16)&0x8000u, exponent=(bits>>23)&255u, fraction=bits&0x7fffffu;
    std::uint32_t result=0;
    if(exponent>=113) result=((exponent-112)<<10)+roundShift(fraction,13);
    else if(exponent>=102) result=roundShift(fraction|0x800000u,126-exponent);
    return static_cast<Half>(result ? sign|result : 0);
}
float fromHalf(Half value) {
    const auto exponent=(value>>10)&31, fraction=value&1023;
    if(exponent==31) throw std::domain_error("Nonfinite binary16 input");
    // Every finite binary16 value is exactly representable in binary32.
    // Normal values only need exponent rebiasing and mantissa placement;
    // subnormals are an exact integer multiplication by 2^-24.
    if(exponent==0) {
        const float magnitude=static_cast<float>(fraction)*0x1p-24f;
        return (value&0x8000) ? -magnitude : magnitude;
    }
    return std::bit_cast<float>((std::uint32_t(value&0x8000)<<16) |
        (std::uint32_t(exponent+112)<<23) | (std::uint32_t(fraction)<<13));
}
PackedPixel pack(Pixel value) {
    validate(value);
    const auto alpha=toHalf(value.a);
    if(alpha==0) return {};
    return {toHalf(value.r),toHalf(value.g),toHalf(value.b),alpha};
}
Pixel unpack(PackedPixel value) {
    return {fromHalf(value[0]),fromHalf(value[1]),fromHalf(value[2]),fromHalf(value[3])};
}
void validateCanonical(PackedPixel value) {
    for(auto v:value) if(v==0x8000) throw std::domain_error("Noncanonical negative zero");
    const auto p=unpack(value);
    validate(p);
    if(p.a==0 && (value[0] || value[1] || value[2])) throw std::domain_error("Nonzero transparent RGB");
}
float decodeSrgb(float encoded) {
    if(!std::isfinite(encoded)) throw std::domain_error("Invalid sRGB value");
    const float c=std::abs(encoded);
    return std::copysign(c<=0.04045f ? c/12.92f : std::pow((c+0.055f)/1.055f,2.4f),encoded);
}
float encodeSrgb(float linear) {
    if(!std::isfinite(linear)) throw std::domain_error("Invalid linear value");
    const float c=std::abs(linear);
    return std::copysign(c<=0.0031308f ? 12.92f*c : 1.055f*std::pow(c,1.f/2.4f)-0.055f,linear);
}
Pixel fromStraightSrgb(Pixel encoded) {
    validate(encoded);
    if(encoded.a==0) return {};
    return {decodeSrgb(encoded.r)*encoded.a,decodeSrgb(encoded.g)*encoded.a,decodeSrgb(encoded.b)*encoded.a,encoded.a};
}
Pixel toStraightSrgb(Pixel linear) {
    validate(linear);
    if(linear.a==0) return {};
    return {encodeSrgb(linear.r/linear.a),encodeSrgb(linear.g/linear.a),encodeSrgb(linear.b/linear.a),linear.a};
}
Pixel sourceOver(Pixel s, Pixel b) {
    validate(s); validate(b);
    if(s.a==0) s={};
    if(b.a==0) b={};
    const float remaining=1-s.a;
    return {s.r+b.r*remaining,s.g+b.g*remaining,s.b+b.b*remaining,s.a+b.a*remaining};
}
}
