#pragma once
#include "core/editing_types.h"

namespace compositor::engine {
// Selection and placement are validated by the immutable document/caller.
// Rasterize on the source pixel footprint, then apply coverage once to the
// command's proposed result. Inverted selections remain zero outside canvas.
inline float selectionCoverage(const std::optional<Selection>& selection,const Affine& sourceToDocument,
    std::int64_t sourceX,std::int64_t sourceY,int canvasWidth,int canvasHeight) {
    if(!selection) return 1;
    unsigned selected=0;
    for(int y=0;y<8;++y) for(int x=0;x<8;++x) {
        const auto point=sourceToDocument.map({static_cast<double>(sourceX)+(x+.5)/8,
                                             static_cast<double>(sourceY)+(y+.5)/8});
        if(point.x>=0 && point.y>=0 && point.x<canvasWidth && point.y<canvasHeight)
            selected+=selection->coverage(point)>0 ? 1u : 0u;
    }
    return static_cast<float>(selected)/64;
}
}
