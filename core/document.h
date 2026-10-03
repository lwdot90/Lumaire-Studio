#pragma once
#include "core/layer_stack.h"
#include "core/editing_types.h"
#include <string>
#include <string_view>

namespace compositor::engine {
struct DocumentSnapshot {
    DocumentSnapshot(Id id, Id layerId, int width, int height, double resolution,
                     std::shared_ptr<const RasterSnapshot> raster, std::int64_t revision=0,
                     std::string layerName="Background", bool visible=true, float opacity=1, std::optional<Selection> selection={});
    DocumentSnapshot(Id id, int width, int height, double resolution,
                     std::vector<LayerNode> layers, std::int64_t revision=0, std::optional<Selection> selection={});
    const Id id;
    const int width, height;
    const double resolution;
    const std::int64_t revision;
    const LayerStack stack;
    const std::optional<Selection> selection;
    const std::vector<LayerNode>& layers() const { return stack.nodes(); }
    const LayerNode& layer(const Id& id) const;
    // Explicit singleton adapter for M2 import callers; never silently selects
    // the first layer of a layered document.
    const LayerNode& singleLayer() const;
};
using DocumentPtr=std::shared_ptr<const DocumentSnapshot>;
DocumentPtr blankDocument(int width, int height);

class EditTransaction {
public:
    explicit EditTransaction(DocumentPtr input);
    EditTransaction(DocumentPtr input, const Id& target);
    // Whole-graph replacement for metadata/hierarchy edits. Cannot be mixed
    // with pixel writes in one transaction; validation precedes publication.
    void setLayers(std::vector<LayerNode> layers);
    void setCanvas(int width,int height);
    void setSelection(std::optional<Selection> selection);
    // On any replacement error, poison the whole transaction. It can never
    // publish earlier successful replacements after one failed operation.
    void replace(TileCoord coordinate, TilePtr tile);
    void write(TileStore& store, TileCoord coordinate, std::span<const Pixel> pixels);
    DocumentPtr finish(std::int64_t revision) const;
    const DocumentPtr& input() const { return input_; }
private:
    DocumentPtr input_;
    std::optional<Id> target_;
    std::optional<std::vector<LayerNode>> layers_;
    TileMap tiles_;
    std::optional<std::pair<int,int>> canvas_;
    std::optional<std::optional<Selection>> selection_;
    bool pixelWrite_=false;
    bool failed_=false;
};

// Adapted from DocumentHistory.swift. UI-thread owner; immutable snapshots may
// be handed to workers. No-op commands preserve redo. Saved revision is the
// captured revision, not necessarily the current one when IO completes.
class DocumentHistory {
public:
    explicit DocumentHistory(DocumentPtr initial, std::size_t entryLimit=100,
                             std::size_t retainedByteLimit=512*1024*1024);
    const DocumentPtr& current() const { return current_; }
    EditTransaction begin() const { return EditTransaction(current_); }
    EditTransaction begin(const Id& target) const { return EditTransaction(current_,target); }
    bool commit(const EditTransaction& transaction, std::string name);
    bool commit(const EditTransaction& transaction, std::string name, std::optional<Id> selected);
    const std::optional<Id>& selectedLayer() const { return selected_; }
    void selectLayer(std::optional<Id> selected);
    bool undo();
    bool redo();
    bool canUndo() const { return !past_.empty(); }
    bool canRedo() const { return !future_.empty(); }
    std::string_view undoName() const { return past_.empty() ? std::string_view{} : std::string_view(past_.back().name); }
    bool dirty() const { return current_->revision!=savedRevision_; }
    void markSaved(const DocumentPtr& captured);
    void markUnsaved() { savedRevision_=-1; }
    std::size_t retainedBytes() const;
private:
    struct Entry { std::string name; DocumentPtr before, after; std::optional<Id> selectedBefore,selectedAfter; };
    static std::size_t retainedBytes(const DocumentPtr& current, const std::vector<Entry>& past,
                                     const std::vector<Entry>& future);
    void trim(const DocumentPtr& current, std::vector<Entry>& past, std::vector<Entry>& future) const;
    DocumentPtr current_;
    std::optional<Id> selected_;
    std::vector<Entry> past_, future_;
    std::int64_t savedRevision_, nextRevision_;
    std::size_t entryLimit_, retainedByteLimit_;
};
}
