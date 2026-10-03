#include "core/document.h"
#include "core/resources.h"
#include "rendering/mip_cache.h"
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <source_location>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace compositor;
using namespace compositor::engine;
namespace {
constexpr std::uint64_t headroom=64,abundant=1024*1024;
void expect(bool condition,const char* message) {if(!condition) throw std::runtime_error(message);}
template<class Error,class Action> void rejects(Action action,const char* message) {
    bool rejected=false;
    try {action();} catch(const Error&) {rejected=true;}
    expect(rejected,message);
}
void ledger(const MemoryAdmission& memory,std::uint64_t committed,
            const std::source_location location=std::source_location::current()) {
    const auto state=memory.snapshot();
    if(state.pendingCpu==0 && state.pendingGpu==0 && state.committedGpu==0 && state.committedCpu==committed) return;
    std::ostringstream message;
    message<<location.function_name()<<':'<<location.line()<<": memory ledger expected CPU pending=0 committed="<<committed
        <<", GPU pending=0 committed=0; actual CPU pending="<<state.pendingCpu<<" committed="<<state.committedCpu
        <<", GPU pending="<<state.pendingGpu<<" committed="<<state.committedGpu;
    throw std::runtime_error(message.str());
}
struct Environment {
    std::shared_ptr<MemorySample> sample=std::make_shared<MemorySample>(MemorySample{abundant,0});
    std::shared_ptr<RuntimeResources> runtime;
    explicit Environment(std::size_t tileLimit=64*1024,std::uint64_t applicationLimit=128*1024) {
        auto limits=ResourceLimits::forMachine(8ull*1024*1024*1024,1);
        limits.canonicalTiles=tileLimit;limits.applicationMemory=applicationLimit;limits.systemHeadroom=headroom;
        runtime=std::make_shared<RuntimeResources>(limits,[probe=sample]{return *probe;});
    }
    void restore() {*sample={abundant,0};}
};
std::vector<Pixel> pixels(int width,int height,bool alternate=false) {
    std::vector<Pixel> result;
    for(int y=0;y<height;++y) for(int x=0;x<width;++x) {
        const bool bright=((x+y)%2==0)!=alternate;
        result.push_back(bright ? Pixel{1,.25f,0,1} : Pixel{0,.5f,1,1});
    }
    return result;
}
std::size_t denseCharge() {
    TileStore measuring(64*1024);
    return measuring.create(2,2,pixels(2,2))->retainedBytes();
}
std::size_t uniformCharge() {
    TileStore measuring(64*1024);
    return measuring.constant(2,2,{1,0,0,1})->retainedBytes();
}
void paint(DocumentHistory& history,TileStore& store,const std::vector<Pixel>& values) {
    auto edit=history.begin();edit.write(store,{0,0},values);
    expect(history.commit(edit,"Paint"),"Fixture paint did not create a document revision");
}
std::vector<std::uint8_t> bytes(const DocumentHistory& history) {
    return history.current()->singleLayer().raster->tiles.at({0,0})->canonicalBytes();
}
struct HistoryState {
    DocumentPtr document;std::optional<Id> selected;std::string undoName;
    bool dirty,undo,redo;std::vector<std::uint8_t> pixels;
    explicit HistoryState(const DocumentHistory& history)
        :document(history.current()),selected(history.selectedLayer()),undoName(history.undoName()),
         dirty(history.dirty()),undo(history.canUndo()),redo(history.canRedo()),pixels(bytes(history)) {}
    void unchanged(const DocumentHistory& history) const {
        expect(history.current()==document && history.selectedLayer()==selected && history.undoName()==undoName &&
            history.dirty()==dirty && history.canUndo()==undo && history.canRedo()==redo && bytes(history)==pixels,
            "Rejected allocation changed canonical pixels, publication, selection or dirty/undo state");
    }
};
void sharedPoolAcrossDocuments() {
    const auto charge=denseCharge();Environment environment(2*charge);
    const auto poolForFirst=environment.runtime->tiles,poolForSecond=environment.runtime->tiles;
    auto first=std::make_unique<DocumentHistory>(blankDocument(2,2));
    auto second=std::make_unique<DocumentHistory>(blankDocument(2,2));
    DocumentHistory third(blankDocument(2,2));
    paint(*first,*poolForFirst,pixels(2,2));paint(*second,*poolForSecond,pixels(2,2,true));
    expect(poolForFirst->usedBytes()==2*charge,"Separate documents escaped the shared canonical pool ceiling");
    ledger(*environment.runtime->memory,2*charge);
    {
        auto edit=third.begin();
        rejects<std::length_error>([&]{edit.write(*environment.runtime->tiles,{0,0},pixels(2,2));},
            "Third document bypassed the shared tile pool ceiling");
        rejects<std::logic_error>([&]{third.commit(edit,"Rejected paint");},"Denied tile write published a transaction");
    }
    expect(!third.dirty() && !third.canUndo() && third.current()->singleLayer().raster->tiles.empty(),
        "Denied document acquired pixels or history");
    ledger(*environment.runtime->memory,2*charge);
    const auto firstBytes=bytes(*first);
    expect(first->undo(),"Shared-pool fixture could not undo");
    expect(poolForFirst->usedBytes()==2*charge,"Undo released pixels still retained by redo");
    second.reset();
    expect(poolForFirst->usedBytes()==charge,"Closing a document failed to release its unique canonical tiles");
    ledger(*environment.runtime->memory,charge);
    paint(third,*environment.runtime->tiles,pixels(2,2));
    expect(first->redo() && bytes(*first)==firstBytes,"Shared-pool pressure damaged retained redo pixels");
    ledger(*environment.runtime->memory,2*charge);
}
void refusalPreservesCanonicalHistory() {
    Environment environment;auto& store=*environment.runtime->tiles;
    DocumentHistory history(blankDocument(2,2));
    paint(history,store,pixels(2,2));history.markSaved(history.current());
    paint(history,store,pixels(2,2,true));
    expect(history.undo() && !history.dirty() && history.canUndo() && history.canRedo(),"History fixture did not retain saved/redo state");
    const auto used=store.usedBytes();
    for(unsigned round=0;round<2;++round) {
        for(unsigned failure=0;failure<4;++failure) {
            environment.restore();
            if(failure==0) environment.sample->available.reset();
            else if(failure==1) environment.sample->resident.reset();
            else if(failure==2) environment.sample->available=headroom;
            else environment.sample->resident=environment.runtime->limits.applicationMemory;
            const HistoryState before(history);
            {
                auto edit=history.begin();
                rejects<std::length_error>([&]{edit.write(store,{0,0},pixels(2,2,round==0));},
                    "Missing probe or exhausted envelope/headroom admitted new canonical backing");
                rejects<std::logic_error>([&]{history.commit(edit,"Rejected paint");},"Failed admission published document history");
            }
            before.unchanged(history);
            expect(store.usedBytes()==used,"Denied tile write leaked or discarded canonical pool bytes");
            ledger(*environment.runtime->memory,used);
        }
        environment.restore();
        if(round==0) expect(history.redo() && history.dirty(),"Admission refusal destroyed redo or saved revision tracking");
    }
    const auto retained=bytes(history);
    expect(history.undo() && !history.dirty() && history.redo() && bytes(history)==retained,
        "Canonical history could not recover after memory availability returned");
}
void validationAndLocalBudgetRollback() {
    Environment environment;auto& store=*environment.runtime->tiles;
    const auto dense=store.create(2,2,pixels(2,2));
    const auto uniform=store.constant(2,2,{1,0,0,1});
    const auto retained=dense->canonicalBytes();const auto used=store.usedBytes();
    ledger(*environment.runtime->memory,used);
    rejects<std::invalid_argument>([&]{store.create(2,2,pixels(1,2));},"Malformed pixel count allocated a tile");
    rejects<std::invalid_argument>([&]{store.constant(257,1,{1,0,0,1});},"Invalid tile dimensions allocated backing");
    auto invalid=pixels(2,2);invalid.back().r=std::numeric_limits<float>::quiet_NaN();
    rejects<std::domain_error>([&]{store.create(2,2,invalid);},"Nonfinite tile input was accepted");
    auto malformed=retained;malformed[0]=0;malformed[1]=0x80;
    rejects<std::domain_error>([&]{store.fromCanonical(2,2,malformed);},"Noncanonical decoded samples were accepted");
    malformed.pop_back();
    rejects<std::invalid_argument>([&]{store.fromCanonical(2,2,malformed);},"Truncated decoded samples allocated a tile");
    TileStore tooSmall(1,environment.runtime->memory);
    rejects<std::length_error>([&]{tooSmall.create(2,2,pixels(2,2));},"Local tile budget was ignored");
    expect(tooSmall.usedBytes()==0 && store.usedBytes()==used && dense->canonicalBytes()==retained && uniform->uniform(),
        "Validation/local-budget failure changed existing backing");
    ledger(*environment.runtime->memory,used);

    // A transaction that allocated its first tile must still discard all work
    // after a later validation or local-budget failure.
    for(const bool localFailure:{false,true}) {
        TileStore transactionStore(localFailure ? uniformCharge() : 64*1024,environment.runtime->memory);
        DocumentHistory history(blankDocument(512,1));
        const auto original=history.current();
        {
            auto edit=history.begin();
            edit.write(transactionStore,{0,0},std::vector<Pixel>(256,Pixel{1,0,0,1}));
            const auto temporary=transactionStore.usedBytes();
            expect(temporary==uniformCharge(),"First transaction tile did not retain its backing");
            if(localFailure) rejects<std::length_error>([&]{edit.write(transactionStore,{1,0},std::vector<Pixel>(256,Pixel{0,1,0,1}));},
                "Second transaction tile escaped its local budget");
            else rejects<std::invalid_argument>([&]{edit.write(transactionStore,{1,0},std::vector<Pixel>(1,Pixel{0,1,0,1}));},
                "Second transaction tile ignored invalid dimensions");
            rejects<std::logic_error>([&]{history.commit(edit,"Incomplete paint");},"Partially failed tile transaction was published");
            ledger(*environment.runtime->memory,used+temporary);
        }
        expect(transactionStore.usedBytes()==0 && history.current()==original && !history.dirty() && !history.canUndo() &&
            original->singleLayer().raster->pixel(0,0)==Pixel{} && original->singleLayer().raster->pixel(511,0)==Pixel{},
            "Failed transaction retained tiles or changed original pixels/history");
        ledger(*environment.runtime->memory,used);
    }
}
void tileLifetimeOutlivesOwners() {
    Environment environment;const auto memory=environment.runtime->memory;
    std::weak_ptr<RuntimeResources> owner=environment.runtime;
    std::weak_ptr<TileStore> store=environment.runtime->tiles;
    auto history=std::make_unique<DocumentHistory>(blankDocument(2,2));
    paint(*history,*environment.runtime->tiles,pixels(2,2));
    auto first=history->current();const auto firstPixels=bytes(*history);
    std::weak_ptr<const Tile> firstTile=first->singleLayer().raster->tiles.at({0,0});
    paint(*history,*environment.runtime->tiles,pixels(2,2,true));
    auto second=history->current();const auto secondPixels=bytes(*history);
    std::weak_ptr<const Tile> secondTile=second->singleLayer().raster->tiles.at({0,0});
    const auto total=environment.runtime->tiles->usedBytes();
    environment.runtime.reset();
    expect(owner.expired() && store.expired() && !firstTile.expired() && !secondTile.expired(),
        "Tile backing depended on its runtime/store owner's lifetime");
    ledger(*memory,total);
    expect(history->undo() && bytes(*history)==firstPixels && history->redo() && bytes(*history)==secondPixels,
        "History lost canonical data after runtime/store destruction");
    history.reset();ledger(*memory,total);
    first.reset();
    expect(firstTile.expired() && !secondTile.expired(),"Final old-snapshot release did not destroy its unique tile");
    const auto remaining=second->singleLayer().raster->tiles.at({0,0})->retainedBytes();
    ledger(*memory,remaining);
    second.reset();expect(secondTile.expired(),"Final canonical snapshot did not release tile backing");ledger(*memory,0);
}
std::shared_ptr<const RasterSnapshot> raster(TileStore& store,bool alternate=false) {
    auto tile=store.create(8,8,pixels(8,8,alternate));
    return std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,8,8},PackedPixel{},TileMap{{{0,0},std::move(tile)}});
}
FilteredRegion reduced(const std::shared_ptr<const RasterSnapshot>& source) {
    auto result=halveRegion([&](auto x,auto y){return source->pixel(x,y);},8,8,{0,0,4,4});
    expect(result.has_value(),"Direct reduction reference was canceled");return std::move(*result);
}
void exactMip(MipCache& cache,const std::shared_ptr<const RasterSnapshot>& source,const FilteredRegion& reference) {
    for(int y=0;y<4;++y) for(int x=0;x<4;++x) {
        const auto value=cache.pixel(source,1,x,y);
        expect(value && *value==reference.pixels[static_cast<std::size_t>(y*4+x)],"Mip pressure changed full reduced output samples");
    }
}
void mipAdmissionEvictionAndRollback() {
    Environment environment;const auto memory=environment.runtime->memory;
    auto first=raster(*environment.runtime->tiles),second=raster(*environment.runtime->tiles,true);
    const auto firstBytes=first->tiles.at({0,0})->canonicalBytes(),secondBytes=second->tiles.at({0,0})->canonicalBytes();
    const auto firstReference=reduced(first),secondReference=reduced(second);
    const auto canonical=environment.runtime->tiles->usedBytes();
    MipCache cache(2*MipCache::slotBytes,memory);
    exactMip(cache,first,firstReference);
    const auto entryCharge=memory->snapshot().committedCpu-canonical;
    expect(cache.residentPieces()==1 && entryCharge>=cache.residentBytes() && cache.builtPieces()==1,
        "Mip entry was not charged or cache hits rebuilt it");
    ledger(*memory,canonical+entryCharge);
    // Only one real derived entry fits, despite a two-piece local cache. The
    // measured charge avoids relying on private cache metadata layout.
    environment.sample->available=headroom+canonical+entryCharge;
    exactMip(cache,second,secondReference);
    expect(cache.residentPieces()==1 && cache.builtPieces()==2,"Shared headroom denial did not evict and retry the derived cache");
    ledger(*memory,canonical+entryCharge);
    exactMip(cache,first,firstReference);
    expect(cache.residentPieces()==1 && cache.builtPieces()==3,"Evicted mip source was not rebuilt with exact output");
    ledger(*memory,canonical+entryCharge);
    environment.sample->available=headroom+canonical;
    rejects<std::length_error>([&]{cache.pixel(second,1,0,0);},"Mip allocation bypassed exhausted shared headroom");
    expect(cache.residentPieces()==0,"Denied mip miss retained obsolete derived storage");ledger(*memory,canonical);
    environment.restore();exactMip(cache,second,secondReference);cache.clear();ledger(*memory,canonical);
    int checks=0;
    expect(!cache.pixel(first,1,0,0,[&]{return ++checks>3;}) && checks>3,"Canceled mip computation published partial output");
    expect(cache.residentPieces()==0,"Canceled mip computation published a cache entry");ledger(*memory,canonical);
    expect(first->tiles.at({0,0})->canonicalBytes()==firstBytes && second->tiles.at({0,0})->canonicalBytes()==secondBytes,
        "Derived-cache pressure altered canonical source samples");
}
void mipPruneUsesCurrentGraphIdentity() {
    Environment environment;const auto memory=environment.runtime->memory;
    auto absent=raster(*environment.runtime->tiles);
    // Transfer ownership explicitly: a nested initializer-list pair can copy
    // its shared_ptr argument even when that argument uses std::move.
    TileMap replacementTiles;
    replacementTiles.emplace(TileCoord{0,0},environment.runtime->tiles->create(8,8,pixels(8,8,true)));
    auto present=std::make_shared<const RasterSnapshot>(absent->id,absent->extent,PackedPixel{},
        std::move(replacementTiles),absent->revision);
    const auto reference=reduced(present);
    const auto presentCharge=present->tiles.at({0,0})->retainedBytes();
    std::weak_ptr<const RasterSnapshot> oldSource=absent;
    std::weak_ptr<const Tile> oldTile=absent->tiles.at({0,0});
    std::weak_ptr<const RasterSnapshot> currentSource=present;
    std::weak_ptr<const Tile> currentTile=present->tiles.at({0,0});
    MipCache cache(4*MipCache::slotBytes,memory);
    exactMip(cache,absent,reduced(absent));exactMip(cache,present,reference);
    const auto canonical=environment.runtime->tiles->usedBytes();
    const auto twoEntries=memory->snapshot().committedCpu-canonical;
    expect(cache.residentPieces()==2 && twoEntries>0,"Prune fixture did not cache both source identities");
    absent.reset();expect(!oldSource.expired(),"Mip cache failed to retain its source snapshot");
    LayerNode hidden{Id::generate()};hidden.raster=present;hidden.visible=false;
    auto current=std::make_shared<const DocumentSnapshot>(Id::generate(),8,8,72,std::vector<LayerNode>{hidden});
    hidden.raster.reset();
    cache.prune(*current);
    expect(oldSource.expired() && oldTile.expired() && cache.residentPieces()==1,
        "Prune retained an absent source with matching ID/revision or removed a present hidden source");
    ledger(*memory,presentCharge+twoEntries/2);
    const auto built=cache.builtPieces();exactMip(cache,present,reference);
    expect(cache.builtPieces()==built,"Prune discarded a source still in the current graph");
    cache.clear();expect(cache.residentPieces()==0,"Cache clear retained mip pieces");ledger(*memory,presentCharge);
    current.reset();
    expect(!currentSource.expired() && !currentTile.expired(),"Clearing the document prematurely released its external source owner");
    present.reset();
    expect(currentSource.expired() && currentTile.expired(),"Final source release retained canonical tile ownership");
    ledger(*memory,0);
}
}
int main() {
    try {
        sharedPoolAcrossDocuments();refusalPreservesCanonicalHistory();validationAndLocalBudgetRollback();
        tileLifetimeOutlivesOwners();mipAdmissionEvictionAndRollback();mipPruneUsesCurrentGraphIdentity();
        std::cout<<"shared canonical pools, atomic refusal/rollback, snapshot lifetimes and charged mip eviction/pruning passed\n";
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
