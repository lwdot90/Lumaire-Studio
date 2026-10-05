#include "core/retouch.h"
#include "core/editor_commands.h"
#include "core/selection_coverage.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace compositor::engine {
namespace {
constexpr std::size_t maximumDabs=262144;
void checkpoint(std::stop_token stop){if(stop.stop_requested())throw std::runtime_error("Retouch canceled");}
bool samePoint(Coordinate a,Coordinate b){return a.x==b.x && a.y==b.y;}
using Charge=std::shared_ptr<MemoryAdmission::Reservation>;
Charge metadataCharge(TileStore& store,std::uint64_t bytes){
 const auto memory=store.memoryAdmission();if(!memory)return {};
 if(bytes>std::numeric_limits<std::uint64_t>::max()-128)throw std::length_error("Retouch metadata overflow");
 return std::make_shared<MemoryAdmission::Reservation>(memory->require(bytes+128));
}
template<class T,class... Args>std::shared_ptr<T> admittedShared(Charge charge,Args&&... args){
 auto* value=new T(std::forward<Args>(args)...);if(charge)charge->commit();
 return std::shared_ptr<T>(value,[charge=std::move(charge)](T* owned){delete owned;});
}
constexpr std::uint64_t mapNodeBytes=sizeof(TileMap::value_type)+4*sizeof(void*);
std::shared_ptr<const RasterSnapshot> rasterWithCharge(const RasterSnapshot& source,TileMap tiles,std::int64_t revision,Charge charge){
 return admittedShared<RasterSnapshot>(std::move(charge),Id::generate(),source.extent,source.defaultValue,std::move(tiles),revision,source.sourceProfile);
}
bool finitePoint(Coordinate p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::abs(p.x)<=1000000&&std::abs(p.y)<=1000000;}
void validateStroke(const RetouchStroke& s){
 if((s.kind!=RetouchKind::Clone&&s.kind!=RetouchKind::Heal)||!finitePoint(s.sourceAnchor)||s.points.empty()||s.points.size()>8192||
 !std::isfinite(s.diameter)||s.diameter<1||s.diameter>4096||!std::isfinite(s.hardness)||s.hardness<0||s.hardness>1||
 !std::isfinite(s.opacity)||s.opacity<0||s.opacity>1||!std::isfinite(s.healingRadius)||s.healingRadius<1||s.healingRadius>32||
 s.canvasWidth<1||s.canvasHeight<1||s.canvasWidth>30000||s.canvasHeight>30000||std::uint64_t(s.canvasWidth)*std::uint64_t(s.canvasHeight)>100000000)
 throw std::invalid_argument("Retouch stroke exceeds editing limits");
 for(auto p:s.points)if(!finitePoint(p))throw std::invalid_argument("Invalid retouch point");
 s.localToDocument.inverse();if(s.selection)s.selection->validate(s.canvasWidth,s.canvasHeight);
}
class Reader {
 struct Entry{TileCoord coord;Tile::ReadLease lease;};
 const RasterSnapshot& source_;std::stop_token stop_;std::array<std::optional<Entry>,4> cache_;std::size_t next_=0;
 Pixel pixel(std::int64_t x,std::int64_t y){
  if(!source_.extent.contains(x,y))return {};
  const TileCoord coord{floorTile(x),floorTile(y)};const auto tile=source_.tiles.find(coord);
  if(tile==source_.tiles.end())return unpack(source_.defaultValue);
  Entry* entry=nullptr;for(auto& cached:cache_)if(cached&&cached->coord==coord){entry=&*cached;break;}
  if(!entry){checkpoint(stop_);auto& slot=cache_[next_++%cache_.size()];slot.reset();slot.emplace(Entry{coord,tile->second->read(stop_)});entry=&*slot;}
  const auto extent=tileExtent(source_.extent,coord);return entry->lease.linearPixel(static_cast<int>(x-extent.x),static_cast<int>(y-extent.y));
 }
public:
 Reader(const RasterSnapshot& source,std::stop_token stop):source_(source),stop_(stop){}
 Pixel sample(Coordinate p){
  const double x=p.x-.5,y=p.y-.5;if(!std::isfinite(x)||!std::isfinite(y)||x< -2000000||y< -2000000||x>2000000||y>2000000)return {};
  const auto ix=static_cast<std::int64_t>(std::floor(x)),iy=static_cast<std::int64_t>(std::floor(y));const float fx=static_cast<float>(x-std::floor(x)),fy=static_cast<float>(y-std::floor(y));
  const std::array<Pixel,4> values{pixel(ix,iy),pixel(ix+1,iy),pixel(ix,iy+1),pixel(ix+1,iy+1)};
  const std::array<float,4> weights{(1-fx)*(1-fy),fx*(1-fy),(1-fx)*fy,fx*fy};Pixel result;
  for(std::size_t i=0;i<4;++i){result.r+=values[i].r*weights[i];result.g+=values[i].g*weights[i];result.b+=values[i].b*weights[i];result.a+=values[i].a*weights[i];}return result;
 }
};
std::array<double,3> lowFrequency(Reader& reader,const Affine& inverse,Coordinate document,double radius){
 constexpr std::array<double,5> weights{1,4,6,4,1};std::array<double,3> sum{};double total=0;
 for(int y=0;y<5;++y)for(int x=0;x<5;++x){const auto p=reader.sample(inverse.map({document.x+(x-2)*radius/2,document.y+(y-2)*radius/2}));if(p.a<=0)continue;const double w=weights[static_cast<std::size_t>(x)]*weights[static_cast<std::size_t>(y)]*p.a;sum[0]+=w*p.r/p.a;sum[1]+=w*p.g/p.a;sum[2]+=w*p.b/p.a;total+=w;}
 if(total>0)for(auto& value:sum)value/=total;
 return sum;
}
double tip(double distance,double radius,double hardness){if(distance>=radius)return 0;if(hardness==1||distance<=radius*hardness)return 1;const double u=(distance/radius-hardness)/(1-hardness);return std::max(0.,(std::exp(-2.5*u*u)-std::exp(-2.5))/(1-std::exp(-2.5)));}
std::shared_ptr<const RasterSnapshot> applyStroke(const std::shared_ptr<const RasterSnapshot>& source,const RetouchStroke& stroke,TileStore& store,std::stop_token stop){
 checkpoint(stop);if(stroke.opacity==0)return source;const auto inverse=stroke.localToDocument.inverse();
 const double spacing=std::max(.25,stroke.diameter*.025);std::size_t capacity=1;
 for(std::size_t i=1;i<stroke.points.size();++i){const auto steps=std::ceil(std::hypot(stroke.points[i].x-stroke.points[i-1].x,stroke.points[i].y-stroke.points[i-1].y)/spacing);if(steps>double(maximumDabs-capacity))throw std::length_error("Retouch dab limit exceeded");capacity+=static_cast<std::size_t>(steps);}
 const auto memory=store.memoryAdmission();std::optional<MemoryAdmission::Reservation> dabCharge;if(memory)dabCharge=memory->require(capacity*sizeof(Coordinate));std::vector<Coordinate> dabs;dabs.reserve(capacity);if(dabCharge)dabCharge->commit();dabs.push_back(stroke.points.front());double untilNext=spacing;
 for(std::size_t i=1;i<stroke.points.size();++i){checkpoint(stop);const auto a=stroke.points[i-1],b=stroke.points[i];const auto length=std::hypot(b.x-a.x,b.y-a.y);if(length==0)continue;double offset=untilNext;while(offset<=length){checkpoint(stop);if(dabs.size()>=maximumDabs)throw std::length_error("Retouch dab limit exceeded");const double t=offset/length;dabs.push_back({a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t});offset+=spacing;}untilNext=offset-length;}
 const double radius=stroke.diameter/2,rx=radius*std::hypot(inverse.a,inverse.c),ry=radius*std::hypot(inverse.b,inverse.d);auto first=inverse.map(dabs.front());double left=first.x,right=left,top=first.y,bottom=top;
 for(auto dab:dabs){auto local=inverse.map(dab);left=std::min(left,local.x);right=std::max(right,local.x);top=std::min(top,local.y);bottom=std::max(bottom,local.y);}
 const auto& extent=source->extent;left=std::max(left-rx-1,double(extent.x));right=std::min(right+rx+1,double(extent.x+extent.width));top=std::max(top-ry-1,double(extent.y));bottom=std::min(bottom+ry+1,double(extent.y+extent.height));if(left>=right||top>=bottom)return source;
 const auto columns=static_cast<std::uint64_t>(floorTile(static_cast<std::int64_t>(std::ceil(right))-1)-floorTile(static_cast<std::int64_t>(std::floor(left)))+1);
 const auto rows=static_cast<std::uint64_t>(floorTile(static_cast<std::int64_t>(std::ceil(bottom))-1)-floorTile(static_cast<std::int64_t>(std::floor(top)))+1);
 auto graphCharge=metadataCharge(store,sizeof(RasterSnapshot)+(source->tiles.size()+columns*rows)*mapNodeBytes);
 TileMap tiles=source->tiles;Reader reader(*source,stop);bool changed=false;
 const Coordinate delta{stroke.sourceAnchor.x-stroke.points.front().x,stroke.sourceAnchor.y-stroke.points.front().y};
 for(auto ty=floorTile(static_cast<std::int64_t>(std::floor(top)));ty<=floorTile(static_cast<std::int64_t>(std::ceil(bottom))-1);++ty)for(auto tx=floorTile(static_cast<std::int64_t>(std::floor(left)));tx<=floorTile(static_cast<std::int64_t>(std::ceil(right))-1);++tx){
  checkpoint(stop);const TileCoord coordinate{tx,ty};const auto region=tileExtent(extent,coordinate);const auto count=static_cast<std::size_t>(region.width)*static_cast<std::size_t>(region.height);std::optional<MemoryAdmission::Reservation> scratch;if(memory)scratch=memory->require(count*(sizeof(Pixel)+sizeof(float)));std::vector<Pixel> output(count);std::vector<float> coverage(count,0);if(scratch)scratch->commit();
  for(auto dab:dabs){checkpoint(stop);const auto local=inverse.map(dab);const auto x0=static_cast<int>(std::clamp(std::floor(local.x-rx-1-double(region.x)),0.,double(region.width))),x1=static_cast<int>(std::clamp(std::ceil(local.x+rx+1-double(region.x)),0.,double(region.width)));const auto y0=static_cast<int>(std::clamp(std::floor(local.y-ry-1-double(region.y)),0.,double(region.height))),y1=static_cast<int>(std::clamp(std::ceil(local.y+ry+1-double(region.y)),0.,double(region.height)));
   for(int y=y0;y<y1;++y){checkpoint(stop);for(int x=x0;x<x1;++x){double amount=0;for(int sy=0;sy<2;++sy)for(int sx=0;sx<2;++sx){const auto p=stroke.localToDocument.map({double(region.x+x)+.25+.5*sx,double(region.y+y)+.25+.5*sy});amount+=tip(std::hypot(p.x-dab.x,p.y-dab.y),radius,stroke.hardness)/4;}auto& value=coverage[static_cast<std::size_t>(y)*static_cast<std::size_t>(region.width)+static_cast<std::size_t>(x)];value=std::max(value,static_cast<float>(amount));}}
  }
  bool tileChanged=false;for(int y=0;y<region.height;++y){checkpoint(stop);for(int x=0;x<region.width;++x){const auto index=static_cast<std::size_t>(y)*static_cast<std::size_t>(region.width)+static_cast<std::size_t>(x);const Coordinate local{double(region.x+x)+.5,double(region.y+y)+.5};const auto base=reader.sample(local);auto value=base;const float amount=coverage[index]*static_cast<float>(stroke.opacity)*selectionCoverage(stroke.selection,stroke.localToDocument,region.x+x,region.y+y,stroke.canvasWidth,stroke.canvasHeight);
   if(amount>0){const auto destination=stroke.localToDocument.map(local);const Coordinate sampleDocument{destination.x+delta.x,destination.y+delta.y};const auto sample=reader.sample(inverse.map(sampleDocument));if(sample.a>0){Pixel replacement=sample;if(stroke.kind==RetouchKind::Heal&&base.a>0){const auto sourceMean=lowFrequency(reader,inverse,sampleDocument,stroke.healingRadius),destinationMean=lowFrequency(reader,inverse,destination,stroke.healingRadius);replacement={static_cast<float>((sample.r/sample.a-sourceMean[0]+destinationMean[0])*base.a),static_cast<float>((sample.g/sample.a-sourceMean[1]+destinationMean[1])*base.a),static_cast<float>((sample.b/sample.a-sourceMean[2]+destinationMean[2])*base.a),base.a};}else if(stroke.kind==RetouchKind::Heal){replacement=base;}value={base.r+(replacement.r-base.r)*amount,base.g+(replacement.g-base.g)*amount,base.b+(replacement.b-base.b)*amount,base.a+(replacement.a-base.a)*amount};}}
   output[index]=value;tileChanged=tileChanged||pack(value)!=pack(base);
  }}if(tileChanged){auto tile=store.create(static_cast<int>(region.width),static_cast<int>(region.height),output);if(tile->uniform()&&tile->pixel(0,0)==source->defaultValue)tiles.erase(coordinate);else tiles.insert_or_assign(coordinate,std::move(tile));changed=true;}
 }
 checkpoint(stop);if(!changed)return source;if(source->revision==std::numeric_limits<std::int64_t>::max())throw std::overflow_error("Retouch revision exhausted");return rasterWithCharge(*source,std::move(tiles),source->revision+1,std::move(graphCharge));
}
}
bool RetouchStroke::operator==(const RetouchStroke& other) const{return kind==other.kind&&samePoint(sourceAnchor,other.sourceAnchor)&&diameter==other.diameter&&hardness==other.hardness&&opacity==other.opacity&&healingRadius==other.healingRadius&&localToDocument==other.localToDocument&&selection==other.selection&&canvasWidth==other.canvasWidth&&canvasHeight==other.canvasHeight&&points.size()==other.points.size()&&std::equal(points.begin(),points.end(),other.points.begin(),samePoint);}
void validateRetouchStack(const RetouchStack& stack,const RasterSnapshot& rendered){
 if(stack.policy!=1||!stack.source)throw std::invalid_argument("Invalid retained retouch source or policy");
 const auto& a=stack.source->sourceProfile;const auto& b=rendered.sourceProfile;
 if(stack.source->extent!=rendered.extent||!(a==b||(a&&b&&a->id==b->id&&a->bytes==b->bytes)))throw std::invalid_argument("Retouch source and cache metadata differ");
 if(stack.strokes.empty()||stack.strokes.size()>16)throw std::length_error("Retouch stack requires 1 to 16 strokes");
 std::size_t points=0;for(const auto& stroke:stack.strokes){validateStroke(stroke);points+=stroke.points.size();if(points>32768)throw std::length_error("Retouch point limit exceeded");}
}
std::shared_ptr<const RasterSnapshot> evaluateRetouchStack(const RetouchStack& stack,TileStore& store,std::stop_token stop){
 checkpoint(stop);if(!stack.source||stack.policy!=1)throw std::invalid_argument("Invalid retained retouch source or policy");if(stack.strokes.empty())return stack.source;validateRetouchStack(stack,*stack.source);auto result=stack.source;for(const auto& stroke:stack.strokes)result=applyStroke(result,stroke,store,stop);checkpoint(stop);if(result!=stack.source)return result;
 auto graphCharge=metadataCharge(store,sizeof(RasterSnapshot)+result->tiles.size()*mapNodeBytes);
 return rasterWithCharge(*result,result->tiles,result->revision,std::move(graphCharge));
}
EditTransaction setRetouchStrokes(std::shared_ptr<const DocumentSnapshot> input,const Id& target,TileStore& store,std::vector<RetouchStroke> strokes,std::stop_token stop){
 if(!input)throw std::invalid_argument("Retouch requires a document");
 checkpoint(stop);const auto& original=input->layer(target);if(original.folder||!original.raster)throw std::invalid_argument("Select a raster layer for retouch");EditTransaction edit(input);if(original.retouch&&original.retouch->strokes==strokes)return edit;if(!original.retouch&&strokes.empty())return edit;
 const auto source=original.retouch?original.retouch->source:original.adjustments?original.adjustments->source:original.raster;auto result=source;std::shared_ptr<const RetouchStack> retained;
 if(!strokes.empty()){
  if(strokes.size()>16)throw std::length_error("Retouch stack exceeds 16 strokes");
  std::size_t points=0;std::uint64_t bytes=sizeof(RetouchStack)+strokes.capacity()*sizeof(RetouchStroke);
  for(const auto& stroke:strokes){validateStroke(stroke);points+=stroke.points.size();if(points>32768)throw std::length_error("Retouch point limit exceeded");if(stroke.points.capacity()>(std::numeric_limits<std::uint64_t>::max()-bytes)/sizeof(Coordinate))throw std::length_error("Retouch metadata overflow");bytes+=stroke.points.capacity()*sizeof(Coordinate);}
  auto charge=metadataCharge(store,bytes);
  auto stack=admittedShared<RetouchStack>(std::move(charge),RetouchStack{source,std::move(strokes),1});
  result=evaluateRetouchStack(*stack,store,stop);retained=std::move(stack);
 }
 auto nodes=input->layers();for(auto& node:nodes)if(node.id==target){node.retouch=std::move(retained);if(original.adjustments){std::uint64_t bytes=sizeof(AdjustmentStack)+original.adjustments->operations.capacity()*sizeof(AdjustmentParameters);for(const auto& operation:original.adjustments->operations){bytes+=operation.curve.capacity()*sizeof(CurvePoint);for(const auto& curve:operation.channelCurves)bytes+=curve.capacity()*sizeof(CurvePoint);}
 auto charge=metadataCharge(store,bytes);auto adjustments=admittedShared<AdjustmentStack>(std::move(charge),*original.adjustments);adjustments->source=result;node.raster=evaluateAdjustmentStack(*adjustments,store,stop);node.adjustments=std::move(adjustments);}else node.raster=result;break;}checkpoint(stop);edit.setLayers(std::move(nodes));return edit;
}
}
