#pragma once
#include <array>
#include <cstdint>

namespace compositor::engine {
using Half = std::uint16_t;
using PackedPixel = std::array<Half, 4>;
struct Pixel {
    float r=0, g=0, b=0, a=0;
    bool operator==(const Pixel&) const = default;
};

// Storage conversion is independent of the host rounding mode. Reject overflow,
// NaN and infinity; preserve half subnormals, canonicalize signed zero.
Half toHalf(float value);
float fromHalf(Half value);
PackedPixel pack(Pixel value);
Pixel unpack(PackedPixel value);
void validateCanonical(PackedPixel value);
float decodeSrgb(float encoded);
float encodeSrgb(float linear);
Pixel fromStraightSrgb(Pixel encoded);
Pixel toStraightSrgb(Pixel linear);
Pixel sourceOver(Pixel source, Pixel backdrop);
}
