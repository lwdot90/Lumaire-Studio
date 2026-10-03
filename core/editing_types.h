#pragma once
#include "core/tiles.h"
#include "core/affine.h"
#include <cmath>
#include <optional>
#include <stdexcept>

namespace compositor::engine {
enum class SelectionShape { Rectangle, Ellipse };
struct Selection {
    Extent bounds;
    SelectionShape shape=SelectionShape::Rectangle;
    bool inverted=false;
    bool operator==(const Selection&) const = default;
    void validate(int width,int height) const {
        if(bounds.width<0 || bounds.height<0) throw std::invalid_argument("Negative selection dimensions");
        if(bounds.x<0 || bounds.y<0 || bounds.x>width || bounds.y>height ||
           bounds.width>width-bounds.x || bounds.height>height-bounds.y ||
           (shape!=SelectionShape::Rectangle && shape!=SelectionShape::Ellipse))
            throw std::invalid_argument("Selection exceeds canvas");
    }
    float coverage(double x,double y) const {
        if(!std::isfinite(x) || !std::isfinite(y)) return 0;
        bool inside=x>=static_cast<double>(bounds.x) && y>=static_cast<double>(bounds.y) && x<static_cast<double>(bounds.x+bounds.width) && y<static_cast<double>(bounds.y+bounds.height);
        if(inside && shape==SelectionShape::Ellipse) {
            const double rx=static_cast<double>(bounds.width)/2,ry=static_cast<double>(bounds.height)/2;
            const double dx=(x-static_cast<double>(bounds.x)-rx)/rx;
            const double dy=(y-static_cast<double>(bounds.y)-ry)/ry;
            inside=dx*dx+dy*dy<=1;
        }
        return inside!=inverted ? 1.f : 0.f;
    }
    float coverage(Coordinate point) const {return coverage(point.x,point.y);}
};
using BrushPoint=Coordinate;
struct BrushSettings {
    double diameter=40,hardness=1,opacity=1;
    Pixel color{0,0,0,1}; // Straight encoded sRGB; opacity caps the whole stroke.
    bool erasing=false,paintMask=false;
};
struct TransformParameters {
    double dx=0,dy=0,scaleX=1,scaleY=1,rotationDegrees=0;
};
enum class AdjustmentKind { Exposure, Brightness, Contrast, Saturation };
struct AdjustmentParameters {
    AdjustmentKind kind=AdjustmentKind::Exposure;
    double value=0;
};
}
