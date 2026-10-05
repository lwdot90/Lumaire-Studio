#pragma once
#include "core/editing_types.h"
#include "core/raster.h"
#include <stop_token>

namespace compositor::engine {
struct DocumentSnapshot;
class EditTransaction;
enum class RetouchKind {Clone,Heal};
struct RetouchStroke {
    RetouchKind kind=RetouchKind::Clone;
    Coordinate sourceAnchor;
    std::vector<Coordinate> points;
    double diameter=40,hardness=.5,opacity=1,healingRadius=4;
    Affine localToDocument;
    std::optional<Selection> selection;
    int canvasWidth=1,canvasHeight=1;
    bool operator==(const RetouchStroke& other) const;
};
// Source precedes every retained retouch stroke and any layer adjustments.
struct RetouchStack {
    std::shared_ptr<const RasterSnapshot> source;
    std::vector<RetouchStroke> strokes;
    std::uint32_t policy=1;
};
void validateRetouchStack(const RetouchStack&,const RasterSnapshot& rendered);
std::shared_ptr<const RasterSnapshot> evaluateRetouchStack(const RetouchStack&,TileStore&,std::stop_token={});
EditTransaction setRetouchStrokes(std::shared_ptr<const DocumentSnapshot>,const Id&,TileStore&,std::vector<RetouchStroke>,std::stop_token={});
}
