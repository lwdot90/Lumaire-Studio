#include "core/retouch.h"
#include "core/editor_commands.h"
#include "io/retouch_parameters.h"
#include "io/project_store.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <sqlite3.h>
#include <iostream>
using namespace compositor::engine;
using namespace compositor::io;
namespace {
void expect(bool v,const char* why){if(!v)throw std::runtime_error(why);}
template<class F>void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}expect(failed,"Expected refusal");}
void same(const RasterSnapshot& a,const RasterSnapshot& b){expect(a.id==b.id&&a.revision==b.revision&&a.extent==b.extent,"Exact raster identity");for(int y=0;y<8;++y)for(int x=0;x<8;++x)expect(pack(a.pixel(x,y))==pack(b.pixel(x,y)),"Exact effective bits");}
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
 QTemporaryDir dir(QCoreApplication::applicationDirPath()+"/retouch-persistence-XXXXXX");expect(dir.isValid(),"Fixture directory");TileStore tiles(32*1024*1024);std::vector<Pixel> pixels;for(int y=0;y<8;++y)for(int x=0;x<8;++x)pixels.push_back({float(x+1)/16,float(y+1)/16,.125f,1});TileMap map;map.emplace(TileCoord{0,0},tiles.create(8,8,pixels));
 LayerNode layer{Id::generate()};layer.raster=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,8,8},PackedPixel{},std::move(map));
 Selection selection{{0,0,8,8},SelectionShape::Ellipse,false,1.25};auto original=std::make_shared<const DocumentSnapshot>(Id::generate(),8,8,72,std::vector<LayerNode>{layer},0,selection);
 RetouchStroke clone;clone.sourceAnchor={1.25,1.5};clone.points={{5,5},{5.25,5.5}};clone.diameter=2;clone.hardness=.625;clone.opacity=.8125;clone.localToDocument={};clone.selection=selection;clone.canvasWidth=8;clone.canvasHeight=8;
 auto heal=clone;heal.kind=RetouchKind::Heal;heal.sourceAnchor={2,2};heal.points={{6,2}};heal.healingRadius=2;
 auto retouched=setRetouchStrokes(original,layer.id,tiles,{clone,heal}).finish(1);
 AdjustmentParameters exposure{AdjustmentKind::Exposure,.25};auto edited=setRevisableAdjustments(retouched,layer.id,tiles,{exposure}).finish(2);
 auto nodes=edited->layers();auto copy=nodes[0];copy.id=Id::generate();copy.siblingOrder=1;copy.sampling=Sampling::Lanczos;nodes.push_back(copy);edited=std::make_shared<const DocumentSnapshot>(edited->id,8,8,72,nodes,2,selection);
 const auto path=dir.filePath("retouch.cproj");auto identity=saveProject(path,edited);auto loaded=loadProject(path,tiles).document;const auto& reopened=loaded->layer(layer.id);
 expect(loaded->selection==original->selection,"Feathered document selection");expect(reopened.retouch&&reopened.retouch->strokes==std::vector<RetouchStroke>{clone,heal},"Exact frozen strokes");same(*reopened.retouch->source,*layer.raster);same(*reopened.adjustments->source,*edited->layer(layer.id).adjustments->source);same(*reopened.raster,*edited->layer(layer.id).raster);
 expect(reopened.raster==loaded->layer(copy.id).raster&&reopened.adjustments->source==loaded->layer(copy.id).adjustments->source&&reopened.retouch==loaded->layer(copy.id).retouch,"Shared intermediate and final caches and immutable stroke backing");
 auto revisedStrokes=reopened.retouch->strokes;revisedStrokes[0].opacity=.375;auto revised=setRetouchStrokes(loaded,layer.id,tiles,revisedStrokes).finish(3);auto expectedRetouch=setRetouchStrokes(original,layer.id,tiles,revisedStrokes).finish(1);auto expected=setRevisableAdjustments(expectedRetouch,layer.id,tiles,{exposure}).finish(2);for(int y=0;y<8;++y)for(int x=0;x<8;++x)expect(pack(revised->layer(layer.id).raster->pixel(x,y))==pack(expected->layer(layer.id).raster->pixel(x,y)),"Revision replay starts from original");
 auto record=retouchParameters(*reopened.retouch,*reopened.raster,reopened.adjustments.get(),reopened.sampling);auto decoded=readRetouchParameters(record,reopened.retouch->source);expect(decoded.stack.strokes==reopened.retouch->strokes,"Codec round trip");auto json=QJsonDocument::fromJson(QByteArray::fromStdString(record)).object();json.insert("policy",2);rejects([&]{readRetouchParameters(QJsonDocument(json).toJson(QJsonDocument::Compact).toStdString(),layer.raster);});
 std::stop_source stop;stop.request_stop();rejects([&]{loadProject(path,tiles,stop.get_token());});SaveOptions canceled;canceled.expected=identity;canceled.stop=stop.get_token();rejects([&]{saveProject(path,revised,canceled);});expect(fileIdentity(path)==identity,"Canceled save preserves destination");
 auto excessive=std::make_shared<RetouchStack>(*reopened.retouch);excessive->strokes[0].points.assign(8192,Coordinate{1.23456789012345,2.34567890123456});excessive->strokes.resize(1);auto longRecord=retouchParameters(*excessive,*reopened.raster,reopened.adjustments.get(),reopened.sampling);expect(longRecord.size()>65536,"Long valid stroke exceeds former cap");expect(readRetouchParameters(longRecord,excessive->source).stack.strokes==excessive->strokes,"Long valid stroke remains savable");
 auto invalidNodes=edited->layers();invalidNodes.clear();for(int i=0;i<16;++i){auto node=edited->layers()[0];node.id=Id::generate();node.siblingOrder=i;node.retouch=excessive;invalidNodes.push_back(node);}expect(longRecord.size()*16>4*1024*1024,"Aggregate retouch fixture exceeds metadata limit");auto oversized=std::make_shared<const DocumentSnapshot>(edited->id,8,8,72,invalidNodes,3,selection);SaveOptions refuse;refuse.expected=identity;rejects([&]{saveProject(path,oversized,refuse);});expect(fileIdentity(path)==identity,"Metadata refusal preserves destination");same(*loadProject(path,tiles).document->layer(layer.id).raster,*reopened.raster);
 auto admission=std::make_shared<compositor::MemoryAdmission>(64*1024*1024,0,[]{return compositor::MemorySample{UINT64_C(1024)*1024*1024,0};});std::shared_ptr<const RetouchStack> retained;
 {TileStore admittedTiles(16*1024*1024,admission);auto admittedDocument=loadProject(path,admittedTiles).document;retained=admittedDocument->layer(layer.id).retouch;expect(retained==admittedDocument->layer(copy.id).retouch,"One admitted backing for duplicate placements");}
 expect(admission->snapshot().committedCpu>=sizeof(RetouchStack)+128+retained->strokes.capacity()*sizeof(RetouchStroke),"Metadata charge survives document and TileStore destruction");expect(retained->strokes==std::vector<RetouchStroke>{clone,heal},"Backing remains readable after owner destruction");retained.reset();expect(admission->snapshot().committedCpu==0&&admission->snapshot().pendingCpu==0,"Final metadata and source release returns admission to zero");
 const auto purePath=dir.filePath("pure.cproj");saveProject(purePath,retouched);auto pure=loadProject(purePath,tiles).document;expect(pure->singleLayer().retouch&&!pure->singleLayer().adjustments,"Retouch without adjustments");same(*pure->singleLayer().raster,*retouched->singleLayer().raster);
 std::cout<<"Retouch persistence tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
