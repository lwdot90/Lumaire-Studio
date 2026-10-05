#include "core/editor_commands.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace compositor;
using namespace compositor::engine;
namespace {
void expect(bool value,const char* message) {if(!value) throw std::runtime_error(message);}
template<class F> void rejects(F operation) {
    bool rejected=false;try {operation();} catch(const std::exception&) {rejected=true;}
    expect(rejected,"Invalid channel curve was accepted");
}
DocumentPtr fixture(TileStore& store,std::span<const Pixel> pixels,Extent extent={0,0,1,1}) {
    TileMap tiles;const auto coordinate=TileCoord{floorTile(extent.x),floorTile(extent.y)};
    tiles.emplace(coordinate,store.create(static_cast<int>(extent.width),static_cast<int>(extent.height),pixels));
    auto raster=std::make_shared<const RasterSnapshot>(Id::generate(),extent,PackedPixel{},std::move(tiles));
    return std::make_shared<const DocumentSnapshot>(Id::generate(),Id::generate(),static_cast<int>(extent.width),static_cast<int>(extent.height),72,raster);
}
double encode(double value) {return value<=.0031308 ? 12.92*value : 1.055*std::pow(value,1/2.4)-.055;}
double decode(double value) {return value<=.04045 ? value/12.92 : std::pow((value+.055)/1.055,2.4);}
double map(double value,const std::vector<CurvePoint>& curve) {
    for(std::size_t i=1;i<curve.size();++i) if(value<=curve[i].input || i+1==curve.size()) {
        const auto& a=curve[i-1];const auto& b=curve[i];
        return a.output+(b.output-a.output)*(value-a.input)/(b.input-a.input);
    }
    throw std::logic_error("Unreachable mapping");
}
AdjustmentParameters correction() {
    AdjustmentParameters p;p.kind=AdjustmentKind::Curves;p.curve={{0,0},{.25,.5},{1,1}};
    p.channelCurves[0]={{0,0},{.5,.8},{1,1}};
    p.channelCurves[1]={{0,0},{.5,.2},{1,1}};
    p.channelCurves[2]={{0,0},{.5,.65},{1,1}};
    return p;
}
void analyticOrderAndAlpha() {
    TileStore store(4*1024*1024);
    const std::array pixels{fromStraightSrgb({.25f,.25f,.25f,.5f}),Pixel{}};
    auto input=fixture(store,pixels,{0,0,2,1});const auto p=correction();
    auto result=adjustLayer(input,input->singleLayer().id,store,p).finish(1);
    const auto original=input->singleLayer().raster->pixel(0,0),actual=result->singleLayer().raster->pixel(0,0);
    const std::array linear{original.r,original.g,original.b},out{actual.r,actual.g,actual.b};
    for(std::size_t i=0;i<3;++i) {
        const double code=encode(linear[i]/original.a);
        const double expected=decode(map(map(code,p.curve),p.channelCurves[i]))*original.a;
        expect(std::abs(out[i]-expected)<.0006,"Channel output differs from independent master-then-channel mapping");
    }
    const double reversed=decode(map(map(encode(original.r/original.a),p.channelCurves[0]),p.curve))*original.a;
    expect(std::abs(actual.r-reversed)>.03,"Master/channel order was reversed");
    expect(actual.a==original.a && pack(result->singleLayer().raster->pixel(1,0))==PackedPixel{},"Channel curves changed alpha/transparent pixel");
    expect(pack(input->singleLayer().raster->pixel(0,0))==pack(pixels[0]),"Channel correction mutated source");
}
void legacyExactAndNeutralHdr() {
    TileStore store(4*1024*1024);
    const std::array pixels{fromStraightSrgb({.13f,.47f,.83f,.625f}),Pixel{2,-.25f,.125f,.5f}};
    auto input=fixture(store,pixels,{0,0,2,1});const auto id=input->singleLayer().id;
    AdjustmentParameters p;p.kind=AdjustmentKind::Curves;p.curve={{0,0},{.25,.18},{.5,.5},{.75,.82},{1,1}};
    auto output=adjustLayer(input,id,store,p).finish(1);
    for(int x=0;x<2;++x) {
        const auto original=input->singleLayer().raster->pixel(x,0);
        std::array codes{original.r,original.g,original.b};
        for(auto& code:codes) {
            code=code<=0 ? 0 : code>=original.a ? 1 : std::clamp(encodeSrgb(code/original.a),0.f,1.f);
            std::size_t upper=1;while(upper+1<p.curve.size() && code>p.curve[upper].input) ++upper;
            const auto low=p.curve[upper-1],high=p.curve[upper];
            const double t=(static_cast<double>(code)-low.input)/(high.input-low.input);
            code=static_cast<float>(low.output+(high.output-low.output)*t);
        }
        const auto expected=pack({decodeSrgb(codes[0])*original.a,decodeSrgb(codes[1])*original.a,decodeSrgb(codes[2])*original.a,original.a});
        expect(pack(output->singleLayer().raster->pixel(x,0))==expected,"Identity channels changed legacy master-only half bits");
    }
    p.curve={{0,0},{.3,.3},{1,1}};
    p.channelCurves[1]={{0,0},{.2,.2},{.9,.9},{1,1}};
    expect(adjustmentIsNeutral(p) && adjustLayer(input,id,store,p).finish(1)==input,"All identity curves clipped HDR or created edit");
    p.channelCurves[0]={{0,0},{.5,.8},{1,1}};
    auto bounded=adjustLayer(input,id,store,p).finish(1)->singleLayer().raster->pixel(1,0);
    expect(bounded.r<=bounded.a && bounded.g==0 && bounded.a==.5f,"Nonneutral curves lost legacy all-channel clamp policy");
}
void signedSparseAndCancellation() {
    TileStore store(4*1024*1024);const std::array pixels{fromStraightSrgb({.25f,.25f,.25f,1}),Pixel{}};
    auto input=fixture(store,pixels,{-2,-1,2,1});
    const auto p=correction();auto output=adjustLayer(input,input->singleLayer().id,store,p).finish(1);
    expect(output->singleLayer().raster->extent==input->singleLayer().raster->extent &&
        output->singleLayer().raster->pixel(-2,-1).r>input->singleLayer().raster->pixel(-2,-1).r,"Signed source extent was not corrected");
    auto sparse=blankDocument(1024,1024);auto unchanged=adjustLayer(sparse,sparse->singleLayer().id,store,p).finish(1);
    expect(unchanged==sparse && unchanged->singleLayer().raster->tiles.empty(),"Channel curves materialized transparent sparse tiles");
    std::stop_source canceled;canceled.request_stop();
    rejects([&]{adjustLayer(input,input->singleLayer().id,store,p,canceled.get_token());});
    // A dedicated token is canceled at the first real scratch admission.
    std::stop_source during;
    auto memory=std::make_shared<MemoryAdmission>(1024*1024,0,[&]{during.request_stop();return MemorySample{4*1024*1024,0};});
    TileStore cancelStore(4*1024*1024,memory);
    rejects([&]{adjustLayer(input,input->singleLayer().id,cancelStore,p,during.get_token());});
    expect(during.stop_requested() && memory->snapshot().pendingCpu==0 && memory->snapshot().committedCpu==0,"Cancellation leaked channel scratch");
}
void validationAndPolicies() {
    TileStore store(4*1024*1024);const std::array pixels{fromStraightSrgb({.25f,.25f,.25f,1})};auto input=fixture(store,pixels);
    auto p=correction();
    for(int channel=-1;channel<3;++channel) {
        auto malformed=p;auto& curve=channel<0 ? malformed.curve : malformed.channelCurves[static_cast<std::size_t>(channel)];
        curve={{0,0},{.5,.5},{.5,.7},{1,1}};rejects([&]{adjustmentIsNeutral(malformed);});
        curve={{0,0},{1,std::numeric_limits<double>::quiet_NaN()}};rejects([&]{adjustmentIsNeutral(malformed);});
        curve={{.1,0},{1,1}};rejects([&]{adjustmentIsNeutral(malformed);});
        curve={{0,0},{1,1.01}};rejects([&]{adjustmentIsNeutral(malformed);});
        curve.resize(17);rejects([&]{adjustmentIsNeutral(malformed);});
    }
    AdjustmentStack legacy{input->singleLayer().raster,{p},1};rejects([&]{validateAdjustmentStack(legacy,*legacy.source);});
    legacy.operations[0].channelCurves=AdjustmentParameters{}.channelCurves;
    validateAdjustmentStack(legacy,*legacy.source);auto old=evaluateAdjustmentStack(legacy,store);
    auto current=legacy;current.policy=2;auto compatible=evaluateAdjustmentStack(current,store);
    expect(pack(old->pixel(0,0))==pack(compatible->pixel(0,0)),"Legacy policy master changed on policy-2 evaluation");
    const auto id=input->singleLayer().id;auto revised=setRevisableAdjustments(input,id,store,{p}).finish(1);
    expect(revised->singleLayer().adjustments->policy==2 && revised->singleLayer().adjustments->operations[0]==p,"New channel parameters or policy were not retained");
    expect(setRevisableAdjustments(revised,id,store,{p}).finish(2)==revised,"Identical channel controls created history");
    AdjustmentParameters irrelevant{AdjustmentKind::Exposure,1};irrelevant.channelCurves=p.channelCurves;
    auto normalized=setRevisableAdjustments(input,id,store,{irrelevant}).finish(1);
    expect(normalized->singleLayer().adjustments->operations[0].channelCurves==AdjustmentParameters{}.channelCurves,"Non-Curves retained irrelevant channel controls");
}
}
int main() {
    try {analyticOrderAndAlpha();legacyExactAndNeutralHdr();signedSparseAndCancellation();validationAndPolicies();std::cout<<"All 4 channel curve groups passed\n";}
    catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
