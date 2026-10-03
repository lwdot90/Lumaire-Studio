#include "core/sampling.h"
#include "core/document.h"
#include <cfenv>
#include <bit>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <set>
#include <thread>

using namespace compositor::engine;
namespace {
void expect(bool pass, const char* message) {
    if(!pass) { std::cerr<<message<<'\n'; std::exit(1); }
}
template<class F> void rejects(F action, const char* message) {
    try { action(); } catch(const std::exception&) { return; }
    expect(false,message);
}
bool near(float a, float b) { return std::abs(a-b)<0.000001f; }
void halfTests() {
    // Exhaust every finite half encoding, including all subnormals/signs.
    for(unsigned bits=0;bits<65536;++bits) {
        const auto h=static_cast<Half>(bits);
        if((h&0x7c00)==0x7c00) { rejects([&]{fromHalf(h);},"nonfinite half accepted"); continue; }
        expect(toHalf(fromHalf(h))==(h==0x8000 ? 0 : h),"exhaustive binary16 roundtrip");
        const auto exponent=(h>>10)&31,fraction=h&1023;
        const float magnitude=exponent==0 ? std::ldexp(static_cast<float>(fraction),-24)
            : std::ldexp(static_cast<float>(1024+fraction),exponent-25);
        const float reference=(h&0x8000) ? -magnitude : magnitude;
        expect(std::bit_cast<std::uint32_t>(fromHalf(h))==std::bit_cast<std::uint32_t>(reference),"binary16 decode differs from independent ldexp oracle");
    }
    // Check every positive adjacent-half midpoint, and the float immediately
    // on either side, against ties-to-even. Negative symmetry is independent.
    for(unsigned bits=0;bits<0x7bff;++bits) {
        const auto lo=static_cast<Half>(bits), hi=static_cast<Half>(bits+1);
        const float mid=(fromHalf(lo)+fromHalf(hi))*0.5f;
        const auto rounded=(bits&1) ? hi : lo;
        expect(toHalf(mid)==rounded,"half midpoint ties-to-even");
        expect(toHalf(std::nextafter(mid,0.f))==lo,"below half midpoint");
        expect(toHalf(std::nextafter(mid,std::numeric_limits<float>::infinity()))==hi,"above half midpoint");
        expect(toHalf(-mid)==(rounded ? (rounded|0x8000) : 0),"negative half midpoint");
    }
    for(int mode:{FE_UPWARD,FE_DOWNWARD,FE_TOWARDZERO,FE_TONEAREST}) {
        expect(std::fesetround(mode)==0,"set rounding mode");
        expect(toHalf(1.00048828125f)==0x3c00,"half conversion independent of rounding mode");
    }
    std::fesetround(FE_TONEAREST);
    for(float invalid:{65505.f,-65505.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})
        rejects([&]{toHalf(invalid);},"invalid float accepted");
    expect(pack({4,-2,1,0})==PackedPixel{},"zero alpha canonicalization");
    expect(pack({4,-2,1,std::ldexp(1.f,-26)})==PackedPixel{},"rounded-zero alpha canonicalization");
    expect(pack({-2,4,1,.5f})[0]==toHalf(-2),"extended RGB retained");
    rejects([]{pack({0,0,0,1.1f});},"alpha range checked");
    rejects([]{validateCanonical({0x8000,0,0,0x3c00});},"persisted negative zero rejected");
    rejects([]{validateCanonical({1,0,0,0});},"persisted transparent RGB rejected");
}
void geometryTests() {
    expect(floorTile(-257)==-2 && floorTile(-256)==-1 && floorTile(-1)==-1 && floorTile(256)==1,"signed floor grid");
    const Extent asset{-1,-2,258,259};
    expect(tileExtent(asset,{-1,-1})==Extent{-1,-2,1,2},"negative origin edge tile");
    expect(tileExtent(asset,{0,0})==Extent{0,0,256,256},"full interior tile");
    expect(tileExtent(asset,{1,1})==Extent{256,256,1,1},"last edge tile");
    rejects([&]{tileExtent(asset,{2,0});},"out of bounds tile rejected");
    constexpr auto low=std::numeric_limits<std::int64_t>::min(), high=std::numeric_limits<std::int64_t>::max();
    expect(tileExtent({low,low,1,1},{floorTile(low),floorTile(low)})==Extent{low,low,1,1},"minimum origin arithmetic");
    expect(tileExtent({high-1,high-1,1,1},{floorTile(high-1),floorTile(high-1)})==Extent{high-1,high-1,1,1},"maximum origin arithmetic");
    rejects([&]{Extent{high,0,1,1}.validate();},"endpoint overflow rejected");
    rejects([]{Extent{0,0,30000,30000}.validate();},"aggregate extent cap");
    rejects([]{Extent{0,0,-1,1}.validate();},"negative dimensions");
    const auto id=Id::generate();
    expect(Id(id.text())==id && id.text()[14]=='4',"canonical UUID roundtrip");
    rejects([]{Id("00000000-0000-0000-0000-00000000000A");},"uppercase UUID rejected");
}
void tileTests() {
    TileStore store(4*1024*1024);
    for(auto origin:{std::numeric_limits<std::int64_t>::min(),std::int64_t(-257),std::int64_t(-1),
                     std::int64_t(255),std::numeric_limits<std::int64_t>::max()-2}) {
        const Extent extent{origin,origin,2,2};TileMap edgeTiles;
        for(auto y=floorTile(origin);y<=floorTile(origin+1);++y)
            for(auto x=floorTile(origin);x<=floorTile(origin+1);++x) {
                const auto area=tileExtent(extent,{x,y});
                edgeTiles[{x,y}]=store.constant(static_cast<int>(area.width),static_cast<int>(area.height),{.5f,0,0,1});
            }
        RasterSnapshot edge(Id::generate(),extent,{},edgeTiles);
        for(int y=0;y<2;++y) for(int x=0;x<2;++x)
            expect(edge.pixel(origin+x,origin+y)==Pixel{.5f,0,0,1},"Validated raster tap at signed/partial grid edge");
        expect(edge.pixel(origin+2,origin)==Pixel{},"Validated raster exterior is transparent");
    }
    {
        auto tile=store.constant(256,256,{.5f,.25f,0,1});
        expect(tile->uniform() && tile->retainedBytes()<1024,"uniform tile avoids payload allocation");
        expect(tile->linearPixel(12,24)==unpack(tile->pixel(12,24)),"Uniform decoded color matches canonical source");
        rejects([&]{tile->linearPixel(-1,0);},"Decoded uniform tile bounds checked");
        const auto bytes=tile->canonicalBytes();
        expect(bytes.size()==524288 && bytes[0]==0 && bytes[1]==0x38 && bytes[6]==0 && bytes[7]==0x3c,"canonical little-endian layout");
        auto copy=store.fromCanonical(256,256,bytes);
        expect(copy->samePixels(*tile) && copy->version()!=tile->version(),"byte roundtrip and immutable versions");
        auto bad=bytes; bad[0]=1; bad[1]=0x7c;
        const auto before=store.usedBytes();
        rejects([&]{store.fromCanonical(256,256,bad);},"nonfinite decoded tile rejected");
        expect(store.usedBytes()==before,"invalid input consumes no tile budget");
        rejects([&]{store.fromCanonical(1,1,bytes);},"incorrect decoded length rejected");
        rejects([&]{tile->pixel(256,0);},"tile pixel bounds checked");
    }
    expect(store.usedBytes()==0,"tile budget released after last reference");
    std::vector<Pixel> pixels(256*256,Pixel{0,0,0,1}); pixels[1]={1,0,0,1};
    auto tile=store.create(256,256,pixels);
    expect(!tile->uniform() && tile->retainedBytes()>=524288,"dense tile payload charged");
    for(int x:{0,1,255}) expect(tile->linearPixel(x,0)==unpack(tile->pixel(x,0)),"Dense decoded pixels match canonical source");
    expect(store.fromCanonical(256,256,tile->canonicalBytes())->samePixels(*tile),"dense byte roundtrip");
    TileStore tiny(1024);
    rejects([&]{tiny.create(256,256,pixels);},"dense allocation fails before exceeding budget");
    expect(tiny.usedBytes()==0,"failed allocation reservation released");
    auto surviving=TileStore(1024).constant(1,1,{1,0,0,1});
    expect(surviving->pixel(0,0)==pack({1,0,0,1}),"snapshot survives tile-store lifetime");
}
void historyTests() {
    TileStore store(8*1024*1024);
    const auto initial=blankDocument(257,256);
    DocumentHistory history(initial);
    auto edit=history.begin();
    auto red=store.constant(256,256,{1,0,0,1});
    edit.replace({0,0},red);
    expect(initial->singleLayer().raster->tiles.empty(),"transaction leaves input immutable");
    expect(history.commit(edit,"Fill") && history.dirty(),"commit creates dirty revision");
    const auto first=history.current();
    history.markSaved(first);
    expect(!history.dirty() && history.undoName()=="Fill","saved revision and undo name");
    edit=history.begin();
    edit.replace({0,0},store.constant(256,256,{1,0,0,1}));
    edit.replace({1,0},store.constant(1,256,{0,1,0,1}));
    expect(history.commit(edit,"Edge"),"edge tile committed");
    const auto second=history.current();
    expect(second->singleLayer().raster->tiles.at({0,0})==red,"untouched tile pointer shared");
    expect(first->singleLayer().raster->tiles.size()==1,"previous snapshot unchanged");
    history.markSaved(first);
    expect(history.dirty(),"completion of older captured save retains current dirty state");
    expect(history.undo() && history.current()==first && !history.dirty(),"undo restores exact saved snapshot");
    auto noop=history.begin();
    noop.replace({0,0},store.constant(256,256,{1,0,0,1}));
    expect(!history.commit(noop,"No-op") && history.canRedo(),"equal pixels preserve redo");
    expect(history.redo() && history.current()==second,"redo restores exact snapshot");
    rejects([&]{history.commit(noop,"Stale");},"stale edit rejected");
    expect(history.undo(),"undo before branching");
    edit=history.begin(); edit.replace({0,0},store.constant(256,256,{0,0,1,1}));
    history.commit(edit,"Branch");
    expect(!history.canRedo() && history.current()->revision>second->revision,"branch gets unique revision and clears redo");
    const auto before=history.current();
    auto failed=history.begin(); failed.replace({0,0},red);
    rejects([&]{failed.replace({1,0},red);},"invalid edge replacement rejected");
    rejects([&]{history.commit(failed,"Partial");},"poisoned transaction cannot partially publish");
    expect(history.current()==before,"failed edit restores original snapshot");
    TileStore tiny(1);
    failed=history.begin(); failed.replace({0,0},red);
    std::vector<Pixel> pixels(256,Pixel{0,1,0,1});
    rejects([&]{failed.write(tiny,{1,0},pixels);},"allocation failure inside transaction");
    rejects([&]{history.commit(failed,"Partial allocation");},"allocation failure poisons transaction");
    expect(history.current()==before,"allocation failure preserves revision");
    pixels[0].r=std::numeric_limits<float>::quiet_NaN();
    failed=history.begin();
    rejects([&]{failed.write(store,{1,0},pixels);},"nonfinite operation rejected");
    rejects([&]{history.commit(failed,"Invalid output");},"nonfinite output cannot publish");
    DocumentHistory bounded(initial,1);
    auto step=bounded.begin(); step.replace({0,0},red); bounded.commit(step,"One");
    step=bounded.begin(); step.replace({0,0},store.constant(256,256,{0,0,1,1})); bounded.commit(step,"Two");
    expect(bounded.retainedBytes()==red->retainedBytes(),"history excludes live shared tiles and counts retained versions once");
    expect(bounded.undo() && !bounded.canUndo(),"history entry bound enforced");
    DocumentHistory noRetained(before,100,0);
    step=noRetained.begin(); step.replace({0,0},red); noRetained.commit(step,"Budget");
    expect(!noRetained.canUndo() && noRetained.retainedBytes()==0,"history retained-byte bound enforced");
    rejects([&]{history.markSaved(blankDocument(1,1));},"foreign saved document rejected");
}
void restoredPixelEditTests() {
    TileStore store(64*1024);
    std::vector<Pixel> left(256,Pixel{1,0,0,1}),right(256,Pixel{0,0,1,1});
    left[63]={0,1,0,1}; right[255]={.5f,.25f,0,1};
    TileMap tiles;
    tiles.emplace(TileCoord{0,0},store.create(256,1,left));
    tiles.emplace(TileCoord{1,0},store.create(256,1,right));
    auto raster=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,513,1},PackedPixel{},std::move(tiles));
    const auto initial=std::make_shared<const DocumentSnapshot>(Id::generate(),Id::generate(),513,1,72,raster);
    DocumentHistory history(initial);
    auto edit=history.begin(); edit.replace({2,0},store.constant(1,1,{0,1,0,1}));
    expect(history.commit(edit,"Paint edge"),"edge edit creates a redo candidate");
    const auto painted=history.current();
    expect(history.undo() && history.current()==initial && !history.dirty(),"undo restores saved input before temporary writes");

    auto changedLeft=left,changedRight=right;
    changedLeft[127]={0,0,1,1}; changedRight[3]={1,0,0,1};
    edit=history.begin();
    edit.write(store,{0,0},changedLeft);
    edit.write(store,{1,0},changedRight);
    edit.replace({0,0},store.constant(256,1,{}));
    edit.replace({2,0},store.constant(1,1,{1,0,0,1}));
    edit.write(store,{1,0},right);
    edit.write(store,{0,0},left);
    edit.replace({2,0},store.constant(1,1,{}));
    expect(edit.finish(initial->revision+1)==initial,"restoring dense pixels and sparse defaults returns the original snapshot");
    expect(!history.commit(edit,"Temporary writes") && history.current()==initial && !history.dirty(),
        "restored multi-coordinate edit creates no revision or dirty state");
    expect(!history.canUndo() && history.canRedo(),"restored multi-coordinate edit preserves redo without adding an undo entry");
    expect(history.redo() && history.current()==painted && history.undoName()=="Paint edge"
        && history.current()->singleLayer().raster->pixel(512,0)==Pixel{0,1,0,1},
        "redo after restored writes retains the previously painted pixels");
}
void concurrentTileTests() {
    TileStore store(64*1024);
    std::array<std::array<std::int64_t,500>,4> versions{};
    std::vector<std::thread> workers;
    for(std::size_t worker=0;worker<versions.size();++worker) workers.emplace_back([&,worker] {
        for(auto& version:versions[worker]) {
            const auto tile=store.constant(256,256,{1,0,0,1});
            version=tile->version();
            expect(tile->pixel(255,255)==pack({1,0,0,1}),"concurrent tile read");
        }
    });
    for(auto& worker:workers) worker.join();
    std::set<std::int64_t> unique;
    for(const auto& worker:versions) unique.insert(worker.begin(),worker.end());
    expect(unique.size()==2000 && store.usedBytes()==0,"concurrent versions unique and reservations balanced");
}
void scalarTests() {
    for(float value:{-2.f,-.04045f,0.f,.003f,.5f,1.f,2.f})
        expect(std::abs(encodeSrgb(decodeSrgb(value))-value)<0.000002f,"extended odd-symmetric sRGB roundtrip");
    const auto linear=fromStraightSrgb({.5f,.5f,.5f,.5f});
    expect(near(linear.r,.10702057f) && near(linear.a,.5f),"decode straight before premultiplication");
    expect(near(toStraightSrgb(linear).r,.5f),"unpremultiply before encoding");
    const auto over=sourceOver({.5f,0,0,.5f},{0,0,1,1});
    expect(over==Pixel{.5f,0,.5f,1},"linear premultiplied source-over");
    TileStore store(1024*1024);
    TileMap tiles;
    tiles[{0,0}]=store.constant(256,1,{1,0,0,1});
    tiles[{1,0}]=store.constant(1,1,{0,0,1,1});
    RasterSnapshot raster(Id::generate(),{0,0,257,1},{},tiles);
    expect(sample(raster,256,.5,Sampling::Nearest)==Pixel{0,0,1,1},"nearest boundary chooses right pixel");
    expect(sample(raster,256,.5,Sampling::Bilinear)==Pixel{.5f,0,.5f,1},"bilinear reads across tile boundary");
    expect(sample(raster,0,.5,Sampling::Bilinear)==Pixel{.5f,0,0,.5f},"transparent exterior participates in filtering");
    expect(sample(raster,-1,.5,Sampling::Nearest)==Pixel{},"raster exterior transparent");
    expect(sample(raster,1e100,1e100,Sampling::Nearest)==Pixel{},"far coordinates avoid conversion overflow");
    rejects([&]{sample(raster,std::numeric_limits<double>::infinity(),0,Sampling::Nearest);},"nonfinite sampling rejected");
    RasterSnapshot negative(Id::generate(),{-1,-1,1,1},pack({0,1,0,1}));
    expect(negative.pixel(-1,-1)==Pixel{0,1,0,1} && negative.pixel(0,0)==Pixel{},"missing interior uses explicit default, exterior does not");
}
}
int main() {
    halfTests(); geometryTests(); tileTests(); historyTests(); restoredPixelEditTests(); concurrentTileTests(); scalarTests();
    std::cout<<"binary16, geometry, immutable tiles, budget, transactions, history and scalar sampling passed\n";
}
