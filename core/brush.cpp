#include "core/editor_commands.h"
#include "core/selection_coverage.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>

namespace compositor::engine {
namespace {
constexpr std::size_t maximumDabs=262144;
struct Dab {Coordinate document,local;};
void cancelled(std::stop_token stop) {
    if(stop.stop_requested()) throw std::runtime_error("Brush stroke canceled");
}
double falloff(double u) {
    constexpr double k=2.5;
    return std::max(0.,(std::exp(-k*u*u)-std::exp(-k))/(1-std::exp(-k)));
}
double tip(double distance,double radius,double hardness) {
    if(distance>=radius) return 0;
    if(distance<=radius*hardness || hardness==1) return 1;
    return falloff((distance/radius-hardness)/(1-hardness));
}
}
EditTransaction brushStroke(DocumentPtr document,const Id& target,TileStore& store,
    std::span<const Coordinate> points,const BrushSettings& settings,std::stop_token stop) {
    cancelled(stop);
    if(!document || points.size()>8192 || !std::isfinite(settings.diameter) || settings.diameter<1 || settings.diameter>4096 ||
       !std::isfinite(settings.hardness) || settings.hardness<0 || settings.hardness>1 ||
       !std::isfinite(settings.opacity) || settings.opacity<0 || settings.opacity>1)
        throw std::invalid_argument("Brush settings exceed editing limits");
    const auto& layer=document->layer(target);
    const auto source=settings.paintMask ? layer.mask : layer.raster;
    if(!source || (!settings.paintMask && layer.folder)) throw std::invalid_argument("Brush needs a raster target");
    if(settings.paintMask && !layer.maskEnabled) throw std::invalid_argument("Enable mask before painting");
    for(const auto* node=&layer;;) {
        if(!node->visible) throw std::invalid_argument("Show the layer before painting");
        if(!node->parent) break;
        node=&document->layer(*node->parent);
    }
    for(std::size_t i=0;i<points.size();++i) {
        if(i%256==0) cancelled(stop);
        const auto point=points[i];
        if(!std::isfinite(point.x) || !std::isfinite(point.y) || std::abs(point.x)>1000000 || std::abs(point.y)>1000000)
            throw std::invalid_argument("Invalid brush point");
    }
    const auto inverse=layer.localToDocument.inverse();
    const auto color=fromStraightSrgb(settings.color);
    EditTransaction edit(document);
    if(points.empty() || settings.opacity==0) return edit;
    const auto spacing=std::max(.25,settings.diameter*(settings.hardness>=1 ? .015 : .025));
    std::size_t capacity=1;
    for(std::size_t i=1;i<points.size();++i) {
        if(i%256==0) cancelled(stop);
        const auto steps=std::ceil(std::hypot(points[i].x-points[i-1].x,points[i].y-points[i-1].y)/spacing);
        if(steps>static_cast<double>(maximumDabs-capacity)) throw std::length_error("Brush stroke exceeds bounded dab count");
        capacity+=static_cast<std::size_t>(steps);
    }
    const auto memory=store.memoryAdmission();
    std::optional<MemoryAdmission::Reservation> dabCharge;
    if(memory) dabCharge=memory->require(capacity*sizeof(Dab));
    std::vector<Dab> dabs;dabs.reserve(capacity);if(dabCharge) dabCharge->commit();
    const auto append=[&](Coordinate point){dabs.push_back({point,inverse.map(point)});};
    append(points.front());
    double untilNext=spacing;
    for(std::size_t i=1;i<points.size();++i) {
        cancelled(stop);
        const auto start=points[i-1],end=points[i];
        const double length=std::hypot(end.x-start.x,end.y-start.y);
        if(length==0) continue;
        double offset=untilNext;
        while(offset<=length) {
            if(dabs.size()%256==0) cancelled(stop);
            if(dabs.size()>=maximumDabs) throw std::length_error("Brush stroke exceeds bounded dab count");
            const auto fraction=offset/length;
            append({start.x+(end.x-start.x)*fraction,start.y+(end.y-start.y)*fraction});offset+=spacing;
        }
        untilNext=offset-length;
    }
    const double radius=settings.diameter/2;
    const double localRadiusX=radius*std::hypot(inverse.a,inverse.c),localRadiusY=radius*std::hypot(inverse.b,inverse.d);
    double left=dabs.front().local.x,right=left,top=dabs.front().local.y,bottom=top;
    for(std::size_t i=0;i<dabs.size();++i) {
        if(i%256==0) cancelled(stop);
        const auto& dab=dabs[i];
        left=std::min(left,dab.local.x);right=std::max(right,dab.local.x);top=std::min(top,dab.local.y);bottom=std::max(bottom,dab.local.y);
    }
    const auto& extent=source->extent;
    left=std::max(left-localRadiusX-1,static_cast<double>(extent.x));right=std::min(right+localRadiusX+1,static_cast<double>(extent.x+extent.width));
    top=std::max(top-localRadiusY-1,static_cast<double>(extent.y));bottom=std::min(bottom+localRadiusY+1,static_cast<double>(extent.y+extent.height));
    if(left>=right || top>=bottom) return edit;
    TileMap maskTiles=settings.paintMask ? source->tiles : TileMap{};
    bool maskChanged=false;
    if(!settings.paintMask) edit=EditTransaction(document,target);
    const auto firstX=floorTile(static_cast<std::int64_t>(std::floor(left))),lastX=floorTile(static_cast<std::int64_t>(std::ceil(right))-1);
    const auto firstY=floorTile(static_cast<std::int64_t>(std::floor(top))),lastY=floorTile(static_cast<std::int64_t>(std::ceil(bottom))-1);
    for(auto ty=firstY;ty<=lastY;++ty) for(auto tx=firstX;tx<=lastX;++tx) {
        cancelled(stop);
        const TileCoord coordinate{tx,ty};const auto region=tileExtent(extent,coordinate);
        const double regionLeft=static_cast<double>(region.x),regionTop=static_cast<double>(region.y);
        const double regionRight=static_cast<double>(region.x+region.width),regionBottom=static_cast<double>(region.y+region.height);
        const auto count=static_cast<std::size_t>(region.width)*static_cast<std::size_t>(region.height);
        std::optional<MemoryAdmission::Reservation> scratch;
        if(memory) scratch=memory->require(count*(sizeof(float)+sizeof(Pixel)));
        std::vector<float> coverage(count,0);std::vector<Pixel> output(count);if(scratch) scratch->commit();
        for(std::size_t i=0;i<dabs.size();++i) {
            if(i%256==0) cancelled(stop);
            const auto& dab=dabs[i];
            if(dab.local.x+localRadiusX+1<regionLeft || dab.local.x-localRadiusX-1>regionRight ||
               dab.local.y+localRadiusY+1<regionTop || dab.local.y-localRadiusY-1>regionBottom) continue;
            const int x0=static_cast<int>(std::max(0.,std::floor(dab.local.x-localRadiusX-1-regionLeft)));
            const int x1=static_cast<int>(std::min(static_cast<double>(region.width),std::ceil(dab.local.x+localRadiusX+1-regionLeft)));
            const int y0=static_cast<int>(std::max(0.,std::floor(dab.local.y-localRadiusY-1-regionTop)));
            const int y1=static_cast<int>(std::min(static_cast<double>(region.height),std::ceil(dab.local.y+localRadiusY+1-regionTop)));
            for(int y=y0;y<y1;++y) {
                cancelled(stop);
                for(int x=x0;x<x1;++x) {
                    double amount=0;
                    // Pixel-footprint integration also handles rotated/nonuniform placements.
                    for(int sy=0;sy<2;++sy) for(int sx=0;sx<2;++sx) {
                        const auto sample=layer.localToDocument.map({static_cast<double>(region.x+x)+.25+.5*sx,static_cast<double>(region.y+y)+.25+.5*sy});
                        amount+=tip(std::hypot(sample.x-dab.document.x,sample.y-dab.document.y),radius,settings.hardness)/4;
                    }
                    auto& old=coverage[static_cast<std::size_t>(y)*static_cast<std::size_t>(region.width)+static_cast<std::size_t>(x)];
                    old=static_cast<float>(settings.hardness>=1 ? std::max(static_cast<double>(old),amount) : 1-(1-old)*(1-amount));
                }
            }
        }
        const auto original=source->tiles.find(coordinate);
        std::optional<Tile::ReadLease> lease;
        if(original!=source->tiles.end()) lease=original->second->read(stop);
        bool changed=false;
        for(int y=0;y<region.height;++y) {
            cancelled(stop);
            for(int x=0;x<region.width;++x) {
                const auto index=static_cast<std::size_t>(y)*static_cast<std::size_t>(region.width)+static_cast<std::size_t>(x);
                const auto base=lease ? lease->pixel(x,y) : source->defaultValue;
                const auto value=unpack(base);
                const float selected=coverage[index]>0 ? selectionCoverage(document->selection,layer.localToDocument,region.x+x,region.y+y,document->width,document->height) : 0;
                const float amount=coverage[index]*static_cast<float>(settings.opacity)*selected;
                Pixel result=value;
                if(amount>0) {
                    if(settings.paintMask) {
                        const float gray=std::clamp(settings.color.r,0.f,1.f);
                        const float alpha=settings.erasing ? value.a*(1-amount) : value.a*(1-amount)+gray*amount;
                        result={0,0,0,alpha};
                    } else if(settings.erasing) result={value.r*(1-amount),value.g*(1-amount),value.b*(1-amount),value.a*(1-amount)};
                    else result=sourceOver({color.r*amount,color.g*amount,color.b*amount,color.a*amount},value);
                }
                output[index]=result;changed=changed || pack(result)!=base;
            }
        }
        lease.reset();
        if(!changed) continue;
        auto replacement=store.create(static_cast<int>(region.width),static_cast<int>(region.height),output);
        if(settings.paintMask) {
            maskChanged=true;
            if(replacement->uniform() && replacement->pixel(0,0)==source->defaultValue) maskTiles.erase(coordinate);
            else maskTiles.insert_or_assign(coordinate,std::move(replacement));
        } else edit.replace(coordinate,std::move(replacement));
    }
    cancelled(stop);
    if(maskChanged) {
        if(source->revision==std::numeric_limits<std::int64_t>::max()) throw std::overflow_error("Mask revision exhausted");
        auto nodes=document->layers();
        const bool shared=std::any_of(nodes.begin(),nodes.end(),[&](const LayerNode& node){return node.id!=target && node.mask && node.mask->id==source->id;});
        const auto assetId=shared ? Id::generate() : source->id;
        for(auto& node:nodes) if(node.id==target) node.mask=std::make_shared<const RasterSnapshot>(assetId,source->extent,source->defaultValue,std::move(maskTiles),source->revision+1,source->sourceProfile);
        edit.setLayers(std::move(nodes));
    }
    return edit;
}
}
