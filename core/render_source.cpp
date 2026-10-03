#include "core/render_source.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <unordered_set>

namespace compositor::engine {
bool SourceDescriptor::supported() const noexcept {
    return precision==SourcePrecision::LegacyRGBA16F && policy==SourceColorPolicy::LegacyLinearSrgb &&
           policyVersion=="legacy-schema-1";
}
RenderSource::RenderSource(std::shared_ptr<const RasterSnapshot> raster,Affine transform,SourceDescriptor descriptor)
    :raster_(std::move(raster)),descriptor_(std::move(descriptor)),forward_(transform),inverse_(transform.inverse()) {
    if(!raster_) throw std::invalid_argument("Render source requires a raster snapshot");
    if(!descriptor_.supported()) throw std::invalid_argument("Render source precision/color policy is unsupported");
    if(descriptor_.originalProfile && descriptor_.originalProfile!=raster_->sourceProfile)
        throw std::invalid_argument("Render source profile differs from immutable raster metadata");
    descriptor_.originalProfile=raster_->sourceProfile;
    raster_->extent.validate();
}
Coordinate RenderSource::documentToSource(Coordinate point) const {
    if(!std::isfinite(point.x) || !std::isfinite(point.y)) throw std::invalid_argument("Nonfinite source coordinate");
    const auto mapped=inverse_.map(point);
    if(!std::isfinite(mapped.x) || !std::isfinite(mapped.y)) throw std::overflow_error("Source coordinate overflow");
    return mapped;
}
bool RenderSource::sameOutput(const RenderSource& other) const noexcept {
    // Pointer identity includes tile maps, defaults, profile and revision even
    // when different immutable snapshots accidentally reuse a textual ID.
    return raster_==other.raster_ && descriptor_==other.descriptor_ && forward_==other.forward_;
}
SourceReadContext::SourceReadContext(std::shared_ptr<const RenderSource> source,std::stop_token stop)
    :source_(std::move(source)),stop_(stop) {
    if(!source_) throw std::invalid_argument("Source reader requires an immutable source");
}
void SourceReadContext::release() noexcept {lease_={};}
Pixel SourceReadContext::pixel(std::int64_t x,std::int64_t y,std::stop_token stop) {
    const auto canceled=[&]{return stop_.stop_requested() || stop.stop_requested();};
    if(canceled()) {release();throw std::runtime_error("Source read canceled");}
    const auto& raster=*source_->raster();
    if(!raster.extent.contains(x,y)) return {};
    const TileCoord coordinate{floorTile(x),floorTile(y)};
    if(!coordinate_ || *coordinate_!=coordinate) {
        release(); // Release before rehydration/admission of the next tile.
        const auto found=raster.tiles.find(coordinate);
        tile_=found==raster.tiles.end() ? nullptr : found->second.get();
        tileExtent_=tileExtent(raster.extent,coordinate);
        coordinate_=coordinate;++lookups_;
    }
    if(!tile_) return unpack(raster.defaultValue);
    if(!lease_) {
        if(stop_.stop_possible() && stop.stop_possible()) {
            std::stop_source combined;
            const auto cancel=[&]{combined.request_stop();};
            std::stop_callback lifetimeStop(stop_,cancel);
            std::stop_callback requestStop(stop,cancel);
            lease_=tile_->read(combined.get_token());
        } else lease_=tile_->read(stop.stop_possible() ? stop : stop_);
    }
    if(canceled()) {release();throw std::runtime_error("Source read canceled");}
    return lease_.linearPixel(static_cast<int>(x-tileExtent_.x),static_cast<int>(y-tileExtent_.y));
}
std::size_t RenderDependencyGraph::index(std::string_view id) const {
    const auto found=indices_.find(id);
    if(found==indices_.end()) throw std::out_of_range("Unknown render dependency");
    return found->second;
}
RenderDependencyGraph::RenderDependencyGraph(std::vector<RenderNodeDescriptor> nodes):nodes_(std::move(nodes)) {
    // Metadata is admitted/bounded by the caller. These independent structural
    // caps also prevent an adversarial graph from exhausting traversal stacks.
    if(nodes_.size()>10000) throw std::length_error("Render graph exceeds node limit");
    auto& byId=indices_;
    std::size_t parameterBytes=0,edges=0;
    for(std::size_t i=0;i<nodes_.size();++i) {
        const auto& n=nodes_[i];
        if(n.id.empty() || n.id.size()>256 || !byId.emplace(n.id,i).second)
            throw std::invalid_argument("Invalid or duplicate render node identity");
        n.region.validate();
        if(n.halo>30000 || n.parameterContract.size()>256 || n.parameters.size()>1024*1024 ||
           n.parameters.size()>16*1024*1024-parameterBytes)
            throw std::length_error("Render node parameters exceed limits");
        parameterBytes+=n.parameters.size();
        if(n.dependencies.size()>10000 || n.dependencies.size()>100000-edges)
            throw std::length_error("Render dependency count exceeds limits");
        edges+=n.dependencies.size();
        if(n.kind==RenderNodeKind::Source) {
            if(!n.source || !n.dependencies.empty() || !n.parameterContract.empty() || !n.parameters.empty() || n.halo ||
               n.region!=n.source->extent()) throw std::invalid_argument("Invalid legacy source node");
        } else if(n.kind==RenderNodeKind::IsolatedGroup || n.kind==RenderNodeKind::FilterStack ||
                  n.kind==RenderNodeKind::EffectStack || n.kind==RenderNodeKind::AlternateColorPolicy) {
            if(n.source || n.parameterContract.empty() || n.dependencies.empty())
                throw std::invalid_argument("Unsupported operation requires explicit contract and dependencies");
        } else throw std::invalid_argument("Unknown render node kind");
    }
    for(const auto& n:nodes_) {
        std::unordered_set<std::string> seen;
        for(const auto& dependency:n.dependencies)
            if(!byId.contains(dependency) || !seen.insert(dependency).second)
                throw std::invalid_argument("Missing or repeated render dependency");
    }
    readiness_.resize(nodes_.size());
    std::vector<unsigned char> visited(nodes_.size());
    std::vector<unsigned> heights(nodes_.size());
    std::function<void(std::size_t,unsigned)> visit=[&](std::size_t i,unsigned depth) {
        if(depth>64) throw std::length_error("Render dependency depth exceeds limit");
        if(visited[i]==1) throw std::invalid_argument("Cyclic render dependencies");
        if(visited[i]==2) return;
        visited[i]=1;
        bool unavailable=false;
        unsigned height=0;
        for(const auto& d:nodes_[i].dependencies) {
            const auto j=byId.at(d); visit(j,depth+1);
            height=std::max(height,heights[j]+1);
            unavailable|=readiness_[j]!=RenderReadiness::Ready;
        }
        if(height>64) throw std::length_error("Render dependency depth exceeds limit");
        heights[i]=height;
        readiness_[i]=unavailable ? RenderReadiness::DependencyUnavailable :
            nodes_[i].kind==RenderNodeKind::Source ? RenderReadiness::Ready : RenderReadiness::UnsupportedSemantics;
        visited[i]=2;
    };
    for(std::size_t i=0;i<nodes_.size();++i) visit(i,0);
}
const RenderNodeDescriptor& RenderDependencyGraph::node(std::string_view id) const {return nodes_[index(id)];}
RenderReadiness RenderDependencyGraph::readiness(std::string_view id) const {return readiness_[index(id)];}
bool RenderDependencyGraph::sameOutput(std::string_view id,const RenderDependencyGraph& other) const {
    // Compare the entire reachable dependency closure once. Graph validation
    // ensures finite acyclic traversal; readiness is part of the output key.
    std::unordered_set<std::string> compared;
    std::function<bool(std::string_view)> same=[&](std::string_view current) {
        const auto leftIndex=index(current);
        const auto found=other.indices_.find(current);
        if(found==other.indices_.end()) return false;
        const auto rightIndex=found->second;
        const auto& a=nodes_[leftIndex]; const auto& b=other.nodes_[rightIndex];
        if(!compared.insert(a.id).second) return true;
        if(a.revision!=b.revision || a.kind!=b.kind || a.dependencies!=b.dependencies ||
           a.parameterContract!=b.parameterContract || a.parameters!=b.parameters || a.region!=b.region ||
           a.halo!=b.halo || readiness_[leftIndex]!=other.readiness_[rightIndex] || bool(a.source)!=bool(b.source)) return false;
        if(a.source && !a.source->sameOutput(*b.source)) return false;
        for(const auto& dependency:a.dependencies) if(!same(dependency)) return false;
        return true;
    };
    return same(id);
}
}
