#include "core/retouch.h"
#include "core/layer_stack.h"
#include "core/resampling.h"
#include <functional>
#include <unordered_map>
#include <limits>

namespace compositor::engine {
bool sameRegion(const LayerStack& before,const LayerStack& after,SampleRegion bounds,Coordinate step) {
    bounds.validate();
    if(!std::isfinite(step.x) || !std::isfinite(step.y) || step.x<=0 || step.y<=0)
        throw std::invalid_argument("Invalid region comparison step");
    const auto support=[&](const LayerStack::Prepared& layer) {
        if(layer.sampling==Sampling::Lanczos)
            return reductionSupport(reductionPlan(layer.inverse,step,static_cast<int>(layer.raster->extent.width),static_cast<int>(layer.raster->extent.height)));
        return layer.sampling==Sampling::Bilinear ? Coordinate{.5,.5} : Coordinate{};
    };
    const auto a=before.prepared(),b=after.prepared();
    std::size_t i=0,j=0;
    for(;;) {
        while(i<a.size() && !intersectsSource(a[i].raster->extent,a[i].inverse,bounds,support(a[i]))) ++i;
        while(j<b.size() && !intersectsSource(b[j].raster->extent,b[j].inverse,bounds,support(b[j]))) ++j;
        if(i==a.size() || j==b.size()) return i==a.size() && j==b.size();
        const auto& old=a[i++];const auto& next=b[j++];
        if(old.inverse!=next.inverse || old.sampling!=next.sampling || old.opacity!=next.opacity || old.blend!=next.blend) return false;
        if(old.maskEnabled!=next.maskEnabled) return false;
        if(old.maskEnabled && old.mask!=next.mask) {
            if(!old.mask || !next.mask) return false;
            const auto& oldMask=*old.mask;const auto& nextMask=*next.mask;
            if(oldMask.extent!=nextMask.extent || oldMask.defaultValue!=nextMask.defaultValue) return false;
            const auto maskChanged=[&](const TileMap& from,const TileMap& to) {
                for(const auto& [coordinate,tile]:from) {
                    const auto found=to.find(coordinate);
                    if((found==to.end() || found->second!=tile) &&
                       intersectsSource(tileExtent(oldMask.extent,coordinate),old.inverse,bounds,{.5,.5})) return true;
                }
                return false;
            };
            if(maskChanged(oldMask.tiles,nextMask.tiles) || maskChanged(nextMask.tiles,oldMask.tiles)) return false;
        }
        if(old.raster==next.raster) continue;
        const auto& x=*old.raster;const auto& y=*next.raster;
        if(x.extent!=y.extent || x.defaultValue!=y.defaultValue) return false;
        const auto halo=support(old);
        const auto changed=[&](const TileMap& from,const TileMap& to) {
            for(const auto& [coordinate,tile]:from) {
                const auto found=to.find(coordinate);
                if((found==to.end() || found->second!=tile) &&
                   intersectsSource(tileExtent(x.extent,coordinate),old.inverse,bounds,halo)) return true;
            }
            return false;
        };
        if(changed(x.tiles,y.tiles) || changed(y.tiles,x.tiles)) return false;
    }
}
namespace {
constexpr double infinity=std::numeric_limits<double>::infinity();
double down(double value) { return std::nextafter(value,-infinity); }
double up(double value) { return std::nextafter(value,infinity); }
struct Interval { double low,high; };
Interval scale(double coefficient,double low,double high) {
    if(coefficient<0) std::swap(low,high);
    return {down(coefficient*low),up(coefficient*high)};
}
Interval axis(double a,double b,double translation,SampleRegion bounds) {
    const auto x=scale(a,bounds.left,bounds.right),y=scale(b,bounds.top,bounds.bottom);
    // Outward rounding at each operation also covers cancellation and large
    // signed asset origins. A NaN from overflow cannot prove disjointness.
    return {down(down(x.low+y.low)+translation),up(up(x.high+y.high)+translation)};
}
bool disjoint(Interval range,std::int64_t origin,std::int64_t size,double support) {
    if(std::isnan(range.low) || std::isnan(range.high)) return false;
    const double low=down(down(static_cast<double>(origin))-support);
    const double high=up(up(static_cast<double>(origin+size))+support);
    return range.high<low || range.low>high;
}
}
void SampleRegion::validate() const {
    for(const auto value:{left,top,right,bottom}) if(!std::isfinite(value)) throw std::invalid_argument("Nonfinite sample region");
    if(left>right || top>bottom) throw std::invalid_argument("Inverted sample region");
}
bool SampleRegion::contains(double x,double y) const { return x>=left && x<=right && y>=top && y<=bottom; }
LayerStack::LayerStack(std::vector<LayerNode> nodes):nodes_(std::move(nodes)) {
    if(nodes_.size()>10000) throw std::length_error("Layer count exceeds 10000");
    auto& byId=byId_;byId.reserve(nodes_.size());
    std::unordered_map<std::string,const RasterSnapshot*> assets;
    std::unordered_map<std::string,const RasterSnapshot*> masks;
    std::uint64_t pixels=0,maskPixels=0;
    const auto registerAsset=[&](const std::shared_ptr<const RasterSnapshot>& raster) {
        const auto [found,inserted]=assets.emplace(raster->id.text(),raster.get());
        if(!inserted && found->second!=raster.get()) throw std::invalid_argument("Conflicting versions of one asset ID");
        if(inserted) {
            const auto& extent=raster->extent;
            pixels+=static_cast<std::uint64_t>(extent.width)*static_cast<std::uint64_t>(extent.height);
            if(pixels>100000000) throw std::length_error("Aggregate live raster extents exceed 100 MP");
        }
    };
    for(std::size_t i=0;i<nodes_.size();++i) {
        const auto& node=nodes_[i];
        if(node.name.size()>4096) throw std::invalid_argument("Layer name exceeds limit");
        if(!byId.emplace(node.id.text(),i).second) throw std::invalid_argument("Duplicate layer ID");
        blendIdentifier(node.blend); node.localToDocument.inverse();
        if(node.siblingOrder<0 || !std::isfinite(node.opacity) || node.opacity<0 || node.opacity>1)
            throw std::invalid_argument("Invalid layer order or opacity");
        if(node.sampling!=Sampling::Nearest && node.sampling!=Sampling::Bilinear && node.sampling!=Sampling::Lanczos) throw std::invalid_argument("Unsupported layer sampling");
        if(node.folder) {
            if(node.raster || node.mask || node.adjustments || node.retouch || node.blend!=BlendMode::Normal) throw std::invalid_argument("Folders must be pass-through");
        } else {
            if(!node.raster) throw std::invalid_argument("Raster layer has no asset");
            if(node.adjustments) {
                validateAdjustmentStack(*node.adjustments,*node.raster);
                registerAsset(node.adjustments->source);
            }
            if(node.retouch) {
                validateRetouchStack(*node.retouch,node.adjustments ? *node.adjustments->source : *node.raster);
                registerAsset(node.retouch->source);
            }
            if(node.mask && node.mask->extent!=node.raster->extent)
                throw std::invalid_argument("Linked mask extent must match its raster");
            if(node.mask) {
                const auto& mask=*node.mask;
                if(mask.sourceProfile || mask.defaultValue[0]!=0 || mask.defaultValue[1]!=0 || mask.defaultValue[2]!=0)
                    throw std::invalid_argument("Mask must contain unprofiled alpha coverage");
                const auto alpha=unpack(mask.defaultValue).a;
                if(!std::isfinite(alpha) || alpha<0 || alpha>1) throw std::invalid_argument("Invalid mask default coverage");
                const auto [found,inserted]=masks.emplace(mask.id.text(),node.mask.get());
                if(!inserted && found->second!=node.mask.get()) throw std::invalid_argument("Conflicting versions of one mask ID");
                if(inserted) {
                    maskPixels+=static_cast<std::uint64_t>(mask.extent.width)*static_cast<std::uint64_t>(mask.extent.height);
                    if(maskPixels>100000000) throw std::length_error("Aggregate live mask extents exceed 100 MP");
                }
            }
            registerAsset(node.raster);
        }
    }
    for(const auto& [id,mask]:masks) {
        (void)mask;
        if(assets.contains(id)) throw std::invalid_argument("Mask and color asset IDs must be distinct");
    }
    // Adapt LayerHierarchy.validate/entries: reject cycles and missing/nonfolder
    // parents, then visit bottom-to-top with inherited visibility and opacity. As specified
    // for Linux, child transforms already map directly to document coordinates.
    const auto root=nodes_.size();
    std::vector<std::vector<std::size_t>> children(nodes_.size()+1);
    for(std::size_t i=0;i<nodes_.size();++i) {
        auto parent=nodes_[i].parent;
        std::size_t depth=0,owner=root;
        while(parent) {
            const auto found=byId.find(parent->text());
            if(found==byId.end() || !nodes_[found->second].folder) throw std::invalid_argument("Missing or nonfolder parent");
            if(depth++==0) owner=found->second;
            if(depth>64 || found->second==i) throw std::invalid_argument("Layer hierarchy cycle or depth overflow");
            parent=nodes_[found->second].parent;
        }
        children[owner].push_back(i);
    }
    for(auto& siblings:children) {
        std::sort(siblings.begin(),siblings.end(),[&](std::size_t a,std::size_t b){return nodes_[a].siblingOrder<nodes_[b].siblingOrder;});
        for(std::size_t order=0;order<siblings.size();++order)
            if(nodes_[siblings[order]].siblingOrder!=static_cast<int>(order)) throw std::invalid_argument("Sibling orders must be contiguous and unique");
    }
    std::function<void(std::size_t,bool,float)> visit=[&](std::size_t parent,bool visible,float opacity) {
        for(auto index:children[parent]) {
            const auto& node=nodes_[index]; const bool effective=visible && node.visible;
            const float inherited=opacity*node.opacity;
            if(node.folder) visit(index,effective,inherited);
            else if(effective && inherited>0) visible_.push_back({node.raster,node.localToDocument.inverse(),node.sampling,inherited,node.blend,node.mask,node.maskEnabled});
        }
    };
    visit(root,true,1);
}
const LayerNode& LayerStack::node(const Id& id) const {
    const auto found=byId_.find(id.text());
    if(found==byId_.end()) throw std::invalid_argument("Unknown layer ID");
    return nodes_[found->second];
}
Pixel LayerStack::evaluate(double documentX,double documentY) const {
    return evaluate(visible_,documentX,documentY);
}
Pixel LayerStack::evaluate(std::span<const Prepared> layers,double documentX,double documentY) {
    if(!std::isfinite(documentX) || !std::isfinite(documentY)) throw std::invalid_argument("Invalid document sample coordinate");
    Pixel output{};
    for(const auto& prepared:layers) {
        const auto position=prepared.inverse.map({documentX,documentY});
        auto pixel=sample(*prepared.raster,position.x,position.y,prepared.sampling);
        float coverage=1;
        if(prepared.mask && prepared.maskEnabled) {
            const auto& mask=*prepared.mask;const auto& extent=mask.extent;
            const PixelReader read=[&](auto px,auto py) {
                if(px<extent.x || py<extent.y || px>=extent.x+extent.width || py>=extent.y+extent.height)
                    return Pixel{0,0,0,1};
                return mask.pixel(px,py);
            };
            coverage=std::clamp(sampleFrom(read,position.x,position.y,Sampling::Bilinear).a,0.f,1.f);
        }
        const auto opacity=prepared.opacity*coverage;
        pixel.r*=opacity; pixel.g*=opacity; pixel.b*=opacity; pixel.a*=opacity;
        output=composite(pixel,output,prepared.blend);
    }
    return output;
}
std::vector<LayerStack::Prepared> LayerStack::intersecting(std::span<const Prepared> layers,SampleRegion bounds) {
    bounds.validate(); std::vector<Prepared> result;
    for(const auto& prepared:layers) {
        const auto& m=prepared.inverse; const auto& extent=prepared.raster->extent;
        const double support=prepared.sampling==Sampling::Lanczos ? 3 : prepared.sampling==Sampling::Bilinear ? .5 : 0;
        if(disjoint(axis(m.a,m.c,m.tx,bounds),extent.x,extent.width,support)
            || disjoint(axis(m.b,m.d,m.ty,bounds),extent.y,extent.height,support)) continue;
        result.push_back(prepared);
    }
    return result;
}
LayerRegion LayerStack::region(SampleRegion bounds) const { return {bounds,intersecting(visible_,bounds)}; }
bool intersectsSource(const Extent& extent,const Affine& inverse,SampleRegion bounds,Coordinate support,Coordinate inputOrigin,double inputScale) {
    bounds.validate();extent.validate();
    if(!std::isfinite(support.x) || !std::isfinite(support.y) || support.x<0 || support.y<0)
        throw std::invalid_argument("Invalid source filter support");
    if(!std::isfinite(inputOrigin.x) || !std::isfinite(inputOrigin.y) || !std::isfinite(inputScale) || inputScale<=0)
        throw std::invalid_argument("Invalid source sampling grid");
    auto x=axis(inverse.a,inverse.c,inverse.tx,bounds),y=axis(inverse.b,inverse.d,inverse.ty,bounds);
    if(inputOrigin.x!=0 || inputScale!=1) x={down(down(x.low-inputOrigin.x)/inputScale),up(up(x.high-inputOrigin.x)/inputScale)};
    if(inputOrigin.y!=0 || inputScale!=1) y={down(down(y.low-inputOrigin.y)/inputScale),up(up(y.high-inputOrigin.y)/inputScale)};
    return !disjoint(x,extent.x,extent.width,support.x) && !disjoint(y,extent.y,extent.height,support.y);
}
TileMap requiredSourceTiles(const RasterSnapshot& raster,const Affine& inverse,Sampling sampling,SampleRegion bounds) {
    bounds.validate();inverse.inverse();
    if(sampling!=Sampling::Nearest && sampling!=Sampling::Bilinear) throw std::invalid_argument("Invalid tile coverage sampling");
    const auto x=axis(inverse.a,inverse.c,inverse.tx,bounds),y=axis(inverse.b,inverse.d,inverse.ty,bounds);
    const double support=sampling==Sampling::Bilinear ? .5 : 0;
    TileMap required;
    for(const auto& [coordinate,tile]:raster.tiles) {
        const auto extent=tileExtent(raster.extent,coordinate);
        if(!disjoint(x,extent.x,extent.width,support) && !disjoint(y,extent.y,extent.height,support)) required.emplace(coordinate,tile);
    }
    return required;
}
LayerRegion LayerRegion::region(SampleRegion bounds) const {
    bounds.validate();
    if(!bounds_.contains(bounds.left,bounds.top) || !bounds_.contains(bounds.right,bounds.bottom))
        throw std::invalid_argument("Subregion exceeds prepared sample bounds");
    return {bounds,LayerStack::intersecting(layers_,bounds)};
}
Pixel LayerRegion::evaluate(double x,double y) const {
    if(!bounds_.contains(x,y)) throw std::invalid_argument("Sample exceeds prepared region");
    return LayerStack::evaluate(layers_,x,y);
}
}
