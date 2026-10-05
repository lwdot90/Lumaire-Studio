#include "core/image_resize.h"
#include "core/retouch.h"
#include "core/resampling.h"
#include <algorithm>
#include <array>
#include <list>
#include <limits>
#include <map>
#include <stdexcept>

namespace compositor::engine {
namespace {
using Charge=std::shared_ptr<MemoryAdmission::Reservation>;
Charge metadataCharge(TileStore& store,std::uint64_t bytes) {
 if(!store.memoryAdmission())return {};
 if(bytes>std::numeric_limits<std::uint64_t>::max()-128)throw std::length_error("Image resize metadata overflow");
 return std::make_shared<MemoryAdmission::Reservation>(store.memoryAdmission()->require(bytes+128));
}
template<class T,class... Args>std::shared_ptr<T> admittedShared(Charge charge,Args&&... args) {
 auto* value=new T(std::forward<Args>(args)...);if(charge)charge->commit();
 return std::shared_ptr<T>(value,[charge=std::move(charge)](T* owned){delete owned;});
}
constexpr std::uint64_t mapNodeBytes=sizeof(TileMap::value_type)+4*sizeof(void*);
std::uint64_t retouchBytes(const RetouchStack& stack) {
 std::uint64_t bytes=sizeof(RetouchStack)+stack.strokes.size()*sizeof(RetouchStroke);
 for(const auto& stroke:stack.strokes)bytes+=stroke.points.size()*sizeof(Coordinate);
 return bytes;
}
std::uint64_t adjustmentBytes(const AdjustmentStack& stack) {
 std::uint64_t bytes=sizeof(AdjustmentStack)+stack.operations.size()*sizeof(AdjustmentParameters);
 for(const auto& operation:stack.operations){bytes+=operation.curve.size()*sizeof(CurvePoint);for(const auto& channel:operation.channelCurves)bytes+=channel.size()*sizeof(CurvePoint);}return bytes;
}
void checkpoint(std::stop_token stop) {if(stop.stop_requested())throw std::runtime_error("Image resize canceled");}
void add(Pixel& into,Pixel value,float weight) {into.r+=value.r*weight;into.g+=value.g*weight;into.b+=value.b*weight;into.a+=value.a*weight;}
std::int64_t dimension(std::int64_t value,double scale) {
 const double result=std::max(1.,std::round(static_cast<double>(value)*scale));
 if(!std::isfinite(result) || result>30000)throw std::invalid_argument("Resampled source dimensions exceed limits");
 return static_cast<std::int64_t>(result);
}
Affine remap(const Affine& old,const Extent& extent,const Extent& resized,double sx,double sy) {
 const double rx=static_cast<double>(extent.width)/static_cast<double>(resized.width),ry=static_cast<double>(extent.height)/static_cast<double>(resized.height);
 Affine next{sx*old.a*rx,sy*old.b*rx,sx*old.c*ry,sy*old.d*ry,sx*(old.tx+old.a*static_cast<double>(extent.x)+old.c*static_cast<double>(extent.y)),sy*(old.ty+old.b*static_cast<double>(extent.x)+old.d*static_cast<double>(extent.y))};next.inverse();return next;
}
// Four admitted immutable leases at most; no full source image is materialized.
class Reader {
 struct Entry {TilePtr tile;Tile::ReadLease lease;Extent extent;bool restore=false;};
 const RasterSnapshot& raster_;std::stop_token stop_;std::array<Entry,4> entries_;std::size_t next_=0;
 void retire(Entry& entry) {entry.lease={};if(entry.restore && entry.tile)entry.tile->spill(stop_);entry.tile.reset();entry.restore=false;}
public:
 Reader(const RasterSnapshot& raster,std::stop_token stop):raster_(raster),stop_(stop){}
 ~Reader(){for(auto& entry:entries_){entry.lease={};if(entry.restore && entry.tile)try{entry.tile->spill();}catch(...){}}}
 void clear(){for(auto& entry:entries_)retire(entry);}
 Pixel operator()(std::int64_t x,std::int64_t y) {
  x=std::clamp(x,std::int64_t{0},raster_.extent.width-1)+raster_.extent.x;y=std::clamp(y,std::int64_t{0},raster_.extent.height-1)+raster_.extent.y;
  for(auto& entry:entries_)if(entry.tile && entry.extent.contains(x,y))return entry.lease.linearPixel(static_cast<int>(x-entry.extent.x),static_cast<int>(y-entry.extent.y));
  const TileCoord coordinate{floorTile(x),floorTile(y)};const auto found=raster_.tiles.find(coordinate);if(found==raster_.tiles.end())return unpack(raster_.defaultValue);
  checkpoint(stop_);auto& entry=entries_[next_++%entries_.size()];retire(entry);entry.tile=found->second;entry.extent=tileExtent(raster_.extent,coordinate);entry.restore=entry.tile->spillable() && entry.tile->spillStatus().spilled;entry.lease=entry.tile->read(stop_);return entry.lease.linearPixel(static_cast<int>(x-entry.extent.x),static_cast<int>(y-entry.extent.y));
 }
};
FilterAxis axis(double center,double footprint,Sampling mode) {
 if(mode==Sampling::Nearest)return {{static_cast<std::int64_t>(std::floor(center)),1}};
 if(mode==Sampling::Lanczos)return lanczosAxis(center,std::max(1.,footprint));
 const double position=center-.5;const auto left=static_cast<std::int64_t>(std::floor(position));const auto fraction=static_cast<float>(position-static_cast<double>(left));return {{left,1-fraction},{left+1,fraction}};
}
std::shared_ptr<const RasterSnapshot> resizeRaster(const std::shared_ptr<const RasterSnapshot>& source,TileStore& store,Extent output,Sampling mode,bool mask,std::stop_token stop) {
 checkpoint(stop);if(source->revision==std::numeric_limits<std::int64_t>::max())throw std::overflow_error("Resampled source revision exhausted");
 const auto tileCount=static_cast<std::uint64_t>(((output.width+tileSide-1)/tileSide)*((output.height+tileSide-1)/tileSide));
 auto graphCharge=metadataCharge(store,sizeof(RasterSnapshot)+(source->tiles.empty() ? 0 : tileCount*mapNodeBytes));
 if(source->tiles.empty())return admittedShared<RasterSnapshot>(std::move(graphCharge),Id::generate(),output,source->defaultValue,TileMap{},source->revision+1,source->sourceProfile);
 TileMap tiles;const double fx=static_cast<double>(source->extent.width)/static_cast<double>(output.width),fy=static_cast<double>(source->extent.height)/static_cast<double>(output.height);
 Reader read(*source,stop);
 for(auto ty=floorTile(output.y);ty<=floorTile(output.y+output.height-1);++ty)for(auto tx=floorTile(output.x);tx<=floorTile(output.x+output.width-1);++tx){
  checkpoint(stop);const TileCoord coordinate{tx,ty};const auto region=tileExtent(output,coordinate);
  const auto count=static_cast<std::size_t>(region.width*region.height);
  const auto tapCount=[&](double footprint){return mode==Sampling::Lanczos ? static_cast<std::uint64_t>(std::ceil(6*std::max(1.,footprint)))+3 : std::uint64_t{2};};
  const auto workspace=count*sizeof(Pixel)+(region.width*tapCount(fx)+tapCount(fy))*sizeof(FilterTap)+16*region.width*sizeof(Pixel)+region.width*sizeof(FilterAxis)+4096;
  std::optional<MemoryAdmission::Reservation> scratch;if(store.memoryAdmission()){scratch.emplace(store.memoryAdmission()->require(workspace));scratch->commit();}
  std::vector<Pixel> pixels(count);std::vector<FilterAxis> xs;xs.reserve(static_cast<std::size_t>(region.width));for(auto x=region.x;x<region.x+region.width;++x)xs.push_back(axis((static_cast<double>(x)+.5)*fx,fx,mode));
  struct Row {std::int64_t y;std::vector<Pixel> pixels;};std::list<Row> cache;
  for(auto y=region.y;y<region.y+region.height;++y){checkpoint(stop);const auto ys=axis((static_cast<double>(y)+.5)*fy,fy,mode);
   for(const auto& yt:ys){if(yt.weight==0)continue;checkpoint(stop);const auto sourceY=std::clamp(yt.index,std::int64_t{0},source->extent.height-1);
    auto row=std::find_if(cache.begin(),cache.end(),[&](const Row& value){return value.y==sourceY;});
    if(row==cache.end()){
     if(cache.size()==16)cache.pop_back();
     cache.push_front(Row{sourceY,std::vector<Pixel>(static_cast<std::size_t>(region.width))});row=cache.begin();
     for(std::size_t x=0;x<xs.size();++x){checkpoint(stop);for(const auto& xt:xs[x]){if(xt.index%256==0)checkpoint(stop);if(xt.weight!=0)add(row->pixels[x],read(xt.index,sourceY),xt.weight);}}
    }else cache.splice(cache.begin(),cache,row);
    const auto base=static_cast<std::size_t>((y-region.y)*region.width);for(std::size_t x=0;x<xs.size();++x)add(pixels[base+x],cache.front().pixels[x],yt.weight);
   }
  }
  for(auto& pixel:pixels){pixel=finishFiltered(pixel);if(mask)pixel={0,0,0,std::clamp(pixel.a,0.f,1.f)};}
  read.clear();checkpoint(stop);auto tile=store.create(static_cast<int>(region.width),static_cast<int>(region.height),pixels);if(tile->spillable())tile->spill(stop);tiles.emplace(coordinate,std::move(tile));
 }
 checkpoint(stop);return admittedShared<RasterSnapshot>(std::move(graphCharge),Id::generate(),output,PackedPixel{},std::move(tiles),source->revision+1,source->sourceProfile);
}
}
EditTransaction resampleDocument(DocumentPtr document,TileStore& store,int width,int height,Sampling sampling,std::stop_token stop) {
 if(!document)throw std::invalid_argument("Image resize requires a document");
 Extent{0,0,width,height}.validate();checkpoint(stop);
 if(sampling!=Sampling::Nearest && sampling!=Sampling::Bilinear && sampling!=Sampling::Lanczos)throw std::invalid_argument("Unsupported image resize filter");
 EditTransaction edit(document);if(width==document->width && height==document->height)return edit;
 const double sx=static_cast<double>(width)/static_cast<double>(document->width),sy=static_cast<double>(height)/static_cast<double>(document->height);
 auto nodes=document->layers();std::map<std::pair<const RasterSnapshot*,bool>,std::shared_ptr<const RasterSnapshot>> cache;
 std::map<const RasterSnapshot*,std::uint64_t> colorExtents,maskExtents;std::uint64_t colorPixels=0,maskPixels=0;
 for(const auto& node:nodes)if(node.raster){const auto source=node.retouch ? node.retouch->source : node.adjustments ? node.adjustments->source : node.raster;const Extent extent{0,0,dimension(source->extent.width,sx),dimension(source->extent.height,sy)};extent.validate();const auto pixels=static_cast<std::uint64_t>(extent.width*extent.height);if(colorExtents.emplace(source.get(),pixels).second)colorPixels+=pixels;if(node.mask && maskExtents.emplace(node.mask.get(),pixels).second)maskPixels+=pixels;}
 if(colorPixels>100000000 || maskPixels>100000000)throw std::invalid_argument("Aggregate resampled source or mask dimensions exceed 100 MP");
 struct Retouched {std::shared_ptr<const RetouchStack> stack;std::shared_ptr<const RasterSnapshot> raster;};std::map<const RetouchStack*,Retouched> retouched;
 struct Adjusted {std::shared_ptr<const AdjustmentStack> stack;std::shared_ptr<const RasterSnapshot> raster;};std::map<std::pair<const AdjustmentStack*,const RasterSnapshot*>,Adjusted> adjusted;
 for(auto& node:nodes){checkpoint(stop);if(!node.raster){auto& a=node.localToDocument;a={sx*a.a,sy*a.b,sx*a.c,sy*a.d,sx*a.tx,sy*a.ty};continue;}
  const auto source=node.retouch ? node.retouch->source : node.adjustments ? node.adjustments->source : node.raster;const auto oldExtent=source->extent;const Extent nextExtent{0,0,dimension(oldExtent.width,sx),dimension(oldExtent.height,sy)};
  const auto resized=[&](const std::shared_ptr<const RasterSnapshot>& raster,bool mask){const auto key=std::make_pair(raster.get(),mask);if(const auto found=cache.find(key);found!=cache.end())return found->second;auto result=resizeRaster(raster,store,nextExtent,sampling,mask,stop);cache.emplace(key,result);return result;};
  auto output=resized(source,false);
  if(node.retouch){const auto key=node.retouch.get();if(const auto found=retouched.find(key);found!=retouched.end()){node.retouch=found->second.stack;output=found->second.raster;}else{auto stack=admittedShared<RetouchStack>(metadataCharge(store,retouchBytes(*node.retouch)),*node.retouch);stack->source=output;for(auto& stroke:stack->strokes)stroke.localToDocument=remap(stroke.localToDocument,oldExtent,nextExtent,1,1);output=evaluateRetouchStack(*stack,store,stop);node.retouch=std::move(stack);retouched.emplace(key,Retouched{node.retouch,output});}}
  if(node.adjustments){const auto key=std::make_pair(node.adjustments.get(),output.get());if(const auto found=adjusted.find(key);found!=adjusted.end()){node.adjustments=found->second.stack;output=found->second.raster;}else{auto stack=admittedShared<AdjustmentStack>(metadataCharge(store,adjustmentBytes(*node.adjustments)),*node.adjustments);stack->source=output;output=evaluateAdjustmentStack(*stack,store,stop);node.adjustments=std::move(stack);adjusted.emplace(key,Adjusted{node.adjustments,output});}}
  if(node.mask)node.mask=resized(node.mask,true);
  node.raster=std::move(output);node.localToDocument=remap(node.localToDocument,oldExtent,nextExtent,sx,sy);
 }
 std::optional<Selection> selection=document->selection;if(selection){auto& b=selection->bounds;const auto left=static_cast<std::int64_t>(std::floor(static_cast<double>(b.x)*sx)),top=static_cast<std::int64_t>(std::floor(static_cast<double>(b.y)*sy));const auto right=static_cast<std::int64_t>(std::ceil(static_cast<double>(b.x+b.width)*sx)),bottom=static_cast<std::int64_t>(std::ceil(static_cast<double>(b.y+b.height)*sy));b={left,top,std::min<std::int64_t>(width,right)-left,std::min<std::int64_t>(height,bottom)-top};selection->featherRadius=std::min(256.,selection->featherRadius*std::sqrt(sx*sy));selection->validate(width,height);}
 checkpoint(stop);edit.setLayers(std::move(nodes));edit.setCanvas(width,height);edit.setSelection(selection);return edit;
}
}
