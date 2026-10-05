#pragma once
#include "core/adjustment_stack.h"
#include "core/sampling.h"
namespace compositor::io {
struct StoredAdjustment {engine::AdjustmentStack stack; engine::Id renderedId; std::uint64_t renderedRevision; engine::Sampling sampling;};
std::string adjustmentParameters(const engine::AdjustmentStack&,const engine::RasterSnapshot&,engine::Sampling);
StoredAdjustment readAdjustmentParameters(const std::string&,std::shared_ptr<const engine::RasterSnapshot>,int formatVersion=5);
}
