#pragma once
#include "core/editing_types.h"
#include "core/raster.h"
#include <stop_token>

namespace compositor::engine {
// Immutable retained source and ordered whole-layer operations. The displayed
// raster is a derived cache; it never replaces the source of later revisions.
struct AdjustmentStack {
    std::shared_ptr<const RasterSnapshot> source;
    std::vector<AdjustmentParameters> operations;
    std::uint32_t policy=2; // Legacy RGBA16F appearance plus master-then-channel curves.
};
// Metadata-only validation. Unsupported policies and unbounded stacks fail.
void validateAdjustmentStack(const AdjustmentStack&,const RasterSnapshot& rendered);
// Worker only; bounded immutable evaluation through the existing tile engine.
std::shared_ptr<const RasterSnapshot> evaluateAdjustmentStack(const AdjustmentStack&,TileStore&,std::stop_token={});
}
