#include "core/editor_commands.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace compositor::engine {
namespace {
void validatePlacement(const LayerNode& node) {
    node.localToDocument.inverse();
    if(std::abs(node.localToDocument.tx)>1000000 || std::abs(node.localToDocument.ty)>1000000)
        throw std::invalid_argument("Layer placement exceeds geometry limits");
    if(!node.raster) return;
    const auto& extent=node.raster->extent;
    const auto& map=node.localToDocument;
    const auto width=std::hypot(map.a,map.b)*static_cast<double>(extent.width);
    const auto height=std::hypot(map.c,map.d)*static_cast<double>(extent.height);
    if(!std::isfinite(width) || !std::isfinite(height) || width<=0 || height<=0 || width>300000 || height>300000)
        throw std::invalid_argument("Layer size exceeds geometry limits");
    for(const auto x:{extent.x,extent.x+extent.width}) for(const auto y:{extent.y,extent.y+extent.height}) {
        const auto point=map.map({static_cast<double>(x),static_cast<double>(y)});
        if(!std::isfinite(point.x) || !std::isfinite(point.y) || std::abs(point.x)>1000000 || std::abs(point.y)>1000000)
            throw std::invalid_argument("Layer corners exceed geometry limits");
    }
}
void requireDocument(const DocumentPtr& document) {
    if(!document) throw std::invalid_argument("Geometry edit requires a document");
}
}
EditTransaction transformLayer(DocumentPtr document,const Id& id,const TransformParameters& parameters) {
    requireDocument(document);
    for(const auto value:{parameters.dx,parameters.dy,parameters.scaleX,parameters.scaleY,parameters.rotationDegrees})
        if(!std::isfinite(value)) throw std::invalid_argument("Nonfinite transform parameter");
    if(parameters.scaleX<=0 || parameters.scaleY<=0 || std::abs(parameters.dx)>1000000 || std::abs(parameters.dy)>1000000)
        throw std::invalid_argument("Transform parameter exceeds geometry limits");
    const auto& original=document->layer(id);
    if(original.folder || !original.raster) throw std::invalid_argument("Transform requires a raster layer");
    EditTransaction edit(document);
    const double degrees=std::remainder(parameters.rotationDegrees,360);
    if(parameters.dx==0 && parameters.dy==0 && parameters.scaleX==1 && parameters.scaleY==1 && degrees==0)
        return edit;
    auto nodes=document->layers();
    auto& node=*std::find_if(nodes.begin(),nodes.end(),[&](const auto& candidate){return candidate.id==id;});
    const auto& extent=original.raster->extent;
    const Coordinate localCenter{static_cast<double>(extent.x)+static_cast<double>(extent.width)/2,
                                 static_cast<double>(extent.y)+static_cast<double>(extent.height)/2};
    const auto center=original.localToDocument.map(localCenter);
    const double radians=degrees*std::numbers::pi/180;
    const double cs=std::cos(radians),sn=std::sin(radians);
    const auto& old=original.localToDocument;
    Affine next{(cs*old.a-sn*old.b)*parameters.scaleX,(sn*old.a+cs*old.b)*parameters.scaleX,
                (cs*old.c-sn*old.d)*parameters.scaleY,(sn*old.c+cs*old.d)*parameters.scaleY,0,0};
    next.tx=center.x+parameters.dx-next.a*localCenter.x-next.c*localCenter.y;
    next.ty=center.y+parameters.dy-next.b*localCenter.x-next.d*localCenter.y;
    node.localToDocument=next;
    validatePlacement(node);
    edit.setLayers(std::move(nodes));
    return edit;
}
EditTransaction cropDocument(DocumentPtr document,Extent bounds) {
    requireDocument(document);bounds.validate();
    if(bounds.x<0 || bounds.y<0 || bounds.x+bounds.width>document->width || bounds.y+bounds.height>document->height)
        throw std::invalid_argument("Crop must lie within the canvas");
    auto nodes=document->layers();
    for(auto& node:nodes) {
        node.localToDocument.tx-=static_cast<double>(bounds.x);
        node.localToDocument.ty-=static_cast<double>(bounds.y);
        validatePlacement(node);
    }
    EditTransaction edit(document);
    edit.setLayers(std::move(nodes));
    edit.setSelection({});
    edit.setCanvas(static_cast<int>(bounds.width),static_cast<int>(bounds.height));
    return edit;
}
EditTransaction resizeDocument(DocumentPtr document,int width,int height) {
    requireDocument(document);Extent{0,0,width,height}.validate();
    const double sx=static_cast<double>(width)/document->width,sy=static_cast<double>(height)/document->height;
    auto nodes=document->layers();
    for(auto& node:nodes) {
        auto& map=node.localToDocument;
        map.a*=sx;map.c*=sx;map.tx*=sx;
        map.b*=sy;map.d*=sy;map.ty*=sy;
        validatePlacement(node);
    }
    EditTransaction edit(document);
    edit.setLayers(std::move(nodes));
    edit.setSelection({});
    edit.setCanvas(width,height);
    return edit;
}
}
