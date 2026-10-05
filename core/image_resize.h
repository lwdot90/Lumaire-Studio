#pragma once
#include "core/document.h"
#include <stop_token>
namespace compositor::engine {
// Resample retained source grids, preserving editable layers and frozen strokes.
// Lanczos uses premultiplied-linear filtering with clamped source edges.
EditTransaction resampleDocument(DocumentPtr,TileStore&,int width,int height,Sampling,std::stop_token={});
}
