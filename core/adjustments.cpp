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
    double minimum=0,maximum=0;
    switch(parameters.kind) {
        case AdjustmentKind::Exposure:minimum=-8;maximum=8;break;
        case AdjustmentKind::Brightness:minimum=-1;maximum=1;break;
        case AdjustmentKind::Contrast:minimum=-.95;maximum=4;break;
        case AdjustmentKind::Saturation:minimum=0;maximum=2;break;
        case AdjustmentKind::Levels: {
            const auto& levels=parameters.levels;
            if(!std::isfinite(levels.inputBlack) || !std::isfinite(levels.inputWhite) ||
               !std::isfinite(levels.gamma) || !std::isfinite(levels.outputBlack) || !std::isfinite(levels.outputWhite) ||
               levels.inputBlack<0 || levels.inputBlack>=levels.inputWhite || levels.inputWhite>1 ||
               levels.gamma<.1 || levels.gamma>10 || levels.outputBlack<0 ||
               levels.outputBlack>levels.outputWhite || levels.outputWhite>1)
                throw std::invalid_argument("Invalid levels controls");
            return;
        }
        case AdjustmentKind::Curves: {
            const auto& curve=parameters.curve;
            if(curve.size()<2 || curve.size()>16 || curve.front().input!=0 || curve.back().input!=1)
                throw std::invalid_argument("Curve requires 2 to 16 points and input endpoints 0 and 1");
            for(std::size_t i=0;i<curve.size();++i) {
                if(!std::isfinite(curve[i].input) || !std::isfinite(curve[i].output) ||
                   curve[i].output<0 || curve[i].output>1 || (i && curve[i].input<=curve[i-1].input))
                    throw std::invalid_argument("Invalid curve point");
            }
            return;
        }
        case AdjustmentKind::ColorBalance: {
            const auto& balance=parameters.colorBalance;
            if(!std::isfinite(balance.warmth) || !std::isfinite(balance.tint) ||
               balance.warmth<-1 || balance.warmth>1 || balance.tint<-1 || balance.tint>1)
                throw std::invalid_argument("Invalid relative color balance");
            return;
        }
        default:throw std::invalid_argument("Unknown adjustment kind");
    }
    if(!std::isfinite(parameters.value)) throw std::invalid_argument("Nonfinite adjustment parameter");
    if(parameters.value<minimum || parameters.value>maximum)
        throw std::invalid_argument("Adjustment parameter exceeds its range");
}
bool neutral(const AdjustmentParameters& parameters) {
    if(parameters.kind==AdjustmentKind::Levels) {
        const auto& levels=parameters.levels;
        return levels.inputBlack==0 && levels.inputWhite==1 && levels.gamma==1 && levels.outputBlack==0 && levels.outputWhite==1;
    }
    if(parameters.kind==AdjustmentKind::Curves)
        return std::all_of(parameters.curve.begin(),parameters.curve.end(),[](const CurvePoint& point){return point.input==point.output;});
    if(parameters.kind==AdjustmentKind::ColorBalance)
        return parameters.colorBalance.warmth==0 && parameters.colorBalance.tint==0;
    return parameters.value==(parameters.kind==AdjustmentKind::Saturation ? 1 : 0);
}
float boundedCode(float channel,float alpha) {
    if(channel<=0) return 0;
    if(channel>=alpha) return 1;
    return std::clamp(encodeSrgb(channel/alpha),0.f,1.f);
}
struct PreparedAdjustment {
    float exposure=1;
    std::array<float,3> gains{1,1,1};
    double inputRange=1,inverseGamma=1,outputRange=1;
};
PreparedAdjustment prepare(const AdjustmentParameters& parameters) {
    PreparedAdjustment prepared;
    if(parameters.kind==AdjustmentKind::Exposure) prepared.exposure=static_cast<float>(std::exp2(parameters.value));
    if(parameters.kind==AdjustmentKind::ColorBalance) {
        prepared.gains={static_cast<float>(std::exp2(parameters.colorBalance.warmth)),
                        static_cast<float>(std::exp2(-parameters.colorBalance.tint)),
                        static_cast<float>(std::exp2(-parameters.colorBalance.warmth))};
    }
    if(parameters.kind==AdjustmentKind::Levels) {
        prepared.inputRange=parameters.levels.inputWhite-parameters.levels.inputBlack;
        prepared.inverseGamma=1/parameters.levels.gamma;
        prepared.outputRange=parameters.levels.outputWhite-parameters.levels.outputBlack;
    }
    return prepared;
}
Pixel adjusted(Pixel original,const AdjustmentParameters& parameters,const PreparedAdjustment& prepared) {
    if(original.a==0) return {};
    if(parameters.kind==AdjustmentKind::Exposure) {
        const auto scale=prepared.exposure;
        return {original.r*scale,original.g*scale,original.b*scale,original.a};
    }
    if(parameters.kind==AdjustmentKind::ColorBalance)
        return {original.r*prepared.gains[0],original.g*prepared.gains[1],original.b*prepared.gains[2],original.a};
    std::array<float,3> codes{boundedCode(original.r,original.a),boundedCode(original.g,original.a),boundedCode(original.b,original.a)};
    if(parameters.kind==AdjustmentKind::Saturation) {
        const float value=static_cast<float>(parameters.value);
        const float luminance=.2126f*codes[0]+.7152f*codes[1]+.0722f*codes[2];
        for(auto& channel:codes) channel=std::clamp(luminance+(channel-luminance)*value,0.f,1.f);
    } else if(parameters.kind==AdjustmentKind::Levels) {
        for(auto& channel:codes) {
            const auto normalized=std::clamp((static_cast<double>(channel)-parameters.levels.inputBlack)/prepared.inputRange,0.,1.);
            channel=static_cast<float>(parameters.levels.outputBlack+prepared.outputRange*std::pow(normalized,prepared.inverseGamma));
        }
    } else if(parameters.kind==AdjustmentKind::Curves) {
        // At most sixteen validated points; no per-pixel allocation or lookup approximation.
        for(auto& channel:codes) {
            std::size_t upper=1;
            while(upper+1<parameters.curve.size() && channel>parameters.curve[upper].input) ++upper;
            const auto& low=parameters.curve[upper-1];
            const auto& high=parameters.curve[upper];
            const double fraction=(static_cast<double>(channel)-low.input)/(high.input-low.input);
            channel=static_cast<float>(low.output+(high.output-low.output)*fraction);
        }
    } else {
        const float value=static_cast<float>(parameters.value);
        for(auto& channel:codes) channel=std::clamp(parameters.kind==AdjustmentKind::Brightness ? channel+value : .5f+(channel-.5f)*(1+value),0.f,1.f);
    }
    return {decodeSrgb(codes[0])*original.a,decodeSrgb(codes[1])*original.a,decodeSrgb(codes[2])*original.a,original.a};
}

}
bool adjustmentIsNeutral(const AdjustmentParameters& parameters) {
    validate(parameters);
    return neutral(parameters);
}
EditTransaction adjustLayer(DocumentPtr input,const Id& target,TileStore& store,
                            const AdjustmentParameters& parameters,std::stop_token stop) {
    if(!input) throw std::invalid_argument("Adjustment requires a document");
    validate(parameters);checkpoint(stop);
    const auto& layer=input->layer(target);
    if(layer.folder || !layer.raster) throw std::invalid_argument("Adjustment requires a raster layer");
    EditTransaction edit(input,target);
    if(neutral(parameters)) return edit;
    const auto prepared=prepare(parameters);
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
            // These adjustments preserve transparent defaults. Do not
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
                            const auto proposed=adjusted(original,parameters,prepared);
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
