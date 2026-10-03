#pragma once
#include "core/pixels.h"
#include <string_view>
#include <cstddef>

namespace compositor::engine {
// The upstream mode set uses stable Linux IDs: retain the original 13 values
// and append extensions. Apple implementations are replaced by the scalar oracle.
enum class BlendMode { Normal, Multiply, Screen, Overlay, Darken, Lighten, Difference,
                       ColorDodge, ColorBurn, Hue, Saturation, Color, Luminosity,
                       LinearBurn, LinearDodge, SoftLight, HardLight, VividLight,
                       LinearLight, PinLight, HardMix, Exclusion, Subtract, Divide,
                       Last=Divide };
inline constexpr std::size_t blendModeCount=static_cast<std::size_t>(BlendMode::Last)+1;
std::string_view blendIdentifier(BlendMode mode);
BlendMode parseBlendMode(std::string_view identifier);
// FP32 premultiplied linear input/output. Artistic overlap alone uses bounded,
// encoded sRGB; never clamp extended non-overlap contributions to [0,alpha].
Pixel composite(Pixel source,Pixel backdrop,BlendMode mode);
}
