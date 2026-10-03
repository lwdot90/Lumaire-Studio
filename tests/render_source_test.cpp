#include "core/render_source.h"
#include <array>
#include <filesystem>
#include <iostream>
#include <cstdlib>

using namespace compositor;
using namespace compositor::engine;
namespace {
void check(bool okay) {if(!okay) throw std::runtime_error("Render source contract check failed");}
template<class F> void rejects(F action) {
    bool rejected=false;try {action();} catch(const std::exception&) {rejected=true;} check(rejected);
}
std::shared_ptr<const RasterSnapshot> raster(TileStore& store) {
    const std::array<Pixel,6> samples{Pixel{1,0,0,1},Pixel{0,1,0,1},Pixel{0,0,1,1},
        Pixel{0.5f,0.25f,0,1},Pixel{},Pixel{0.125f,0,0.25f,0.5f}};
    TileMap tiles;tiles.emplace(TileCoord{-1,-1},store.create(2,3,samples));
    return std::make_shared<RasterSnapshot>(Id("12345678-1234-4123-8123-123456789abc"),Extent{-2,-3,3,3},pack({0.25f,0.25f,0.25f,1}),std::move(tiles),7);
}
RenderNodeDescriptor sourceNode(std::shared_ptr<const RenderSource> source) {
    RenderNodeDescriptor result;result.id="source";result.source=std::move(source);result.region=result.source->extent();return result;
}
RenderNodeDescriptor operation(std::string id,RenderNodeKind kind,std::string dependency) {
    RenderNodeDescriptor result;result.id=std::move(id);result.kind=kind;result.dependencies={std::move(dependency)};
    result.parameterContract="pending-M0-R";result.parameters={1,2};return result;
}
void descriptorAndCoordinates() {
    TileStore store(1024*1024);auto snapshot=raster(store);
    const Affine transform{2,0,0,3,10,-20};RenderSource source(snapshot,transform);
    check(source.identity()==Id("12345678-1234-4123-8123-123456789abc") && source.revision()==7 && source.extent()==snapshot->extent);
    const auto position=source.documentToSource(transform.map({-1.5,-2.5}));check(position.x==-1.5 && position.y==-2.5);
    check(source.descriptor().supported() && source.descriptor().policyVersion=="legacy-schema-1");
    check(source.sameOutput(RenderSource(snapshot,transform)) && !source.sameOutput(RenderSource(snapshot)));
    SourceDescriptor alternate;alternate.precision=SourcePrecision::RGB16;rejects([&]{RenderSource unsupported(snapshot,{},alternate);});
    alternate={};alternate.policyVersion="photoshop";rejects([&]{RenderSource unsupported(snapshot,{},alternate);});
    rejects([&]{RenderSource invalid(snapshot,Affine{0,0,0,0,0,0});});
    rejects([&]{RenderSource invalid(nullptr);});
}
void pixelsAndOwnership() {
    TileStore store(1024*1024);auto snapshot=raster(store);std::weak_ptr<const RasterSnapshot> weak=snapshot;
    auto source=std::make_shared<RenderSource>(snapshot);SourceReadContext reader(source);
    check(reader.pixel(-2,-3)==Pixel{1,0,0,1});check(reader.pixel(-1,-1)==Pixel{0.125f,0,0.25f,0.5f});
    check(reader.pixel(0,-2)==Pixel{0.25f,0.25f,0.25f,1});check(reader.pixel(1,-2)==Pixel{});
    source.reset();snapshot.reset();check(!weak.expired());check(reader.pixel(-2,-3)==Pixel{1,0,0,1});reader.release();
    std::stop_source stop;SourceReadContext canceled(std::make_shared<RenderSource>(raster(store)),stop.get_token());
    stop.request_stop();rejects([&]{canceled.pixel(100,100);});
}
void readinessAndInvalidation() {
    TileStore store(1024*1024);auto source=std::make_shared<RenderSource>(raster(store));
    auto nodes=std::vector{sourceNode(source),operation("group",RenderNodeKind::IsolatedGroup,"source"),
        operation("effects",RenderNodeKind::EffectStack,"group")};
    RenderDependencyGraph first(nodes),same(nodes);
    check(first.readiness("source")==RenderReadiness::Ready);
    check(first.readiness("group")==RenderReadiness::UnsupportedSemantics);
    check(first.readiness("effects")==RenderReadiness::DependencyUnavailable);
    check(first.sameOutput("effects",same));
    nodes[0].revision++;RenderDependencyGraph revision(nodes);check(!first.sameOutput("effects",revision));
    nodes[0].revision--;nodes[1].parameters.push_back(3);RenderDependencyGraph parameters(nodes);
    check(!first.sameOutput("effects",parameters) && first.sameOutput("source",parameters));
    nodes[1].parameters.pop_back();nodes[0]=sourceNode(std::make_shared<RenderSource>(raster(store)));
    RenderDependencyGraph identity(nodes);check(!first.sameOutput("source",identity));
    check(!first.sameOutput("group",RenderDependencyGraph({sourceNode(source)})));
}
void graphValidation() {
    TileStore store(1024*1024);auto source=std::make_shared<RenderSource>(raster(store));auto node=sourceNode(source);
    rejects([&]{RenderDependencyGraph graph({node,node});});
    rejects([&]{RenderDependencyGraph graph({node,operation("bad",RenderNodeKind::FilterStack,"missing")});});
    auto a=operation("a",RenderNodeKind::FilterStack,"b"),b=operation("b",RenderNodeKind::FilterStack,"a");
    rejects([&]{RenderDependencyGraph graph({a,b});});
    a.dependencies={"source","source"};rejects([&]{RenderDependencyGraph graph({node,a});});
    node.halo=1;rejects([&]{RenderDependencyGraph graph({node});});
    auto filter=operation("filter",RenderNodeKind::FilterStack,"source");filter.parameterContract.clear();
    rejects([&]{RenderDependencyGraph graph({sourceNode(source),filter});});
    auto nodes=std::vector{sourceNode(source)};
    // Child-first ordering exercises cached traversal depth as well as cycle checks.
    for(unsigned i=0;i<65;++i) nodes.push_back(operation("n"+std::to_string(i),RenderNodeKind::FilterStack,
        i ? "n"+std::to_string(i-1) : "source"));
    rejects([&]{RenderDependencyGraph graph(nodes);});
}
void spillReads(const std::filesystem::path& parent) {
    auto pattern=(parent/"render-source-test-XXXXXX").string();const auto created=::mkdtemp(pattern.data());
    check(created!=nullptr);const std::filesystem::path path(created);
    struct Cleanup {std::filesystem::path path;~Cleanup(){std::error_code error;std::filesystem::remove_all(path,error);}} cleanup{path};
    auto memory=std::make_shared<MemoryAdmission>(2*1024*1024,0,[]{return MemorySample{2*1024*1024,0};});
    auto storage=std::make_shared<io::SpillStore>(path,io::SpillLimits{65536,65536,0,16,1,32});
    TileStore store(1024*1024,memory,storage);auto snapshot=raster(store);auto tile=snapshot->tiles.begin()->second;
    auto source=std::make_shared<RenderSource>(snapshot);SourceReadContext reader(source);
    check(tile->spillable());check(reader.pixel(-1,-1)==Pixel{0.125f,0,0.25f,0.5f});
    check(tile->spill()); // The existing lease remains readable after eviction.
    check(reader.pixel(-2,-3)==Pixel{1,0,0,1});reader.release();
    check(reader.pixel(-1,-1)==Pixel{0.125f,0,0.25f,0.5f});
    reader.release();check(tile->spill());
    std::stop_source stop;SourceReadContext canceled(source,stop.get_token());stop.request_stop();
    rejects([&]{canceled.pixel(-2,-3);});
}
}
int main(int argc,char** argv) {
    try {
        descriptorAndCoordinates();pixelsAndOwnership();readinessAndInvalidation();graphValidation();
        if(argc!=2) throw std::runtime_error("Provide a verified disk-backed build directory");
        spillReads(std::filesystem::absolute(argv[1]));
        std::cout<<"5 render source test groups passed\n";return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
