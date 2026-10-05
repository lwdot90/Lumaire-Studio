#pragma once
#include "core/tiles.h"
#include "core/affine.h"
#include <cmath>
#include <algorithm>
#include <array>
#include <optional>
#include <stdexcept>
#include <vector>

namespace compositor::engine {
enum class SelectionShape { Rectangle, Ellipse };
struct Selection {
    Extent bounds;
    SelectionShape shape=SelectionShape::Rectangle;
    bool inverted=false;
    double featherRadius=0; // Document pixels; smooth transition spans twice this radius.
    bool operator==(const Selection&) const = default;
    void validate(int width,int height) const {
        if(!std::isfinite(featherRadius) || featherRadius<0 || featherRadius>256)
            throw std::invalid_argument("Invalid selection feather radius");
        if(bounds.width<0 || bounds.height<0) throw std::invalid_argument("Negative selection dimensions");
        if(bounds.x<0 || bounds.y<0 || bounds.x>width || bounds.y>height ||
           bounds.width>width-bounds.x || bounds.height>height-bounds.y ||
           (shape!=SelectionShape::Rectangle && shape!=SelectionShape::Ellipse))
            throw std::invalid_argument("Selection exceeds canvas");
    }
    float coverage(double x,double y) const {
        if(!std::isfinite(x) || !std::isfinite(y)) return 0;
        if(featherRadius>0) {
            if(bounds.width==0 || bounds.height==0) return inverted ? 1.f : 0.f;
            const double rx=static_cast<double>(bounds.width)/2,ry=static_cast<double>(bounds.height)/2;
            const double dx=x-static_cast<double>(bounds.x)-rx,dy=y-static_cast<double>(bounds.y)-ry;
            double distance=0;
            if(shape==SelectionShape::Ellipse) {
                // Normalized radial distance scaled by the shorter semiaxis.
                // This defined ellipse metric avoids iterative closest-point searches.
                distance=(1-std::hypot(dx/rx,dy/ry))*std::min(rx,ry);
            } else {
                const double qx=std::abs(dx)-rx,qy=std::abs(dy)-ry;
                distance=-(std::hypot(std::max(qx,0.),std::max(qy,0.))+std::min(std::max(qx,qy),0.));
            }
            const double t=std::clamp(.5+distance/(2*featherRadius),0.,1.);
            const float selected=static_cast<float>(t*t*(3-2*t));
            return inverted ? 1.f-selected : selected;
        }
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
enum class AdjustmentKind { Exposure, Brightness, Contrast, Saturation, Levels, Curves, ColorBalance };
struct LevelsParameters {
    double inputBlack=0,inputWhite=1,gamma=1,outputBlack=0,outputWhite=1;
    bool operator==(const LevelsParameters&) const = default;
};
struct CurvePoint {
    double input=0,output=0;
    bool operator==(const CurvePoint&) const = default;
};
struct ColorBalanceParameters {
    double warmth=0,tint=0; // Relative RGB correction, not Kelvin temperature.
    bool operator==(const ColorBalanceParameters&) const = default;
};
struct AdjustmentParameters {
    AdjustmentKind kind=AdjustmentKind::Exposure;
    double value=0;
    LevelsParameters levels;
    std::vector<CurvePoint> curve{{0,0},{1,1}};
    ColorBalanceParameters colorBalance;
    std::array<std::vector<CurvePoint>,3> channelCurves{
        std::vector<CurvePoint>{{0,0},{1,1}},
        std::vector<CurvePoint>{{0,0},{1,1}},
        std::vector<CurvePoint>{{0,0},{1,1}}};
    bool operator==(const AdjustmentParameters&) const = default;
};
}
