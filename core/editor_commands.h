#pragma once
#include "core/document.h"
#include "core/editing_types.h"
#include <span>
#include <stop_token>

namespace compositor::engine {
// Worker-owned immutable commands. The UI publishes a successful transaction
// as one history entry; exceptions or cancellation publish no partial edit.
EditTransaction brushStroke(DocumentPtr,const Id&,TileStore&,std::span<const Coordinate>,const BrushSettings&,std::stop_token={});
// Parameter-only validation; safe before scheduling work on the UI thread.
bool adjustmentIsNeutral(const AdjustmentParameters&);
EditTransaction adjustLayer(DocumentPtr,const Id&,TileStore&,const AdjustmentParameters&,std::stop_token={});
EditTransaction transformLayer(DocumentPtr,const Id&,const TransformParameters&);
EditTransaction cropDocument(DocumentPtr,Extent bounds);
EditTransaction resizeDocument(DocumentPtr,int width,int height);
EditTransaction selectDocument(DocumentPtr,std::optional<Selection>);
EditTransaction createLayerMask(DocumentPtr,const Id&,TileStore&,bool fromSelection,std::stop_token={});
EditTransaction removeLayerMask(DocumentPtr,const Id&);
EditTransaction setLayerMaskEnabled(DocumentPtr,const Id&,bool enabled);
}
