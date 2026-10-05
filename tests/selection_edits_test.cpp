#include "core/editor_commands.h"
#include "core/selection_coverage.h"
#include <limits>
#include <iostream>
#include <stdexcept>
using namespace compositor::engine;
namespace {
DocumentPtr selectionCandidate(DocumentPtr input,std::optional<Selection> selection) {
    auto command=selectDocument(input,std::move(selection));return command.finish(input->revision+1);
}
void check(bool okay,const char* message) {if(!okay) throw std::runtime_error(message);}
void selectionCommands() {
    const auto initial=blankDocument(40,30);
    const Selection rectangle{Extent{3,4,20,10},SelectionShape::Rectangle,false};
    auto selected=selectionCandidate(initial,rectangle);
    check(selected->selection==rectangle,"Rectangle selection metadata is retained");
    check(selected->singleLayer().raster==initial->singleLayer().raster,"Selection shares canonical raster");
    check(selectionCandidate(selected,rectangle)==selected,"Repeated selection is a no-op");
    DocumentHistory history(initial);
    auto edit=history.begin();edit.setSelection(selected->selection);
    check(history.commit(edit,"Rectangle selection"),"Selection is one history command");
    check(history.undo() && !history.current()->selection,"Undo restores absent selection");
    check(history.redo() && history.current()->selection==rectangle,"Redo restores exact selection");
    auto inverted=rectangle;inverted.inverted=true;
    auto candidate=selectionCandidate(history.current(),inverted);
    edit=history.begin();edit.setSelection(candidate->selection);check(history.commit(edit,"Invert selection"),"Invert selection commits");
    check(history.undo() && history.current()->selection==rectangle,"Invert undo restores prior shape");
    check(history.redo() && history.current()->selection->inverted,"Invert redo restores inversion");
    auto cleared=selectionCandidate(history.current(),{});edit=history.begin();edit.setSelection(cleared->selection);
    check(history.commit(edit,"Deselect") && !history.current()->selection,"Deselect removes active selection");
    check(history.undo() && history.current()->selection->inverted,"Deselect undo restores selection");
    const auto empty=selectionCandidate(initial,Selection{Extent{0,0,0,0},SelectionShape::Rectangle,false});
    check(empty->selection && !empty->selection->bounds.width,"Empty active selection remains distinct from deselection");
    const auto ellipse=selectionCandidate(initial,Selection{Extent{0,0,40,30},SelectionShape::Ellipse,false});
    check(ellipse->selection->shape==SelectionShape::Ellipse,"Ellipse selection metadata retained");
}
void softCoverage() {
    Selection rectangle{{10,10,20,20},SelectionShape::Rectangle,false,4};
    rectangle.validate(50,50);
    check(rectangle.coverage(10,20)==.5f,"Feather midpoint is half selected");
    check(rectangle.coverage(6,20)==0 && rectangle.coverage(14,20)==1,"Feather support spans twice radius");
    check(rectangle.coverage(9,20)>0 && rectangle.coverage(11,20)<1,"Feather retains fractional coverage on both sides");
    check(rectangle.coverage(7,7)==0,"Rectangle corner uses Euclidean distance");
    auto inverted=rectangle;inverted.inverted=true;
    for(double x:{5.,9.,10.,11.,20.,30.,34.})
        check(std::abs(rectangle.coverage(x,20)+inverted.coverage(x,20)-1)<1e-7,"Inversion complements soft coverage");
    Selection ellipse{{10,10,40,20},SelectionShape::Ellipse,false,4};
    check(ellipse.coverage(10,20)==.5f && ellipse.coverage(30,10)==.5f,"Ellipse axes share half coverage boundary");
    check(ellipse.coverage(6,20)==ellipse.coverage(30,8),"Ellipse feather metric scales semiaxes");
    check(ellipse.coverage(18,15)==ellipse.coverage(42,25),"Ellipse coverage is symmetric");
    check(ellipse.coverage(30,20)==1,"Ellipse interior fully selected");
    Selection empty{{10,10,0,20},SelectionShape::Ellipse,false,4};
    check(empty.coverage(10,20)==0,"Empty soft shape remains empty");
    empty.inverted=true;check(empty.coverage(10,20)==1,"Empty inverted soft shape covers canvas");
    check(selectionCoverage(inverted,Affine{},-1,20,50,50)==0,"Inverted feather is clipped outside canvas");
    check(selectionCoverage(rectangle,Affine{},9,20,50,50)>0,"Supersampling retains partial outside halo");
    const Affine shifted{1,0,0,1,10,10};
    check(selectionCoverage(rectangle,shifted,0,10,50,50)==selectionCoverage(rectangle,Affine{},10,20,50,50),"Feather follows document-space transform");
    const Affine scaled{2,0,0,2,0,0};
    double expected=0;
    for(int y=0;y<8;++y) for(int x=0;x<8;++x) expected+=rectangle.coverage(2*(4+(x+.5)/8),2*(10+(y+.5)/8));
    check(std::abs(selectionCoverage(rectangle,scaled,4,10,50,50)-expected/64)<1e-7,"Scaled source footprint samples fractional document coverage");
    rectangle.featherRadius=0;
    check(selectionCoverage(rectangle,Affine{},9,20,50,50)==0 && selectionCoverage(rectangle,Affine{},10,20,50,50)==1,"Zero feather preserves hard coverage");
    DocumentHistory history(blankDocument(50,50));
    auto edit=history.begin();rectangle.featherRadius=4;edit.setSelection(rectangle);
    check(history.commit(edit,"Feather selection"),"Soft selection commits once");
    check(history.undo() && !history.current()->selection,"Soft selection undo restores metadata");
    check(history.redo() && history.current()->selection==rectangle,"Soft selection redo retains exact radius");
}
void invalidFeather() {
    for(double radius:{-1.,256.001,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        bool rejected=false;
        try {selectionCandidate(blankDocument(10,10),Selection{{0,0,10,10},SelectionShape::Rectangle,false,radius});}
        catch(const std::invalid_argument&) {rejected=true;}
        check(rejected,"Invalid feather radius rejected by document command");
    }
    Selection maximum{{0,0,10,10},SelectionShape::Rectangle,false,256};maximum.validate(10,10);
    check(maximum.coverage(0,5)==.5f,"Maximum feather radius is accepted");
}
void invalidCommands() {
    bool rejected=false;try {selectionCandidate({},{});} catch(const std::invalid_argument&) {rejected=true;}
    check(rejected,"Null document selection rejected");
    rejected=false;try {selectionCandidate(blankDocument(10,10),Selection{Extent{0,0,11,1},SelectionShape::Rectangle,false});}
    catch(const std::invalid_argument&) {rejected=true;}check(rejected,"Outside-canvas selection rejected");
}
}
int main() {try {selectionCommands();invalidCommands();softCoverage();invalidFeather();std::cout<<"4 selection edit groups passed\n";return 0;}catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}}
