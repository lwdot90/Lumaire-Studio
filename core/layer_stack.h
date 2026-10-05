#pragma once
#include "core/affine.h"
#include "core/blend.h"
#include "core/sampling.h"
#include "core/adjustment_stack.h"
#include <optional>
#include <unordered_map>

namespace compositor::engine {
// Closed document-space sample bounds, not pixel-edge ownership. Adjacent
// regions may overlap for planning; each output pixel is still written once.
struct SampleRegion {
    double left,top,right,bottom;
    void validate() const;
    bool contains(double x,double y) const;
};
class LayerRegion;
struct RetouchStack;
struct LayerNode {
    Id id;
    std::optional<Id> parent;
    int siblingOrder=0;
    bool folder=false,visible=true;
    float opacity=1;
    BlendMode blend=BlendMode::Normal;
    Affine localToDocument;
    Sampling sampling=Sampling::Bilinear;
    std::shared_ptr<const RasterSnapshot> raster;
    std::string name="Layer";
    std::shared_ptr<const RasterSnapshot> mask;
    bool maskEnabled=true;
    std::shared_ptr<const AdjustmentStack> adjustments;
    std::shared_ptr<const RetouchStack> retouch;
    bool operator==(const LayerNode&) const = default;
};
// Validated immutable render input. This is the reference layer-stack evaluator,
// also owned by DocumentSnapshot: one canonical graph for edits and rendering.
class LayerStack {
public:
    explicit LayerStack(std::vector<LayerNode> nodes);
    const std::vector<LayerNode>& nodes() const { return nodes_; }
    const LayerNode& node(const Id& id) const;
    Pixel evaluate(double documentX,double documentY) const;
    LayerRegion region(SampleRegion bounds) const;
    std::size_t visibleRasterCount() const { return visible_.size(); }
    // Immutable render plan shared by CPU and GPU consumers. Opacity includes
    // all pass-through ancestors; folders never render an isolated intermediate.
    struct Prepared {
        std::shared_ptr<const RasterSnapshot> raster;
        Affine inverse;
        Sampling sampling;
        float opacity;
        BlendMode blend;
        std::shared_ptr<const RasterSnapshot> mask;
        bool maskEnabled=true;
    };
    std::span<const Prepared> prepared() const {return visible_;}
private:
    friend class LayerRegion;
    static Pixel evaluate(std::span<const Prepared> layers,double x,double y);
    static std::vector<Prepared> intersecting(std::span<const Prepared> layers,SampleRegion bounds);
    const std::vector<LayerNode> nodes_;
    std::unordered_map<std::string,std::size_t> byId_;
    std::vector<Prepared> visible_;
};
// Owns immutable asset references, so a queued region remains valid after its
// originating document/stack is released. No full-image or tile copies.
class LayerRegion {
public:
    Pixel evaluate(double documentX,double documentY) const;
    LayerRegion region(SampleRegion bounds) const;
    std::size_t layerCount() const { return layers_.size(); }
    std::span<const LayerStack::Prepared> layers() const { return layers_; }
private:
    friend class LayerStack;
    LayerRegion(SampleRegion bounds,std::vector<LayerStack::Prepared> layers)
        :bounds_(bounds),layers_(std::move(layers)) {}
    SampleRegion bounds_;
    std::vector<LayerStack::Prepared> layers_;
};
// Conservative source coverage including neighbor taps; never reads pixels.
TileMap requiredSourceTiles(const RasterSnapshot& raster,const Affine& inverse,
                           Sampling sampling,SampleRegion bounds);
// Conservative shared coverage primitive. Support is measured in source
// pixels, independently per axis, and includes the entire filter/mip halo.
bool intersectsSource(const Extent& extent,const Affine& inverse,SampleRegion bounds,Coordinate support,
                      Coordinate inputOrigin={},double inputScale=1);
// Proves equal pixels without reading pixels. False means potentially dirty.
// Includes both old/new coverage and complete direct/mip filter support.
bool sameRegion(const LayerStack& before,const LayerStack& after,SampleRegion bounds,Coordinate step);
}
