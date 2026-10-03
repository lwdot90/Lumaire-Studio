#pragma once
#include "viewport.h"
#include <vector>

namespace compositor {
// Navigation background; document pixels are rendered separately.
struct PaintRect { int x,y,width,height; float value; };
inline std::vector<PaintRect> checkerboard(const Viewport& view, int width,int height,Point cursor,bool showCursor) {
    std::vector<PaintRect> out;
    const auto a=view.toDevice({0,0}), b=view.toDevice({view.documentWidth,view.documentHeight});
    const int left=static_cast<int>(std::clamp(a.x,0.0,double(width)));
    const int top=static_cast<int>(std::clamp(a.y,0.0,double(height)));
    const int right=static_cast<int>(std::clamp(b.x,0.0,double(width)));
    const int bottom=static_cast<int>(std::clamp(b.y,0.0,double(height)));
    const int cell=std::max(1,static_cast<int>(16*view.dpr));
    for(int y=top;y<bottom;) {
        const int endY=std::min(bottom,(y/cell+1)*cell);
        for(int x=left;x<right;) {
            const int endX=std::min(right,(x/cell+1)*cell);
            out.push_back({x,y,endX-x,endY-y,((x/cell+y/cell)%2) ? .7F : .85F});
            x=endX;
        }
        y=endY;
    }
    if(showCursor) {
        const int x=static_cast<int>(std::clamp(cursor.x*view.dpr,0.0,double(width)));
        const int y=static_cast<int>(std::clamp(cursor.y*view.dpr,0.0,double(height)));
        if(x>=8 && x+8<width && y>=8 && y+8<height) {
            out.push_back({x-8,y,17,1,1}); out.push_back({x,y-8,1,17,1});
        }
    }
    return out;
}
} // namespace compositor
