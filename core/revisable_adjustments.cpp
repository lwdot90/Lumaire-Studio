#include "core/editor_commands.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace compositor::engine {
namespace {
void checkpoint(std::stop_token stop) {
    if(stop.stop_requested()) throw std::runtime_error("Revisable adjustment canceled");
}
AdjustmentParameters canonical(const AdjustmentParameters& input) {
    // Only controls belonging to the selected operation enter its saved state.
    AdjustmentParameters result;
    result.kind=input.kind;
    switch(input.kind) {
        case AdjustmentKind::Exposure:
        case AdjustmentKind::Brightness:
        case AdjustmentKind::Contrast:
        case AdjustmentKind::Saturation:result.value=input.value;break;
        case AdjustmentKind::Levels:result.levels=input.levels;break;
        case AdjustmentKind::Curves:result.curve=input.curve;result.channelCurves=input.channelCurves;break;
        case AdjustmentKind::ColorBalance:result.colorBalance=input.colorBalance;break;
        default:throw std::invalid_argument("Unknown revisable adjustment kind");
    }
    adjustmentIsNeutral(result); // Validate meaningful controls without reading pixels.
    return result;
}
std::vector<AdjustmentParameters> canonicalOperations(const std::vector<AdjustmentParameters>& operations) {
    if(operations.size()>16) throw std::length_error("Adjustment stack exceeds 16 operations");
    std::vector<AdjustmentParameters> result;
    result.reserve(operations.size());
    for(const auto& operation:operations) result.push_back(canonical(operation));
    return result;
}
bool compatibleProfiles(const std::shared_ptr<const SourceProfile>& a,const std::shared_ptr<const SourceProfile>& b) {
    return a==b || (a && b && a->id==b->id && a->bytes==b->bytes);
}
}
void validateAdjustmentStack(const AdjustmentStack& stack,const RasterSnapshot& rendered) {
    if(stack.policy!=1 && stack.policy!=2) throw std::invalid_argument("Unsupported adjustment stack policy");
    if(!stack.source) throw std::invalid_argument("Adjustment stack has no retained source");
    if(stack.source->extent!=rendered.extent || !compatibleProfiles(stack.source->sourceProfile,rendered.sourceProfile))
        throw std::invalid_argument("Adjustment source and cache metadata differ");
    if(stack.operations.empty() || stack.operations.size()>16) throw std::length_error("Adjustment stack requires 1 to 16 operations");
    for(const auto& operation:stack.operations) {
        adjustmentIsNeutral(operation);
        if(stack.policy==1) for(const auto& curve:operation.channelCurves) {
            if(curve.size()<2 || curve.size()>16 || curve.front().input!=0 || curve.back().input!=1 ||
               !std::all_of(curve.begin(),curve.end(),[](const CurvePoint& point){return std::isfinite(point.input) && point.input==point.output;}))
                throw std::invalid_argument("Legacy adjustment policy cannot contain channel corrections");
            for(std::size_t i=1;i<curve.size();++i)
                if(curve[i].input<=curve[i-1].input) throw std::invalid_argument("Invalid legacy identity channel curve");
        }
    }
}
std::shared_ptr<const RasterSnapshot> evaluateAdjustmentStack(const AdjustmentStack& stack,TileStore& store,std::stop_token stop) {
    if(!stack.source) throw std::invalid_argument("Adjustment stack has no retained source");
    if(stack.operations.empty()) {
        if(stack.policy!=1 && stack.policy!=2) throw std::invalid_argument("Unsupported adjustment stack policy");
        checkpoint(stop);return stack.source;
    }
    validateAdjustmentStack(stack,*stack.source);checkpoint(stop);
    const auto operations=canonicalOperations(stack.operations);
    auto output=stack.source;
    const auto layerId=Id::generate();
    // No selection, mask or placement can scope retained whole-source operations.
    auto document=std::make_shared<const DocumentSnapshot>(Id::generate(),layerId,
        static_cast<int>(output->extent.width),static_cast<int>(output->extent.height),72,output,output->revision);
    for(const auto& operation:operations) {
        checkpoint(stop);
        if(adjustmentIsNeutral(operation)) continue;
        if(document->revision==std::numeric_limits<std::int64_t>::max())
            throw std::overflow_error("Adjustment evaluation revision exhausted");
        document=adjustLayer(document,layerId,store,operation,stop).finish(document->revision+1);
    }
    checkpoint(stop);
    output=document->singleLayer().raster;
    // The retained source and derived pixels are distinct live assets even
    // though the legacy destructive command preserves its asset identifier.
    return std::make_shared<const RasterSnapshot>(Id::generate(),output->extent,output->defaultValue,
        output->tiles,output->revision,output->sourceProfile);
}
EditTransaction setRevisableAdjustments(DocumentPtr input,const Id& target,TileStore& store,
                                      std::vector<AdjustmentParameters> operations,std::stop_token stop) {
    if(!input) throw std::invalid_argument("Revisable adjustment requires a document");
    checkpoint(stop);
    const auto& original=input->layer(target);
    if(original.folder || !original.raster) throw std::invalid_argument("Revisable adjustment requires a raster layer");
    operations=canonicalOperations(operations);
    EditTransaction edit(input);
    if(operations.empty() && !original.adjustments) return edit;
    if(original.adjustments && canonicalOperations(original.adjustments->operations)==operations) return edit;
    auto nodes=input->layers();
    for(auto& node:nodes) if(node.id==target) {
        if(operations.empty()) {
            node.raster=original.adjustments->source;node.adjustments.reset();
        } else {
            auto stack=std::make_shared<AdjustmentStack>();
            stack->policy=2;
            stack->source=original.adjustments ? original.adjustments->source : original.raster;
            stack->operations=std::move(operations);
            node.raster=evaluateAdjustmentStack(*stack,store,stop);
            node.adjustments=std::move(stack);
        }
        break;
    }
    checkpoint(stop);edit.setLayers(std::move(nodes));return edit;
}
EditTransaction rasterizeLayerAdjustments(DocumentPtr input,const Id& target) {
    if(!input) throw std::invalid_argument("Rasterize requires a document");
    const auto& original=input->layer(target);
    if(original.folder || !original.raster) throw std::invalid_argument("Rasterize requires a raster layer");
    EditTransaction edit(input);
    if(!original.adjustments && !original.retouch) return edit;
    auto nodes=input->layers();
    for(auto& node:nodes) if(node.id==target) {
        const auto& raster=*node.raster;
        node.raster=std::make_shared<const RasterSnapshot>(Id::generate(),raster.extent,raster.defaultValue,
            raster.tiles,raster.revision,raster.sourceProfile);
        node.adjustments.reset();node.retouch.reset();break;
    }
    edit.setLayers(std::move(nodes));return edit;
}
}
