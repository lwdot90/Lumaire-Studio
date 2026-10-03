#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <time.h>

namespace compositor {
struct Point { double x=0, y=0; };
struct Viewport {
    // Adapted from Compositor/Rendering/CanvasViewport.swift. Zoom 1 means
    // actual display pixels. Store the center document point instead of pan;
    // this preserves the original center-on-resize behavior algebraically.
    double width=1, height=1, dpr=1, zoom=1;
    Point center{512,512};
    bool followsFit=true;
    double documentWidth=1024,documentHeight=1024;
    double pointsPerPixel() const { return zoom/dpr; }
    Point toDocument(Point p) const { return {(p.x-width/2)/pointsPerPixel()+center.x,(p.y-height/2)/pointsPerPixel()+center.y}; }
    Point toLogical(Point p) const { return {(p.x-center.x)*pointsPerPixel()+width/2,(p.y-center.y)*pointsPerPixel()+height/2}; }
    Point toDevice(Point p) const { auto q=toLogical(p); return {q.x*dpr,q.y*dpr}; }
    Point pixelOrigin() const { return toDocument({.5/dpr,.5/dpr}); }
    // Shared absolute device-pixel grid for CPU, GPU and split render blocks.
    Point documentPixel(int x,int y) const {
        const auto origin=pixelOrigin();const double step=1/zoom;
        return {origin.x+x*step,origin.y+y*step};
    }
    void zoomAt(Point anchor,double factor) {
        if(!std::isfinite(factor) || factor<=0) return;
        const auto before=toDocument(anchor);
        zoom=std::clamp(zoom*factor,.01,64.0);
        const auto after=toDocument(anchor);
        center.x+=before.x-after.x; center.y+=before.y-after.y;
        followsFit=false;
    }
    void pan(Point delta) { center.x-=delta.x/pointsPerPixel(); center.y-=delta.y/pointsPerPixel(); followsFit=false; }
    void fit() {
        followsFit=true;
        if(width<=0 || height<=0) return;
        center={documentWidth/2,documentHeight/2};
        zoom=std::clamp(std::min(std::max(1.0,width-96)/documentWidth,std::max(1.0,height-96)/documentHeight)*dpr,.01,64.0);
    }
    void resize(double w,double h,double scale) {
        width=w; height=h; dpr=std::max(1.0,scale);
        if(followsFit) fit();
    }
};
inline std::int64_t monotonicNs() {
    timespec t{}; clock_gettime(CLOCK_MONOTONIC_RAW,&t);
    return static_cast<std::int64_t>(t.tv_sec)*1000000000+t.tv_nsec;
}
}
