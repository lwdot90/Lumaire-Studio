#pragma once
#include "core/editor_commands.h"

namespace compositor::engine {
// Existing enabled linked mask only. Draw toward coverage 1 (reveal) or 0
// (hide); color/erasing in settings are ignored. Document-space soft coverage,
// opacity and active selection follow the existing brush contract.
EditTransaction paintLayerMask(DocumentPtr,const Id&,TileStore&,std::span<const Coordinate>,
                              const BrushSettings&,bool reveal,std::stop_token={});
// Entire mask, independently of active selection. Radius is source pixels,
// finite 0..32; Gaussian sigma=radius/3, support=ceil(radius), normalized.
// Source-extent edges clamp to edge coverage. Radius zero is an exact no-op.
EditTransaction featherLayerMask(DocumentPtr,const Id&,TileStore&,double radius,std::stop_token={});
}
