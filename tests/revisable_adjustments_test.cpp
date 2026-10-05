#include "core/editor_commands.h"
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
    expect(rejected,"Invalid revisable operation was accepted");
}
DocumentPtr fixture(TileStore& store,std::span<const Pixel> pixels,std::shared_ptr<const SourceProfile> profile={}) {
    TileMap tiles;tiles.emplace(TileCoord{0,0},store.create(static_cast<int>(pixels.size()),1,pixels));
    auto raster=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,static_cast<std::int64_t>(pixels.size()),1},
        PackedPixel{},std::move(tiles),0,std::move(profile));
    return std::make_shared<const DocumentSnapshot>(Id::generate(),Id::generate(),static_cast<int>(pixels.size()),1,144,raster);
}
PackedPixel sample(const DocumentPtr& document,int x=0) {return pack(document->singleLayer().raster->pixel(x,0));}
void sourceRevisionAndHistory() {
    TileStore store(4*1024*1024);
    const std::array pixels{Pixel{.125f,.0625f,.03125f,.5f},Pixel{}};
    auto profile=std::make_shared<const SourceProfile>(SourceProfile{Id::generate(),{1,2,3}});
    auto input=fixture(store,pixels,profile);const auto id=input->singleLayer().id;
    auto selected=selectDocument(input,Selection{Extent{1,0,1,1}}).finish(1);
    DocumentHistory history(selected);
    expect(history.commit(setRevisableAdjustments(history.current(),id,store,{{AdjustmentKind::Exposure,1}}),"Exposure"),"Missing first revisable edit");
    const auto first=history.current();
    expect(sample(first)==pack({.25f,.125f,.0625f,.5f}),"Revisable exposure must affect the whole retained source, ignoring selection");
    expect(first->singleLayer().adjustments->source==input->singleLayer().raster,"First edit lost original source");
    expect(first->selection==selected->selection && first->resolution==144 && first->singleLayer().raster->sourceProfile==profile,"Edit changed metadata");
    expect(first->singleLayer().raster->id!=input->singleLayer().raster->id,"Derived cache reused retained source ID");
    expect(history.commit(setRevisableAdjustments(first,id,store,{{AdjustmentKind::Exposure,2}}),"Revise"),"Missing revised parameters");
    const auto revised=history.current();
    expect(sample(revised)==pack({.5f,.25f,.125f,.5f}),"Revision compounded already adjusted pixels instead of evaluating retained source");
    expect(revised->singleLayer().adjustments->source==input->singleLayer().raster && sample(input)==pack(pixels[0]),"Revision changed retained source");
    expect(history.undo() && history.current()==first && history.redo() && history.current()==revised,"History lost parameter revisions");
    expect(history.commit(setRevisableAdjustments(revised,id,store,{}),"Reset"),"Reset did not detach stack");
    expect(!history.current()->singleLayer().adjustments && history.current()->singleLayer().raster==input->singleLayer().raster,"Reset failed exact retained-source restoration");
    expect(history.undo() && history.current()==revised,"Reset undo failed");
    expect(!history.commit(setRevisableAdjustments(revised,id,store,{{AdjustmentKind::Exposure,2}}),"Same"),"Same controls created history");
    expect(history.canRedo(),"Same controls erased reset redo");
}
void canonicalizationAndOrderedMath() {
    TileStore store(4*1024*1024);const std::array pixels{Pixel{.125f,.0625f,.03125f,.5f}};
    auto input=fixture(store,pixels);const auto id=input->singleLayer().id;
    AdjustmentParameters exposure{AdjustmentKind::Exposure,1};exposure.levels.gamma=3;exposure.curve={{0,.2},{1,.8}};exposure.colorBalance.warmth=.75;
    auto first=setRevisableAdjustments(input,id,store,{exposure}).finish(1);
    const auto& stored=first->singleLayer().adjustments->operations.front();
    expect(stored==AdjustmentParameters{AdjustmentKind::Exposure,1},"Irrelevant controls were not canonicalized");
    expect(setRevisableAdjustments(first,id,store,{{AdjustmentKind::Exposure,1}}).finish(2)==first,"Canonical controls were not recognized as unchanged");
    AdjustmentParameters balance;balance.kind=AdjustmentKind::ColorBalance;balance.colorBalance={1,-1};
    auto ordered=setRevisableAdjustments(input,id,store,{{AdjustmentKind::Exposure,1},balance}).finish(1);
    expect(sample(ordered)==pack({.5f,.25f,.03125f,.5f}),"Sequential exposure/balance differs from independent powers-of-two gains");
    expect(sample(input)==pack(pixels[0]),"Sequential evaluation mutated source");
    auto neutral=setRevisableAdjustments(input,id,store,{{AdjustmentKind::Exposure,0}}).finish(1);
    expect(neutral->singleLayer().adjustments && neutral->singleLayer().raster->id!=input->singleLayer().raster->id &&
        neutral->singleLayer().raster->tiles==input->singleLayer().raster->tiles && sample(neutral)==sample(input),"Neutral stack lost exact source tile sharing or distinct cache ID");
}
void duplicateAndRasterize() {
    TileStore store(4*1024*1024);const std::array pixels{Pixel{.125f,0,0,.5f}};
    auto input=fixture(store,pixels);const auto id=input->singleLayer().id;
    auto adjusted=setRevisableAdjustments(input,id,store,{{AdjustmentKind::Exposure,1}}).finish(1);
    auto a=adjusted->singleLayer(),b=a;b.id=Id::generate();b.siblingOrder=1;
    auto duplicated=std::make_shared<const DocumentSnapshot>(adjusted->id,1,1,72,std::vector<LayerNode>{a,b},1);
    auto changed=setRevisableAdjustments(duplicated,a.id,store,{{AdjustmentKind::Exposure,2}}).finish(2);
    expect(changed->layer(b.id).raster==b.raster && changed->layer(b.id).adjustments==b.adjustments,"Changing duplicate mutated its peer");
    auto rasterized=rasterizeLayerAdjustments(changed,a.id).finish(3);
    expect(!rasterized->layer(a.id).adjustments && rasterized->layer(a.id).raster->tiles==changed->layer(a.id).raster->tiles &&
        rasterized->layer(a.id).raster->id!=changed->layer(a.id).raster->id,"Rasterization changed visible tiles or reused derived ID");
    expect(rasterized->layer(b.id).adjustments==b.adjustments,"Rasterization detached peer");
    expect(rasterizeLayerAdjustments(rasterized,a.id).finish(4)==rasterized,"Repeated rasterization was not a no-op");
    auto next=setRevisableAdjustments(rasterized,a.id,store,{{AdjustmentKind::Exposure,1}}).finish(4);
    expect(next->layer(a.id).adjustments->source==rasterized->layer(a.id).raster,"Post-rasterize edit did not establish new retained source");
    EditTransaction write(changed,a.id);
    rejects([&]{write.write(store,TileCoord{0,0},pixels);});
    rejects([&]{write.finish(3);});
    EditTransaction replace(changed,a.id);
    rejects([&]{replace.replace(TileCoord{0,0},a.raster->tiles.begin()->second);});
    rejects([&]{replace.finish(3);});
    auto moved=transformLayer(changed,a.id,{2,0,1,1,0}).finish(3);
    expect(moved->layer(a.id).adjustments==changed->layer(a.id).adjustments,"Metadata transform discarded adjustment stack");
    auto masked=createLayerMask(changed,a.id,store,false).finish(3);
    expect(masked->layer(a.id).mask && masked->layer(a.id).adjustments==changed->layer(a.id).adjustments,"Mask command failed on revisable layer");
}
void metadataAndBounds() {
    TileStore store(4*1024*1024);const std::array pixels{Pixel{.125f,0,0,.5f}};
    auto input=fixture(store,pixels);const auto source=input->singleLayer().raster;
    AdjustmentStack stack{source,{{AdjustmentKind::Exposure,1}},1};validateAdjustmentStack(stack,*source);
    stack.policy=3;rejects([&]{validateAdjustmentStack(stack,*source);});stack.policy=1;
    stack.source.reset();rejects([&]{validateAdjustmentStack(stack,*source);});stack.source=source;
    stack.operations.resize(17);rejects([&]{validateAdjustmentStack(stack,*source);});stack.operations={{AdjustmentKind::Exposure,9}};
    rejects([&]{validateAdjustmentStack(stack,*source);});stack.operations={{AdjustmentKind::Exposure,std::numeric_limits<double>::quiet_NaN()}};
    rejects([&]{validateAdjustmentStack(stack,*source);});
    stack.operations.clear();rejects([&]{validateAdjustmentStack(stack,*source);});
    stack.operations={{AdjustmentKind::Exposure,1}};
    auto incompatible=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,2,1});
    rejects([&]{validateAdjustmentStack(stack,*incompatible);});
    auto profiled=std::make_shared<const RasterSnapshot>(Id::generate(),source->extent,PackedPixel{},TileMap{},0,
        std::make_shared<const SourceProfile>(SourceProfile{Id::generate(),{4}}));
    rejects([&]{validateAdjustmentStack(stack,*profiled);});
    LayerNode folder{Id::generate()};folder.folder=true;folder.adjustments=std::make_shared<const AdjustmentStack>(stack);
    rejects([&]{LayerStack invalid({folder});});
    auto conflicting=std::make_shared<const RasterSnapshot>(source->id,source->extent);
    auto node=input->singleLayer();node.adjustments=std::make_shared<const AdjustmentStack>(AdjustmentStack{conflicting,{{AdjustmentKind::Exposure,1}},1});
    rejects([&]{LayerStack invalid({node});});
    // Source and cache count as distinct live assets. Shared references count once.
    auto largeSource=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,8000,8000});
    auto largeCache=std::make_shared<const RasterSnapshot>(Id::generate(),largeSource->extent);
    node.raster=largeCache;node.adjustments=std::make_shared<const AdjustmentStack>(AdjustmentStack{largeSource,{{AdjustmentKind::Exposure,1}},1});
    rejects([&]{LayerStack invalid({node});});
    node.raster=largeSource;LayerStack shared({node});expect(shared.nodes().size()==1,"Shared source/cache was double-counted");
}
void failureAndCancellation() {
    TileStore store(4*1024*1024);const std::array pixels{Pixel{.125f,0,0,.5f}};
    auto input=fixture(store,pixels);const auto id=input->singleLayer().id;
    std::stop_source canceled;canceled.request_stop();
    rejects([&]{setRevisableAdjustments(input,id,store,{{AdjustmentKind::Exposure,1}},canceled.get_token());});
    expect(!input->singleLayer().adjustments && sample(input)==pack(pixels[0]),"Canceled evaluation published metadata/pixels");
    auto admission=std::make_shared<MemoryAdmission>(1,0,[]{return MemorySample{1024,0};});
    TileStore denied(4*1024*1024,admission);
    rejects([&]{setRevisableAdjustments(input,id,denied,{{AdjustmentKind::Exposure,1}});});
    const auto charges=admission->snapshot();expect(charges.pendingCpu==0 && charges.committedCpu==0,"Refusal leaked memory charges");
    std::stop_source during;
    auto cancelAdmission=std::make_shared<MemoryAdmission>(1024*1024,0,[&]{during.request_stop();return MemorySample{4*1024*1024,0};});
    TileStore cancelStore(4*1024*1024,cancelAdmission);
    rejects([&]{setRevisableAdjustments(input,id,cancelStore,{{AdjustmentKind::Exposure,1}},during.get_token());});
    expect(during.stop_requested() && sample(input)==pack(pixels[0]),"Mid-evaluation cancellation did not preserve source");
    const auto canceledCharges=cancelAdmission->snapshot();
    expect(canceledCharges.pendingCpu==0 && canceledCharges.committedCpu==0,"Mid-evaluation cancellation leaked scratch charges");
    const std::array overflowPixels{Pixel{512,0,0,.5f}};auto overflow=fixture(store,overflowPixels);
    rejects([&]{setRevisableAdjustments(overflow,overflow->singleLayer().id,store,{{AdjustmentKind::Exposure,1},{AdjustmentKind::Exposure,8}});});
    expect(!overflow->singleLayer().adjustments && sample(overflow)==pack(overflowPixels[0]),"Late overflow partially published stack");
}
void extendedAndAlpha() {
    TileStore store(4*1024*1024);const std::array pixels{Pixel{2,-.25f,.125f,.5f},Pixel{}};
    auto input=fixture(store,pixels);const auto id=input->singleLayer().id;
    auto neutral=setRevisableAdjustments(input,id,store,{{AdjustmentKind::Exposure,0}}).finish(1);
    expect(sample(neutral)==sample(input) && neutral->singleLayer().raster->tiles==input->singleLayer().raster->tiles,"Neutral revision clipped extended RGB");
    auto edited=setRevisableAdjustments(input,id,store,{{AdjustmentKind::Exposure,1}}).finish(1);
    expect(sample(edited)==pack({4,-.5f,.25f,.5f}) && sample(edited,1)==PackedPixel{},"Extended RGB or alpha was not preserved");
    auto reset=setRevisableAdjustments(edited,id,store,{}).finish(2);expect(reset->singleLayer().raster==input->singleLayer().raster,"Reset lost extended source");
}
}
int main() {
    try {
        sourceRevisionAndHistory();canonicalizationAndOrderedMath();duplicateAndRasterize();metadataAndBounds();failureAndCancellation();extendedAndAlpha();
        std::cout<<"All 6 revisable adjustment test groups passed\n";
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
