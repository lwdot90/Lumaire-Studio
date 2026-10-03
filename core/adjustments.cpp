#include "core/editor_commands.h"
#include "core/selection_coverage.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>

namespace compositor::engine {
namespace {
void checkpoint(std::stop_token stop) {
    if(stop.stop_requested()) throw std::runtime_error("Adjustment canceled");
}
void validate(const AdjustmentParameters& parameters) {
    if(!std::isfinite(parameters.value)) throw std::invalid_argument("Nonfinite adjustment parameter");
    double minimum=0,maximum=0;
    switch(parameters.kind) {
        case AdjustmentKind::Exposure:minimum=-8;maximum=8;break;
        case AdjustmentKind::Brightness:minimum=-1;maximum=1;break;
        case AdjustmentKind::Contrast:minimum=-.95;maximum=4;break;
        case AdjustmentKind::Saturation:minimum=0;maximum=2;break;
        default:throw std::invalid_argument("Unknown adjustment kind");
    }
    if(parameters.value<minimum || parameters.value>maximum)
        throw std::invalid_argument("Adjustment parameter exceeds its range");
}
bool neutral(const AdjustmentParameters& parameters) {
    return parameters.value==(parameters.kind==AdjustmentKind::Saturation ? 1 : 0);
}
float boundedCode(float channel,float alpha) {
    if(channel<=0) return 0;
    if(channel>=alpha) return 1;
    return std::clamp(encodeSrgb(channel/alpha),0.f,1.f);
}
Pixel adjusted(Pixel original,const AdjustmentParameters& parameters) {
    if(original.a==0) return {};
    if(parameters.kind==AdjustmentKind::Exposure) {
        const auto scale=static_cast<float>(std::exp2(parameters.value));
        return {original.r*scale,original.g*scale,original.b*scale,original.a};
    }
    std::array<float,3> codes{boundedCode(original.r,original.a),boundedCode(original.g,original.a),boundedCode(original.b,original.a)};
    const float value=static_cast<float>(parameters.value);
    if(parameters.kind==AdjustmentKind::Saturation) {
        const float luminance=.2126f*codes[0]+.7152f*codes[1]+.0722f*codes[2];
        for(auto& channel:codes) channel=std::clamp(luminance+(channel-luminance)*value,0.f,1.f);
    } else {
        for(auto& channel:codes) channel=std::clamp(parameters.kind==AdjustmentKind::Brightness ? channel+value : .5f+(channel-.5f)*(1+value),0.f,1.f);
    }
    return {decodeSrgb(codes[0])*original.a,decodeSrgb(codes[1])*original.a,decodeSrgb(codes[2])*original.a,original.a};
}

}
EditTransaction adjustLayer(DocumentPtr input,const Id& target,TileStore& store,
                            const AdjustmentParameters& parameters,std::stop_token stop) {
    if(!input) throw std::invalid_argument("Adjustment requires a document");
    validate(parameters);checkpoint(stop);
    const auto& layer=input->layer(target);
    if(layer.folder || !layer.raster) throw std::invalid_argument("Adjustment requires a raster layer");
    EditTransaction edit(input,target);
    if(neutral(parameters)) return edit;
    const auto& raster=*layer.raster;
    const auto forward=layer.localToDocument;
    forward.inverse();
    const auto firstX=floorTile(raster.extent.x),lastX=floorTile(raster.extent.x+raster.extent.width-1);
    const auto firstY=floorTile(raster.extent.y),lastY=floorTile(raster.extent.y+raster.extent.height-1);
    const auto defaultPixel=unpack(raster.defaultValue);
    for(auto ty=firstY;;++ty) {
        for(auto tx=firstX;;++tx) {
            checkpoint(stop);
            const TileCoord coordinate{tx,ty};
            const auto found=raster.tiles.find(coordinate);
            // All four adjustments preserve transparent defaults. Do not
            // materialize blank tiles merely because an adjustment was invoked.
            if(found!=raster.tiles.end() || defaultPixel.a!=0) {
                const auto region=tileExtent(raster.extent,coordinate);
                const auto count=static_cast<std::size_t>(region.width)*static_cast<std::size_t>(region.height);
                std::optional<MemoryAdmission::Reservation> scratch;
                if(store.memoryAdmission()) scratch.emplace(store.memoryAdmission()->require(count*sizeof(Pixel)));
                std::vector<Pixel> pixels(count);
                if(scratch) scratch->commit();
                Tile::ReadLease lease;
                if(found!=raster.tiles.end()) lease=found->second->read(stop);
                bool changed=false;
                for(int y=0;y<region.height;++y) {
                    checkpoint(stop);
                    for(int x=0;x<region.width;++x) {
                        const auto original=lease ? lease.linearPixel(x,y) : defaultPixel;
                        const auto amount=selectionCoverage(input->selection,forward,region.x+x,region.y+y,input->width,input->height);
                        auto value=original;
                        if(amount>0 && original.a>0) {
                            const auto proposed=adjusted(original,parameters);
                            if(amount==1) value=proposed;
                            else {
                                value.r=original.r+(proposed.r-original.r)*amount;
                                value.g=original.g+(proposed.g-original.g)*amount;
                                value.b=original.b+(proposed.b-original.b)*amount;
                            }
                        }
                        pixels[static_cast<std::size_t>(y)*static_cast<std::size_t>(region.width)+static_cast<std::size_t>(x)]=value;
                        // Packing validates finite representability before any
                        // output tile is created or admitted to the transaction.
                        changed|=pack(value)!=pack(original);
                    }
                }
                lease={}; // Release source bytes before output tile admission.
                checkpoint(stop);
                if(changed) edit.write(store,coordinate,pixels);
                checkpoint(stop);
            }
            if(tx==lastX) break;
        }
        if(ty==lastY) break;
    }
    checkpoint(stop);
    return edit;
}
}
