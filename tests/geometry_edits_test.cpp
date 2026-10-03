#include "core/editor_commands.h"
#include <cmath>
#include <iostream>
#include <limits>

using namespace compositor::engine;
namespace {
void expect(bool condition,const char* message) {if(!condition) throw std::runtime_error(message);}
void close(Coordinate actual,Coordinate expected,const char* message) {
    expect(std::abs(actual.x-expected.x)<1e-9 && std::abs(actual.y-expected.y)<1e-9,message);
}
template<class Function> void rejects(Function function) {
    bool rejected=false;try {function();} catch(const std::exception&) {rejected=true;}
    expect(rejected,"Invalid geometry accepted");
}
DocumentPtr fixture() {
    LayerNode folder{Id::generate()};folder.folder=true;folder.opacity=.5f;
    LayerNode child{Id::generate()};child.parent=folder.id;child.sampling=Sampling::Nearest;
    child.raster=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{-2,-1,4,2},pack({1,0,0,1}));
    child.mask=std::make_shared<const RasterSnapshot>(Id::generate(),child.raster->extent,pack({0,0,0,.5f}));
    child.localToDocument={1,0,0,1,6,5};
    auto document=std::make_shared<const DocumentSnapshot>(Id::generate(),20,10,72,std::vector<LayerNode>{folder,child});
    EditTransaction selection(document);selection.setSelection(Selection{{3,2,10,5}});
    return selection.finish(1);
}
}
int main() {
    try {
        const auto initial=fixture();const auto id=initial->layers()[1].id;
        DocumentHistory history(initial);
        expect(history.commit(transformLayer(initial,id,{3,-1,2,3,90}),"Transform"),"Transform did not commit");
        const auto transformed=history.current();const auto& map=transformed->layer(id).localToDocument;
        close(map.map({0,0}),{9,4},"Placed source center moved incorrectly");
        close(map.map({2,0}),{9,8},"Local horizontal scale and clockwise rotation incorrect");
        close(map.map({0,1}),{6,4},"Local vertical scale and clockwise rotation incorrect");
        expect(transformed->layer(id).raster==initial->layer(id).raster,"Transform resampled source pixels");
        expect(transformed->layer(id).mask==initial->layer(id).mask && transformed->layer(id).maskEnabled,"Transform lost linked mask");
        expect(transformed->selection==initial->selection,"Layer transform changed document selection");
        expect(transformed->stack.evaluate(9,4)==initial->stack.evaluate(6,5),"Linked mask did not follow transformed source center");
        expect(transformed->layer(id).parent==initial->layer(id).parent && transformed->layer(id).sampling==Sampling::Nearest,"Transform lost hierarchy or sampling");
        history.undo();expect(history.current()==initial,"Transform undo lost original snapshot");
        expect(!history.commit(transformLayer(initial,id,{}),"No-op") && history.canRedo(),"Identity transform lost redo");
        expect(!history.commit(transformLayer(initial,id,{0,0,1,1,360}),"Full turn") && history.canRedo(),"Full rotation changed identity or lost redo");
        history.redo();expect(history.current()==transformed,"Transform redo lost snapshot");

        DocumentHistory cropHistory(initial);
        expect(cropHistory.commit(cropDocument(initial,{3,2,10,5}),"Crop"),"Crop did not commit");
        const auto cropped=cropHistory.current();
        expect(cropped->width==10 && cropped->height==5,"Crop dimensions incorrect");
        close(cropped->layer(id).localToDocument.map({0,0}),{3,3},"Crop did not translate source placement");
        expect(cropped->layer(id).raster==initial->layer(id).raster,"Crop destroyed source outside canvas");
        expect(cropped->layer(id).mask==initial->layer(id).mask,"Crop copied mask pixels");
        expect(!cropped->selection,"Crop retained stale document selection");
        expect(cropped->stack.evaluate(3.5,3.5)==initial->stack.evaluate(6.5,5.5),"Crop pixel placement changed");
        cropHistory.undo();expect(cropHistory.current()==initial,"Crop undo failed");
        cropHistory.redo();expect(cropHistory.current()==cropped,"Crop redo failed");

        DocumentHistory resizeHistory(initial);
        expect(resizeHistory.commit(resizeDocument(initial,40,30),"Resize"),"Resize did not commit");
        const auto resized=resizeHistory.current();
        expect(resized->width==40 && resized->height==30 && resized->resolution==initial->resolution,"Resize dimensions or resolution incorrect");
        close(resized->layer(id).localToDocument.map({0,0}),{12,15},"Resize did not scale placement");
        close(resized->layer(id).localToDocument.map({2,1}),{16,18},"Nonuniform canvas scale incorrect");
        expect(resized->layer(id).raster==initial->layer(id).raster,"Resize allocated a new source raster");
        expect(resized->layer(id).mask==initial->layer(id).mask,"Resize allocated a new mask raster");
        expect(!resized->selection,"Resize retained stale document selection");
        expect(resized->stack.evaluate(13,16.5)==initial->stack.evaluate(6.5,5.5),"Resize sampling coordinates incorrect");
        resizeHistory.undo();expect(resizeHistory.current()==initial,"Resize undo failed");
        resizeHistory.redo();expect(resizeHistory.current()==resized,"Resize redo failed");

        rejects([&]{transformLayer({},id,{});});
        rejects([&]{transformLayer(initial,Id::generate(),{});});
        rejects([&]{transformLayer(initial,initial->layers()[0].id,{});});
        for(auto scale:{0.,-1.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})
            rejects([&]{transformLayer(initial,id,{0,0,scale,1,0});});
        rejects([&]{transformLayer(initial,id,{1000001,0,1,1,0});});
        rejects([&]{transformLayer(initial,id,{0,0,1000000,1,0});});
        rejects([&]{cropDocument(initial,{-1,0,10,5});});
        rejects([&]{cropDocument(initial,{0,0,21,10});});
        rejects([&]{cropDocument(initial,{0,0,0,5});});
        rejects([&]{resizeDocument(initial,0,10);});
        rejects([&]{resizeDocument(initial,30001,10);});
        expect(initial->width==20 && initial->height==10 && initial->layer(id).localToDocument.tx==6 && initial->selection.has_value(),"Rejected geometry modified input");
        std::cout<<"non-destructive move/scale/rotate, crop, resize and immutable history passed\n";
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
