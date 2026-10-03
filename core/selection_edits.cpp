#include "core/editor_commands.h"
#include <stdexcept>

namespace compositor::engine {
EditTransaction selectDocument(DocumentPtr input,std::optional<Selection> selection) {
    if(!input) throw std::invalid_argument("Selection requires a document");
    if(selection) selection->validate(input->width,input->height);
    EditTransaction edit(input);edit.setSelection(std::move(selection));
    return edit;
}
}
