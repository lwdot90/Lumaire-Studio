#pragma once
#include "core/retouch.h"
#include "io/adjustment_parameters.h"
namespace compositor::io {
struct StoredRetouch {
 engine::RetouchStack stack;
 engine::Id intermediateId,renderedId;
 std::uint64_t intermediateRevision=0,renderedRevision=0;
 engine::Sampling sampling=engine::Sampling::Bilinear;
 std::string adjustment;
};
std::string retouchParameters(const engine::RetouchStack&,const engine::RasterSnapshot&,const engine::AdjustmentStack*,engine::Sampling);
StoredRetouch readRetouchParameters(const std::string&,std::shared_ptr<const engine::RasterSnapshot>);
}
