#include "core/editor_commands.h"
#include "io/spill_store.h"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace compositor;
using namespace compositor::engine;
namespace {
void expect(bool pass,const char* message) {if(!pass) throw std::runtime_error(message);}
void near(float actual,float expected,const char* message) {expect(std::abs(actual-expected)<.001f,message);}
template<class F> void rejects(F action) {try {action();} catch(const std::exception&) {return;}throw std::runtime_error("Expected rejected brush command");}
DocumentPtr layerDocument(TileStore&,Pixel value={},Affine transform={}) {
    auto raster=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,16,16},pack(value));
    LayerNode layer{Id::generate()};layer.raster=std::move(raster);layer.localToDocument=transform;
    return std::make_shared<const DocumentSnapshot>(Id::generate(),32,32,72,std::vector<LayerNode>{layer});
}
void hardAndCap() {
    TileStore store(4*1024*1024);auto document=layerDocument(store);const auto target=document->singleLayer().id;
    DocumentHistory history(document);BrushSettings settings;settings.diameter=8;settings.opacity=.5;settings.color={1,0,0,1};
    const Coordinate path[]{{4.5,4.5},{10.5,4.5},{4.5,4.5},{10.5,4.5}};
    auto command=brushStroke(document,target,store,path,settings);
    expect(history.commit(command,"Brush stroke"),"Hard stroke changed pixels");
    const auto raster=history.current()->singleLayer().raster;
    near(raster->pixel(7,4).a,.5f,"Overlapping hard dabs exceeded whole-stroke opacity");
    near(raster->pixel(7,4).r,.5f,"Premultiplied linear red coverage");
    expect(raster->pixel(15,15)==Pixel{},"Untouched pixel changed");
    auto captured=history.current();expect(history.undo() && history.current()==document && history.redo() && history.current()==captured,"Stroke one undo/redo entry exact");
    history.undo();const Coordinate outside[]{{100,100}};auto noop=brushStroke(history.current(),target,store,outside,settings);
    expect(!history.commit(noop,"Outside") && history.canRedo(),"No-op destroyed redo");
}
void softAndErase() {
    TileStore store(4*1024*1024);auto document=layerDocument(store);BrushSettings settings;settings.diameter=12;settings.hardness=0;settings.opacity=.25;settings.color={1,1,1,1};
    const Coordinate path[]{{7.5,7.5},{8.5,7.5},{7.5,7.5}};
    auto painted=brushStroke(document,document->singleLayer().id,store,path,settings).finish(1);
    const auto raster=painted->singleLayer().raster;const auto center=raster->pixel(7,7),edge=raster->pixel(12,7);
    expect(center.a>0 && center.a<=.251f && edge.a>0 && edge.a<center.a,"Soft falloff or opacity cap incorrect");
    auto opaque=layerDocument(store,{.5f,.25f,.125f,1});settings.hardness=1;settings.opacity=.5;settings.erasing=true;
    const Coordinate dab[]{{7.5,7.5}};auto erased=brushStroke(opaque,opaque->singleLayer().id,store,dab,settings).finish(1);
    const auto pixel=erased->singleLayer().raster->pixel(7,7);near(pixel.a,.5f,"Eraser alpha");near(pixel.r,.25f,"Eraser preserves premultiplication");
    settings.opacity=1;auto cleared=brushStroke(opaque,opaque->singleLayer().id,store,dab,settings).finish(1);
    expect(cleared->singleLayer().raster->pixel(7,7)==Pixel{},"Full erase canonical transparency");
}
void transformedAndSelected() {
    TileStore store(4*1024*1024);auto document=layerDocument(store,{},Affine{2,0,0,2,0,0});
    BrushSettings settings;settings.diameter=4;settings.color={1,0,0,1};const Coordinate dab[]{{9,9}};
    auto transformed=brushStroke(document,document->singleLayer().id,store,dab,settings).finish(1);
    expect(transformed->singleLayer().raster->pixel(4,4).a>0 && transformed->singleLayer().raster->pixel(7,4).a==0,"Diameter not measured in document space");
    document=selectDocument(document,Selection{Extent{8,8,2,2}}).finish(1);
    settings.diameter=12;auto clipped=brushStroke(document,document->singleLayer().id,store,dab,settings).finish(2);
    expect(clipped->singleLayer().raster->pixel(4,4).a>0 && clipped->singleLayer().raster->pixel(5,4).a==0,"Selection clip not in document space");
    auto half=selectDocument(layerDocument(store,{},Affine{1,0,0,1,.5,0}),Selection{Extent{0,0,1,16}}).finish(1);
    const Coordinate halfDab[]{{1,7.5}};settings.diameter=12;
    auto fractional=brushStroke(half,half->singleLayer().id,store,halfDab,settings).finish(2);
    near(fractional->singleLayer().raster->pixel(0,7).a,.5f,"Transformed selection pixel footprint must have half coverage");
    expect(fractional->singleLayer().raster->pixel(1,7).a==0,"Half-translated selection changed adjacent pixel");
    auto ellipse=selectDocument(layerDocument(store),Selection{Extent{2,2,10,10},SelectionShape::Ellipse,true}).finish(1);
    settings.diameter=16;const Coordinate middle[]{{7,7}};auto inverted=brushStroke(ellipse,ellipse->singleLayer().id,store,middle,settings).finish(2);
    expect(inverted->singleLayer().raster->pixel(7,7).a==0 && inverted->singleLayer().raster->pixel(2,2).a>0,"Inverted ellipse clip");
    auto offset=selectDocument(layerDocument(store,{},Affine{1,0,0,1,-8,0}),Selection{Extent{0,0,1,1},SelectionShape::Rectangle,true}).finish(1);
    settings.diameter=20;const Coordinate boundary[]{{0,7.5}};
    auto bounded=brushStroke(offset,offset->singleLayer().id,store,boundary,settings).finish(2);
    expect(bounded->singleLayer().raster->pixel(0,7).a==0 && bounded->singleLayer().raster->pixel(10,7).a>0,"Inverted selection extended paint past canvas");
}
void masksAndCancellation() {
    TileStore store(4*1024*1024);auto document=layerDocument(store);const auto target=document->singleLayer().id;
    document=createLayerMask(document,target,store,false).finish(1);
    BrushSettings settings;settings.diameter=8;settings.paintMask=true;settings.color={0,0,0,1};settings.opacity=.5;
    const Coordinate dab[]{{7.5,7.5}};auto changed=brushStroke(document,target,store,dab,settings).finish(2);
    near(changed->layer(target).mask->pixel(7,7).a,.5f,"Mask grayscale alpha paint");
    auto white=settings;white.color={1,1,1,1};
    auto revealed=brushStroke(changed,target,store,dab,white).finish(3);
    near(revealed->layer(target).mask->pixel(7,7).a,.75f,"White mask paint reveals toward full coverage");
    white.erasing=true;white.opacity=1;
    auto hidden=brushStroke(revealed,target,store,dab,white).finish(4);
    expect(hidden->layer(target).mask->pixel(7,7)==Pixel{},"Mask eraser paints canonical black coverage");
    expect(changed->layer(target).raster==document->layer(target).raster,"Mask paint changed color raster");
    auto nodes=document->layers();auto duplicate=nodes.front();duplicate.id=Id::generate();duplicate.siblingOrder=1;
    const auto duplicateId=duplicate.id;nodes.push_back(duplicate);
    auto shared=std::make_shared<const DocumentSnapshot>(document->id,document->width,document->height,document->resolution,std::move(nodes),document->revision);
    auto forked=brushStroke(shared,target,store,dab,settings).finish(2);
    expect(forked->layer(target).mask->id!=forked->layer(duplicateId).mask->id && forked->layer(duplicateId).mask==document->layer(target).mask,"Shared mask edit did not fork immutable asset identity");
    DocumentHistory history(document);expect(history.commit(brushStroke(document,target,store,dab,settings),"Paint mask") && history.undo() && history.current()==document,"Mask undo identity");
    std::stop_source stop;stop.request_stop();rejects([&]{brushStroke(document,target,store,dab,settings,stop.get_token());});
    expect(history.canRedo(),"Canceled brush lost redo");
    const std::vector<Coordinate> tooMany(8193);rejects([&]{brushStroke(document,target,store,tooMany,settings);});
    auto tiny=settings;tiny.diameter=1;const Coordinate longPath[]{{0,0},{65536,0}};
    const auto used=store.usedBytes();rejects([&]{brushStroke(document,target,store,longPath,tiny);});
    expect(store.usedBytes()==used,"Overlong dab expansion allocated replacement pixels");
    settings.diameter=4097;rejects([&]{brushStroke(document,target,store,dab,settings);});
    settings.diameter=8;settings.color.a=std::numeric_limits<float>::quiet_NaN();rejects([&]{brushStroke(document,target,store,dab,settings);});
}
void admittedCancellation() {
    std::stop_source stop;unsigned probes=0;
    auto memory=std::make_shared<MemoryAdmission>(4*1024*1024,0,[&]{
        if(++probes==2) stop.request_stop();
        return MemorySample{8*1024*1024,0};
    });
    TileStore store(1024*1024,memory);auto document=layerDocument(store);BrushSettings settings;settings.diameter=8;
    const Coordinate dab[]{{7.5,7.5}};
    rejects([&]{brushStroke(document,document->singleLayer().id,store,dab,settings,stop.get_token());});
    const auto ledger=memory->snapshot();
    expect(probes==2 && store.usedBytes()==0 && ledger.pendingCpu==0 && ledger.committedCpu==0 && document->revision==0,"Cancellation after scratch admission leaked charges or published pixels");
}
void spilledInput(const std::filesystem::path& root) {
    auto pattern=(root/"brush-test-XXXXXX").string();const auto created=::mkdtemp(pattern.data());expect(created,"Create brush disk fixture");
    const std::filesystem::path directory=created;
    try {
        auto memory=std::make_shared<MemoryAdmission>(8*1024*1024,0,[]{return MemorySample{16*1024*1024,0};});
        auto spill=std::make_shared<io::SpillStore>(directory,io::SpillLimits{1024*1024,65536,0,32,1,1024});TileStore store(1024*1024,memory,spill);
        auto document=layerDocument(store);std::vector<Pixel> pixels(16*16,{.25f,.5f,0,1});pixels[0]={1,0,0,1};
        auto edit=EditTransaction(document,document->singleLayer().id);edit.write(store,{0,0},pixels);document=edit.finish(1);
        const auto original=document->singleLayer().raster->tiles.at({0,0});original->spill();const auto version=original->version();
        BrushSettings settings;settings.diameter=4;settings.color={0,0,1,1};const Coordinate dab[]{{7.5,7.5}};
        auto result=brushStroke(document,document->singleLayer().id,store,dab,settings).finish(2);
        expect(result->singleLayer().raster->pixel(7,7).b==1 && original->version()==version && document->singleLayer().raster->pixel(7,7).b==0,"Spilled original mutated by stroke");
    } catch(...) {std::filesystem::remove_all(directory);throw;}
    std::filesystem::remove_all(directory);
}
}
int main(int argc,char** argv) {
    try {expect(argc==2,"Pass disk-backed fixture parent");hardAndCap();softAndErase();transformedAndSelected();masksAndCancellation();admittedCancellation();spilledInput(argv[1]);std::cout<<"Six brush groups passed\n";return 0;}
    catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
