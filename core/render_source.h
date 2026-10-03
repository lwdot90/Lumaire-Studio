#pragma once
#include "core/affine.h"
#include "core/raster.h"
#include <map>
#include <optional>
#include <stop_token>
#include <string_view>

namespace compositor::engine {
// These identifiers describe an interface, not implemented conversion kernels.
enum class SourcePrecision { LegacyRGBA16F, RGB8, RGB16, RGB32F, Gray16, CMYK16 };
enum class SourceColorPolicy { LegacyLinearSrgb, VersionedAlternate };
struct SourceDescriptor {
    SourcePrecision precision=SourcePrecision::LegacyRGBA16F;
    SourceColorPolicy policy=SourceColorPolicy::LegacyLinearSrgb;
    std::string policyVersion="legacy-schema-1";
    std::shared_ptr<const SourceProfile> originalProfile;
    bool operator==(const SourceDescriptor&) const = default;
    bool supported() const noexcept;
};

// Immutable source-resolution adapter. Placement never rewrites source pixels.
class RenderSource final {
public:
    explicit RenderSource(std::shared_ptr<const RasterSnapshot> raster,Affine sourceToDocument={},
                          SourceDescriptor descriptor={});
    const std::shared_ptr<const RasterSnapshot>& raster() const {return raster_;}
    const SourceDescriptor& descriptor() const {return descriptor_;}
    const Extent& extent() const {return raster_->extent;}
    const Id& identity() const {return raster_->id;}
    std::int64_t revision() const {return raster_->revision;}
    const Affine& sourceToDocument() const {return forward_;}
    Coordinate documentToSource(Coordinate point) const;
    bool sameOutput(const RenderSource& other) const noexcept;
private:
    std::shared_ptr<const RasterSnapshot> raster_;
    SourceDescriptor descriptor_;
    Affine forward_,inverse_;
};

// Worker-local, bounded to one tile lease. No raw pixel pointer escapes a lease.
// Each operation checks cancellation, including default/transparent reads.
class SourceReadContext final {
public:
    explicit SourceReadContext(std::shared_ptr<const RenderSource> source,std::stop_token stop={});
    Pixel pixel(std::int64_t sourceX,std::int64_t sourceY,std::stop_token stop={});
    std::size_t lookups() const noexcept {return lookups_;}
    bool hasLease() const noexcept {return bool(lease_);}
    void release() noexcept;
private:
    std::shared_ptr<const RenderSource> source_;
    std::stop_token stop_;
    std::optional<TileCoord> coordinate_;
    Extent tileExtent_;
    const Tile* tile_=nullptr;
    std::size_t lookups_=0;
    Tile::ReadLease lease_;
};

enum class RenderNodeKind { Source, IsolatedGroup, FilterStack, EffectStack, AlternateColorPolicy };
enum class RenderReadiness { Ready, UnsupportedSemantics, DependencyUnavailable };
struct RenderNodeDescriptor {
    std::string id;
    std::uint64_t revision=0;
    RenderNodeKind kind=RenderNodeKind::Source;
    std::vector<std::string> dependencies;
    std::shared_ptr<const RenderSource> source;
    // Opaque exact versioned parameters enter invalidation. They never enable
    // an unimplemented filter/group/color operation.
    std::string parameterContract;
    std::vector<std::uint8_t> parameters;
    Extent region;
    std::uint32_t halo=0;
};
// Validates an immutable bounded DAG. Unsupported nodes remain visible and
// propagate unavailable status; they are never silently rendered as identity.
class RenderDependencyGraph final {
public:
    explicit RenderDependencyGraph(std::vector<RenderNodeDescriptor> nodes);
    const RenderNodeDescriptor& node(std::string_view id) const;
    RenderReadiness readiness(std::string_view id) const;
    // Conservative exact dependency comparison, with no lossy hash cache key.
    bool sameOutput(std::string_view id,const RenderDependencyGraph& other) const;
    const std::vector<RenderNodeDescriptor>& nodes() const {return nodes_;}
private:
    std::size_t index(std::string_view id) const;
    std::vector<RenderNodeDescriptor> nodes_;
    std::map<std::string,std::size_t,std::less<>> indices_;
    std::vector<RenderReadiness> readiness_;
};
}
