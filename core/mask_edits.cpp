#include "core/editor_commands.h"
#include "core/selection_coverage.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <stop_token>
#include <vector>

namespace compositor::engine {
namespace {
void canceled(std::stop_token stop) {
    if(stop.stop_requested()) throw io::SpillError(io::SpillErrorCode::Cancelled,"Mask creation canceled");
}
const LayerNode& rasterLayer(const DocumentPtr& input,const Id& id) {
    if(!input) throw std::invalid_argument("Mask editing requires a document");
    const auto& layer=input->layer(id);
    if(layer.folder || !layer.raster) throw std::invalid_argument("Folder masks are not supported");
    return layer;
}
EditTransaction replaceMask(const DocumentPtr& input,const Id& target,std::shared_ptr<const RasterSnapshot> mask,bool enabled) {
    auto layers=input->layers();
    for(auto& layer:layers) if(layer.id==target) {layer.mask=std::move(mask);layer.maskEnabled=enabled;break;}
    EditTransaction edit(input);edit.setLayers(std::move(layers));return edit;
}
std::int64_t bounded(double value,std::int64_t lower,std::int64_t upper) {
    if(!std::isfinite(value)) throw std::invalid_argument("Mask selection transform overflow");
    if(value<=static_cast<double>(lower)) return lower;
    if(value>=static_cast<double>(upper)) return upper;
    return static_cast<std::int64_t>(value);
}
struct Roi {std::int64_t left=0,top=0,right=0,bottom=0;};
Roi sourceRoi(const Extent& source,const Selection& selection,const Affine& transform) {
    const auto& box=selection.bounds;
    if(!box.width || !box.height) return {};
    const auto inverse=transform.inverse();
    double paddingX=selection.featherRadius,paddingY=paddingX;
    if(selection.shape==SelectionShape::Ellipse && paddingX>0) {
        const double minimum=static_cast<double>(std::min(box.width,box.height));
        paddingX*=static_cast<double>(box.width)/minimum;
        paddingY*=static_cast<double>(box.height)/minimum;
    }
    const double left=static_cast<double>(box.x)-paddingX,top=static_cast<double>(box.y)-paddingY;
    const double right=static_cast<double>(box.x+box.width)+paddingX,bottom=static_cast<double>(box.y+box.height)+paddingY;
    double minX=std::numeric_limits<double>::infinity(),minY=minX,maxX=-minX,maxY=-minX;
    for(const auto point:std::array{Coordinate{left,top},Coordinate{right,top},Coordinate{left,bottom},Coordinate{right,bottom}}) {
        const auto mapped=inverse.map(point);
        minX=std::min(minX,mapped.x);minY=std::min(minY,mapped.y);maxX=std::max(maxX,mapped.x);maxY=std::max(maxY,mapped.y);
    }
    // Conservative pixel-center support; extra boundary pixels are checked by
    // the independent document-space membership predicate below.
    return {bounded(std::floor(minX)-1,source.x,source.x+source.width),
        bounded(std::floor(minY)-1,source.y,source.y+source.height),
        bounded(std::ceil(maxX)+1,source.x,source.x+source.width),
        bounded(std::ceil(maxY)+1,source.y,source.y+source.height)};
}
}
EditTransaction createLayerMask(DocumentPtr input,const Id& target,TileStore& store,bool fromSelection,std::stop_token stop) {
    canceled(stop);const auto& layer=rasterLayer(input,target);
    if(layer.mask) throw std::invalid_argument("Layer already has a mask");
    if(fromSelection && !input->selection) throw std::invalid_argument("Create mask from selection requires an active selection");
    const auto extent=layer.raster->extent;
    const auto selection=fromSelection ? input->selection : std::optional<Selection>{};
    if(selection) selection->validate(input->width,input->height);
    // Selection coverage is constrained to the document canvas, even when
    // inverted. Transparent is therefore the only safe sparse default for
    // source pixels whose linked placement lies outside that canvas.
    const bool defaultWhite=!selection;
    const auto defaultValue=pack(Pixel{0,0,0,defaultWhite?1.f:0.f});
    TileMap tiles;
    std::shared_ptr<MemoryAdmission::Reservation> metadata;
    Roi roi;
    std::size_t maximumTiles=0;
    if(selection) {
        const auto roiSelection=selection->inverted
            ? Selection{Extent{0,0,input->width,input->height},SelectionShape::Rectangle,false}
            : *selection;
        roi=sourceRoi(extent,roiSelection,layer.localToDocument);
        if(roi.left<roi.right && roi.top<roi.bottom) {
            const auto width=static_cast<std::uint64_t>(floorTile(roi.right-1)-floorTile(roi.left)+1);
            const auto height=static_cast<std::uint64_t>(floorTile(roi.bottom-1)-floorTile(roi.top)+1);
            if(height && width>std::numeric_limits<std::size_t>::max()/height) throw std::length_error("Mask tile bounds overflow");
            maximumTiles=static_cast<std::size_t>(width*height);
        }
    }
    if(const auto memory=store.memoryAdmission()) {
        constexpr auto nodeAllowance=sizeof(TileMap::value_type)+128;
        if(maximumTiles>(std::numeric_limits<std::uint64_t>::max()-sizeof(RasterSnapshot)-256)/nodeAllowance)
            throw std::length_error("Mask metadata bounds overflow");
        metadata=std::make_shared<MemoryAdmission::Reservation>(memory->require(maximumTiles*nodeAllowance+sizeof(RasterSnapshot)+256));
        metadata->commit();
    }
    canceled(stop);
    if(selection && maximumTiles) {
        std::optional<MemoryAdmission::Reservation> scratch;
        if(const auto memory=store.memoryAdmission()) scratch=memory->require(tileSide*tileSide*sizeof(Pixel));
        canceled(stop);
        std::vector<Pixel> pixels;pixels.reserve(tileSide*tileSide);if(scratch) scratch->commit();
        const auto firstX=floorTile(roi.left),lastX=floorTile(roi.right-1);
        const auto firstY=floorTile(roi.top),lastY=floorTile(roi.bottom-1);
        for(auto ty=firstY;ty<=lastY;++ty) for(auto tx=firstX;tx<=lastX;++tx) {
            canceled(stop);const TileCoord coordinate{tx,ty};const auto tileBounds=tileExtent(extent,coordinate);
            pixels.resize(static_cast<std::size_t>(tileBounds.width*tileBounds.height));bool differs=false;
            for(int y=0;y<tileBounds.height;++y) {
                canceled(stop);
                for(int x=0;x<tileBounds.width;++x) {
                    const auto coverage=selectionCoverage(selection,layer.localToDocument,tileBounds.x+x,tileBounds.y+y,input->width,input->height);
                    pixels[static_cast<std::size_t>(y*tileBounds.width+x)]={0,0,0,coverage};
                    differs=differs || coverage!=(defaultWhite?1.f:0.f);
                }
            }
            if(differs) tiles.emplace(coordinate,store.create(static_cast<int>(tileBounds.width),static_cast<int>(tileBounds.height),pixels));
        }
    }
    canceled(stop);
    auto mask=std::shared_ptr<const RasterSnapshot>(new RasterSnapshot(Id::generate(),extent,defaultValue,std::move(tiles)),
        [metadata](const RasterSnapshot* value) {delete value;});
    canceled(stop);
    auto command=replaceMask(input,target,std::move(mask),true);
    canceled(stop);return command;
}
EditTransaction removeLayerMask(DocumentPtr input,const Id& target) {
    const auto& layer=rasterLayer(input,target);
    if(!layer.mask) return EditTransaction(input);
    return replaceMask(input,target,{},true);
}
EditTransaction setLayerMaskEnabled(DocumentPtr input,const Id& target,bool enabled) {
    const auto& layer=rasterLayer(input,target);
    if(!layer.mask) throw std::invalid_argument("Layer has no mask");
    if(layer.maskEnabled==enabled) return EditTransaction(input);
    return replaceMask(input,target,layer.mask,enabled);
}
}
