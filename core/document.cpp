#include "core/document.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <unordered_set>
#include <sys/random.h>

namespace compositor::engine {
Id::Id(std::string text):text_(std::move(text)) {
    if(text_.size()!=36) throw std::invalid_argument("Invalid UUID length");
    for(std::size_t i=0;i<text_.size();++i) {
        const char c=text_[i];
        const bool dash=i==8 || i==13 || i==18 || i==23;
        if(dash ? c!='-' : !((c>='0' && c<='9') || (c>='a' && c<='f')))
            throw std::invalid_argument("UUID must be canonical lowercase text");
    }
}
Id Id::generate() {
    std::array<unsigned char,16> bytes{};
    std::size_t offset=0;
    while(offset<bytes.size()) {
        const auto count=getrandom(bytes.data()+offset,bytes.size()-offset,0);
        if(count<0) {
            if(errno==EINTR) continue;
            throw std::system_error(errno,std::generic_category(),"UUID entropy");
        }
        if(count==0) throw std::runtime_error("No UUID entropy available");
        offset+=static_cast<std::size_t>(count);
    }
    bytes[6]=static_cast<unsigned char>((bytes[6]&15)|0x40);
    bytes[8]=static_cast<unsigned char>((bytes[8]&63)|0x80);
    constexpr char hex[]="0123456789abcdef";
    std::string text;
    text.reserve(36);
    for(std::size_t i=0;i<bytes.size();++i) {
        if(i==4 || i==6 || i==8 || i==10) text+='-';
        text+=hex[bytes[i]>>4]; text+=hex[bytes[i]&15];
    }
    return Id(std::move(text));
}
RasterSnapshot::RasterSnapshot(Id idValue, Extent extentValue, PackedPixel defaultPixel, TileMap tileValues, std::int64_t revisionValue,
                               std::shared_ptr<const SourceProfile> profile)
    :id(std::move(idValue)),extent(extentValue),defaultValue(defaultPixel),tiles(std::move(tileValues)),revision(revisionValue),sourceProfile(std::move(profile)) {
    extent.validate();
    validateCanonical(defaultValue);
    if(revision<0) throw std::invalid_argument("Negative asset revision");
    if(sourceProfile && (sourceProfile->bytes.empty() || sourceProfile->bytes.size()>4*1024*1024))
        throw std::invalid_argument("Source ICC profile exceeds limits");
    for(const auto& [coord,tile]:tiles) {
        const auto region=tileExtent(extent,coord);
        if(!tile || tile->width()!=region.width || tile->height()!=region.height)
            throw std::invalid_argument("Tile does not match asset intersection");
    }
}
Pixel RasterSnapshot::pixel(std::int64_t x, std::int64_t y) const {
    // The immutable constructor has already validated extent endpoints and
    // every tile intersection. Do not repeat that validation for every tap.
    if(x<extent.x || y<extent.y || x>=extent.x+extent.width || y>=extent.y+extent.height) return {};
    const TileCoord coord{floorTile(x),floorTile(y)};
    const auto found=tiles.find(coord);
    if(found==tiles.end()) return unpack(defaultValue);
    const auto left=std::max(extent.x,coord.x*tileSide),top=std::max(extent.y,coord.y*tileSide);
    return found->second->linearPixel(static_cast<int>(x-left),static_cast<int>(y-top));
}
DocumentSnapshot::DocumentSnapshot(Id idValue, Id layerIdValue, int w, int h, double ppi,
                                   std::shared_ptr<const RasterSnapshot> rasterValue, std::int64_t revisionValue,
                                   std::string name, bool visibility, float alpha,std::optional<Selection> selected)
    :DocumentSnapshot(std::move(idValue),w,h,ppi,
        {LayerNode{std::move(layerIdValue),{},0,false,visibility,alpha,BlendMode::Normal,{},Sampling::Bilinear,std::move(rasterValue),std::move(name)}},revisionValue,std::move(selected)) {}
DocumentSnapshot::DocumentSnapshot(Id idValue,int w,int h,double ppi,std::vector<LayerNode> nodes,std::int64_t revisionValue,std::optional<Selection> selected)
    :id(std::move(idValue)),width(w),height(h),resolution(ppi),revision(revisionValue),stack(std::move(nodes)),selection(std::move(selected)) {
    Extent{0,0,width,height}.validate();
    if(selection) selection->validate(width,height);
    if(!std::isfinite(resolution) || resolution<=0 || revision<0)
        throw std::invalid_argument("Invalid document metadata");
}
const LayerNode& DocumentSnapshot::layer(const Id& layerId) const {
    return stack.node(layerId);
}
const LayerNode& DocumentSnapshot::singleLayer() const {
    if(layers().size()!=1 || layers()[0].folder) throw std::logic_error("Operation requires a single raster layer");
    return layers()[0];
}
DocumentPtr blankDocument(int width, int height) {
    Extent{0,0,width,height}.validate();
    auto raster=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,width,height});
    return std::make_shared<const DocumentSnapshot>(Id::generate(),Id::generate(),width,height,72,std::move(raster));
}
EditTransaction::EditTransaction(DocumentPtr input):input_(std::move(input)) {
    if(!input_) throw std::invalid_argument("Missing input snapshot");
    if(input_->layers().size()==1 && !input_->layers()[0].folder) {
        target_=input_->layers()[0].id; tiles_=input_->layers()[0].raster->tiles;
    }
}
EditTransaction::EditTransaction(DocumentPtr input,const Id& target):input_(std::move(input)),target_(target) {
    if(!input_) throw std::invalid_argument("Missing input snapshot");
    const auto& node=input_->layer(target);
    if(!node.raster) throw std::invalid_argument("Cannot paint a folder");
    tiles_=node.raster->tiles;
}
void EditTransaction::setLayers(std::vector<LayerNode> layers) {
    if(failed_) throw std::logic_error("Transaction previously failed");
    try {
        if(pixelWrite_) throw std::logic_error("Cannot mix graph and pixel edits");
        LayerStack validated(layers);
        layers_=std::move(layers);
    } catch(...) { failed_=true; throw; }
}
void EditTransaction::replace(TileCoord coordinate, TilePtr tile) {
    if(failed_) throw std::logic_error("Transaction previously failed");
    try {
        if(!target_ || layers_) throw std::logic_error("Pixel edit requires a target and no graph edits");
        pixelWrite_=true;
        const auto& source=*input_->layer(*target_).raster;
        const auto region=tileExtent(source.extent,coordinate);
        if(!tile || tile->width()!=region.width || tile->height()!=region.height)
            throw std::invalid_argument("Replacement tile dimensions do not match asset");
        if(tile->uniform() && tile->pixel(0,0)==source.defaultValue) tiles_.erase(coordinate);
        else {
            const auto original=source.tiles.find(coordinate);
            if(original!=source.tiles.end() && tile->samePixels(*original->second))
                tile=original->second; // An edit back to the input shares its exact version.
            tiles_.insert_or_assign(coordinate,std::move(tile));
        }
    } catch(...) { failed_=true; throw; }
}
void EditTransaction::write(TileStore& store, TileCoord coordinate, std::span<const Pixel> pixels) {
    if(failed_) throw std::logic_error("Transaction previously failed");
    try {
        if(!target_ || layers_) throw std::logic_error("Pixel edit requires a target and no graph edits");
        const auto region=tileExtent(input_->layer(*target_).raster->extent,coordinate);
        replace(coordinate,store.create(static_cast<int>(region.width),static_cast<int>(region.height),pixels));
    } catch(...) { failed_=true; throw; }
}
void EditTransaction::setCanvas(int width,int height) {
    if(failed_) throw std::logic_error("Transaction previously failed");
    try { Extent{0,0,width,height}.validate(); canvas_=std::pair{width,height}; }
    catch(...) { failed_=true; throw; }
}
void EditTransaction::setSelection(std::optional<Selection> selection) {
    if(failed_) throw std::logic_error("Transaction previously failed");
    try {
        if(selection) selection->validate(canvas_ ? canvas_->first : input_->width,canvas_ ? canvas_->second : input_->height);
        selection_=std::move(selection);
    } catch(...) { failed_=true; throw; }
}
DocumentPtr EditTransaction::finish(std::int64_t revision) const {
    if(failed_) throw std::logic_error("Cannot publish a failed transaction");
    auto nodes=layers_ ? *layers_ : input_->layers();
    bool changed=nodes!=input_->layers();
    if(!layers_ && target_ && pixelWrite_) {
        const auto& source=*input_->layer(*target_).raster;
        bool pixelsChanged=tiles_.size()!=source.tiles.size();
        if(!pixelsChanged) for(const auto& [coord,tile]:tiles_) {
            const auto found=source.tiles.find(coord);
            if(found==source.tiles.end() || tile!=found->second) { pixelsChanged=true; break; }
        }
        if(pixelsChanged) {
            if(revision<=source.revision) throw std::invalid_argument("Revision must advance");
            const bool shared=std::count_if(nodes.begin(),nodes.end(),[&](const auto& node){return node.raster.get()==&source;})>1;
            auto raster=std::make_shared<const RasterSnapshot>(shared ? Id::generate() : source.id,source.extent,source.defaultValue,tiles_,revision,source.sourceProfile);
            for(auto& node:nodes) if(node.id==*target_) node.raster=std::move(raster);
            changed=true;
        }
    }
    const int width=canvas_ ? canvas_->first : input_->width,height=canvas_ ? canvas_->second : input_->height;
    const auto selection=selection_ ? *selection_ : input_->selection;
    changed=changed || width!=input_->width || height!=input_->height || selection!=input_->selection;
    if(!changed) return input_;
    if(revision<=input_->revision) throw std::invalid_argument("Revision must advance");
    return std::make_shared<const DocumentSnapshot>(input_->id,width,height,input_->resolution,std::move(nodes),revision,selection);
}
DocumentHistory::DocumentHistory(DocumentPtr initial, std::size_t entryLimit, std::size_t retainedByteLimit)
    :current_(std::move(initial)),entryLimit_(entryLimit),retainedByteLimit_(retainedByteLimit) {
    if(!current_) throw std::invalid_argument("Missing initial document");
    savedRevision_=current_->revision;
    nextRevision_=current_->revision;
    for(const auto& node:current_->layers()) for(const auto& raster:{node.raster,node.mask}) if(raster) nextRevision_=std::max(nextRevision_,raster->revision);
    if(!current_->layers().empty()) selected_=current_->layers().back().id;
}
std::size_t DocumentHistory::retainedBytes(const DocumentPtr& current, const std::vector<Entry>& past,
                                          const std::vector<Entry>& future) {
    std::unordered_set<const Tile*> seen;
    for(const auto& node:current->layers()) for(const auto& raster:{node.raster,node.mask}) if(raster)
        for(const auto& [coord,tile]:raster->tiles) seen.insert(tile.get());
    std::size_t bytes=0;
    const auto count=[&](const std::vector<Entry>& entries) {
        for(const auto& entry:entries) for(const auto& snapshot:{entry.before,entry.after})
            for(const auto& node:snapshot->layers()) for(const auto& raster:{node.raster,node.mask}) if(raster)
            for(const auto& [coord,tile]:raster->tiles)
                if(seen.insert(tile.get()).second) bytes+=tile->retainedBytes();
    };
    count(past); count(future);
    return bytes;
}
std::size_t DocumentHistory::retainedBytes() const { return retainedBytes(current_,past_,future_); }
void DocumentHistory::trim(const DocumentPtr& current, std::vector<Entry>& past, std::vector<Entry>& future) const {
    while(past.size()+future.size()>entryLimit_ || retainedBytes(current,past,future)>retainedByteLimit_) {
        if(!past.empty()) past.erase(past.begin());
        else if(!future.empty()) future.erase(future.begin());
        else break;
    }
}
bool DocumentHistory::commit(const EditTransaction& transaction, std::string name) {
    return commit(transaction,std::move(name),selected_);
}
void DocumentHistory::selectLayer(std::optional<Id> selected) {
    if(selected) current_->layer(*selected);
    selected_=std::move(selected);
}
bool DocumentHistory::commit(const EditTransaction& transaction, std::string name,std::optional<Id> selected) {
    if(transaction.input()!=current_) throw std::logic_error("Stale edit transaction");
    if(nextRevision_==std::numeric_limits<std::int64_t>::max()) throw std::overflow_error("Revision exhausted");
    auto next=transaction.finish(nextRevision_+1);
    if(next==current_) return false;
    if(selected && std::none_of(next->layers().begin(),next->layers().end(),[&](const auto& node){return node.id==*selected;})) selected.reset();
    if(!selected && !next->layers().empty()) selected=next->layers().back().id;
    auto past=past_;
    std::vector<Entry> future;
    past.push_back({std::move(name),current_,next,selected_,selected});
    trim(next,past,future);
    // All potentially throwing allocations precede publication.
    past_.swap(past); future_.swap(future); current_=std::move(next); selected_=std::move(selected); ++nextRevision_;
    for(const auto& node:current_->layers()) for(const auto& raster:{node.raster,node.mask}) if(raster) nextRevision_=std::max(nextRevision_,raster->revision);
    return true;
}
bool DocumentHistory::undo() {
    if(!canUndo()) return false;
    auto past=past_, future=future_;
    auto next=past.back().before;
    auto selected=past.back().selectedBefore;
    future.push_back(past.back()); past.pop_back();
    trim(next,past,future);
    past_.swap(past); future_.swap(future); current_=std::move(next); selected_=std::move(selected);
    return true;
}
bool DocumentHistory::redo() {
    if(!canRedo()) return false;
    auto past=past_, future=future_;
    auto next=future.back().after;
    auto selected=future.back().selectedAfter;
    past.push_back(future.back()); future.pop_back();
    trim(next,past,future);
    past_.swap(past); future_.swap(future); current_=std::move(next); selected_=std::move(selected);
    return true;
}
void DocumentHistory::markSaved(const DocumentPtr& captured) {
    if(!captured || captured->id!=current_->id || captured->revision>nextRevision_)
        throw std::invalid_argument("Saved snapshot belongs to another document or revision range");
    savedRevision_=captured->revision;
}
}
