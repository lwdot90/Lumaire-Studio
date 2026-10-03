#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace compositor::engine {
struct Coordinate { double x=0,y=0; };
// Column vectors, top-left origin and y down, matching the original renderer.
struct Affine {
    double a=1,b=0,c=0,d=1,tx=0,ty=0;
    bool operator==(const Affine&) const = default;
    Coordinate map(Coordinate p) const { return {a*p.x+c*p.y+tx,b*p.x+d*p.y+ty}; }
    Affine inverse() const {
        for(auto v:{a,b,c,d,tx,ty}) if(!std::isfinite(v)) throw std::invalid_argument("Nonfinite affine transform");
        const double determinant=a*d-b*c;
        if(!std::isfinite(determinant) || determinant==0) throw std::invalid_argument("Singular affine transform");
        Affine result{d/determinant,-b/determinant,-c/determinant,a/determinant,0,0};
        result.tx=-(result.a*tx+result.c*ty); result.ty=-(result.b*tx+result.d*ty);
        const double norm=std::max({std::abs(a)+std::abs(c)+std::abs(tx),std::abs(b)+std::abs(d)+std::abs(ty),1.0});
        const double inverseNorm=std::max({std::abs(result.a)+std::abs(result.c)+std::abs(result.tx),
            std::abs(result.b)+std::abs(result.d)+std::abs(result.ty),1.0});
        for(auto v:{result.a,result.b,result.c,result.d,result.tx,result.ty})
            if(!std::isfinite(v)) throw std::invalid_argument("Affine inverse overflow");
        if(!std::isfinite(norm*inverseNorm) || norm*inverseNorm>1e12) throw std::invalid_argument("Ill-conditioned affine transform");
        return result;
    }
    // Adapted from LayerTransform.swift and BrushRaster.pixelToDocument in
    // Document/BrushStroke.swift: translate to center, rotate, scale/flip,
    // translate from source center. Source storage is never resampled here.
    static Affine placement(double x,double y,double width,double height,int sourceWidth,int sourceHeight,
                            double degrees=0,bool flipX=false,bool flipY=false) {
        for(auto value:{x,y,width,height,degrees}) if(!std::isfinite(value)) throw std::invalid_argument("Invalid layer placement");
        if(sourceWidth<1 || sourceHeight<1 || sourceWidth>30000 || sourceHeight>30000 || width<1 || height<1 ||
           width>300000 || height>300000 || std::abs(x)>1000000 || std::abs(y)>1000000)
            throw std::invalid_argument("Layer placement exceeds geometry limits");
        const double angle=std::remainder(degrees,360)*std::numbers::pi/180,cs=std::cos(angle),sn=std::sin(angle);
        const double sx=width/sourceWidth*(flipX ? -1 : 1),sy=height/sourceHeight*(flipY ? -1 : 1);
        Affine result{cs*sx,sn*sx,-sn*sy,cs*sy,0,0};
        result.tx=x+width/2-result.a*sourceWidth/2-result.c*sourceHeight/2;
        result.ty=y+height/2-result.b*sourceWidth/2-result.d*sourceHeight/2;
        result.inverse(); return result;
    }
};
}
