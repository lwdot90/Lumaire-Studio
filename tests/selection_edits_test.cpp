#include "core/editor_commands.h"
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
void invalidCommands() {
    bool rejected=false;try {selectionCandidate({},{});} catch(const std::invalid_argument&) {rejected=true;}
    check(rejected,"Null document selection rejected");
    rejected=false;try {selectionCandidate(blankDocument(10,10),Selection{Extent{0,0,11,1},SelectionShape::Rectangle,false});}
    catch(const std::invalid_argument&) {rejected=true;}check(rejected,"Outside-canvas selection rejected");
}
}
int main() {try {selectionCommands();invalidCommands();std::cout<<"2 selection edit groups passed\n";return 0;}catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}}
