#pragma once
#include "core/pixels.h"
#include <algorithm>
#include <cstdint>

namespace compositor {
inline int displayBayer(int x,int y) {
    constexpr int base[2][2]={{0,2},{3,1}};
    int value=0;
    for(int bit=0;bit<3;++bit) value=value*4+base[(y>>bit)&1][(x>>bit)&1];
    return value;
}
inline int displayQuantize(float value,float dither) {
    const float v=std::clamp(255*value+dither,0.f,255.f);
    const int floor=static_cast<int>(v);const float remainder=v-static_cast<float>(floor);
    return floor+(remainder>.5f || (remainder==.5f && (floor&1)));
}
// Shared scalar display oracle, byte-for-byte the existing CPU presentation.
// Caller clips to canvas and supplies absolute integer document coordinates.
inline std::uint32_t displayArgb(engine::Pixel pixel,float background,int x,int y) {
    const auto composite=engine::sourceOver(pixel,{background,background,background,1});
    const float dither=(static_cast<float>(displayBayer(x&7,y&7))+.5f)/64-.5f;
    const auto r=static_cast<std::uint32_t>(displayQuantize(engine::encodeSrgb(composite.r),dither));
    const auto g=static_cast<std::uint32_t>(displayQuantize(engine::encodeSrgb(composite.g),dither));
    const auto b=static_cast<std::uint32_t>(displayQuantize(engine::encodeSrgb(composite.b),dither));
    return 0xff000000u|(r<<16)|(g<<8)|b;
}
}
