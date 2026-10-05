#include "core/soft_mask_edits.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

namespace compositor::engine {
namespace {
void checkpoint(std::stop_token stop) {
    if(stop.stop_requested()) throw std::runtime_error("Mask edit canceled");
}
const LayerNode& maskedLayer(const DocumentPtr& input,const Id& id) {
    if(!input) throw std::invalid_argument("Mask edit requires a document");
    const auto& layer=input->layer(id);
    if(layer.folder || !layer.raster || !layer.mask) throw std::invalid_argument("Select a raster layer with a linked mask");
    if(!layer.maskEnabled) throw std::invalid_argument("Enable mask before editing");
    return layer;
}
std::int64_t clampedOffset(std::int64_t origin,int offset,std::int64_t first,std::int64_t last) {
    if(offset<0 && origin-first<static_cast<std::int64_t>(-offset)) return first;
    if(offset>0 && last-origin<static_cast<std::int64_t>(offset)) return last;
    return origin+offset;
}
std::shared_ptr<MemoryAdmission::Reservation> metadataCharge(TileStore& store,std::size_t tileCount) {
    const auto memory=store.memoryAdmission();if(!memory) return {};
    constexpr std::uint64_t nodeBytes=sizeof(TileMap::value_type)+128;
    if(tileCount>(std::numeric_limits<std::uint64_t>::max()-sizeof(RasterSnapshot)-256)/nodeBytes)
        throw std::length_error("Mask metadata bounds overflow");
    auto result=std::make_shared<MemoryAdmission::Reservation>(memory->require(tileCount*nodeBytes+sizeof(RasterSnapshot)+256));
    result->commit();return result;
}
}
EditTransaction paintLayerMask(DocumentPtr input,const Id& id,TileStore& store,std::span<const Coordinate> points,
                              const BrushSettings& settings,bool reveal,std::stop_token stop) {
    checkpoint(stop);maskedLayer(input,id);
    auto maskSettings=settings;maskSettings.paintMask=true;maskSettings.erasing=false;
    const float gray=reveal ? 1.f : 0.f;maskSettings.color={gray,gray,gray,1};
    return brushStroke(std::move(input),id,store,points,maskSettings,stop);
}
EditTransaction featherLayerMask(DocumentPtr input,const Id& id,TileStore& store,double radius,std::stop_token stop) {
    checkpoint(stop);const auto& layer=maskedLayer(input,id);
    if(!std::isfinite(radius) || radius<0 || radius>32) throw std::invalid_argument("Mask feather radius must be 0 to 32 source pixels");
    EditTransaction edit(input);if(radius==0) return edit;
    const auto source=layer.mask;const auto extent=source->extent;
    if(source->tiles.empty()) return edit; // Clamped uniform defaults are exact fixed points.
    const int support=static_cast<int>(std::ceil(radius));const double sigma=radius/3;
    const auto firstX=floorTile(extent.x),lastX=floorTile(extent.x+extent.width-1);
    const auto firstY=floorTile(extent.y),lastY=floorTile(extent.y+extent.height-1);
    const auto tileCount=static_cast<std::size_t>((lastX-firstX+1)*(lastY-firstY+1));
    auto metadata=metadataCharge(store,tileCount);
    TileMap tiles=source->tiles;
    std::optional<MemoryAdmission::Reservation> kernelCharge;
    if(store.memoryAdmission()) kernelCharge=store.memoryAdmission()->require(static_cast<std::uint64_t>(2*support+1)*sizeof(double));
    std::vector<double> kernel(static_cast<std::size_t>(2*support+1));if(kernelCharge) kernelCharge->commit();
    double total=0;
    for(int i=-support;i<=support;++i) {const double d=static_cast<double>(i)/sigma;const double weight=std::exp(-.5*d*d);kernel[static_cast<std::size_t>(i+support)]=weight;total+=weight;}
    for(auto& weight:kernel) weight/=total;
    bool changed=false;
    for(auto ty=firstY;ty<=lastY;++ty) for(auto tx=firstX;tx<=lastX;++tx) {
        checkpoint(stop);const TileCoord coordinate{tx,ty};const auto region=tileExtent(extent,coordinate);
        const auto width=static_cast<int>(region.width),height=static_cast<int>(region.height);
        const auto haloWidth=width+2*support,haloHeight=height+2*support;
        const auto haloCount=static_cast<std::size_t>(haloWidth)*static_cast<std::size_t>(haloHeight);
        const auto horizontalCount=static_cast<std::size_t>(width)*static_cast<std::size_t>(haloHeight);
        const auto outputCount=static_cast<std::size_t>(width)*static_cast<std::size_t>(height);
        std::optional<MemoryAdmission::Reservation> scratch;
        if(store.memoryAdmission()) scratch=store.memoryAdmission()->require((haloCount+horizontalCount)*sizeof(float)+outputCount*sizeof(Pixel));
        std::vector<float> halo(haloCount),horizontal(horizontalCount);std::vector<Pixel> output(outputCount);if(scratch) scratch->commit();
        // One read lease at a time. Halo rows gather only alpha coverage; no
        // full mask/raster allocation or persistent multi-tile lease cache.
        for(int y=0;y<haloHeight;++y) {
            checkpoint(stop);const auto sy=clampedOffset(region.y,y-support,extent.y,extent.y+extent.height-1);
            std::optional<TileCoord> active;Tile::ReadLease lease;Extent activeBounds;
            for(int x=0;x<haloWidth;++x) {
                const auto sx=clampedOffset(region.x,x-support,extent.x,extent.x+extent.width-1);
                const TileCoord wanted{floorTile(sx),floorTile(sy)};
                if(!active || *active!=wanted) {
                    lease={};active=wanted;activeBounds=tileExtent(extent,wanted);
                    const auto found=source->tiles.find(wanted);if(found!=source->tiles.end()) lease=found->second->read(stop);
                }
                const auto alpha=lease ? lease.linearPixel(static_cast<int>(sx-activeBounds.x),static_cast<int>(sy-activeBounds.y)).a : unpack(source->defaultValue).a;
                if(!std::isfinite(alpha) || alpha<0 || alpha>1) throw std::invalid_argument("Invalid mask coverage");
                halo[static_cast<std::size_t>(y)*static_cast<std::size_t>(haloWidth)+static_cast<std::size_t>(x)]=alpha;
            }
        }
        for(int y=0;y<haloHeight;++y) {
            checkpoint(stop);
            for(int x=0;x<width;++x) {
                double sum=0;for(int k=0;k<2*support+1;++k)
                    sum+=kernel[static_cast<std::size_t>(k)]*halo[static_cast<std::size_t>(y)*static_cast<std::size_t>(haloWidth)+static_cast<std::size_t>(x+k)];
                horizontal[static_cast<std::size_t>(y)*static_cast<std::size_t>(width)+static_cast<std::size_t>(x)]=static_cast<float>(sum);
            }
        }
        bool tileChanged=false;
        for(int y=0;y<height;++y) {
            checkpoint(stop);
            for(int x=0;x<width;++x) {
                double sum=0;for(int k=0;k<2*support+1;++k)
                    sum+=kernel[static_cast<std::size_t>(k)]*horizontal[static_cast<std::size_t>(y+k)*static_cast<std::size_t>(width)+static_cast<std::size_t>(x)];
                const Pixel value{0,0,0,static_cast<float>(std::clamp(sum,0.,1.))};
                output[static_cast<std::size_t>(y)*static_cast<std::size_t>(width)+static_cast<std::size_t>(x)]=value;
                const auto original=halo[static_cast<std::size_t>(y+support)*static_cast<std::size_t>(haloWidth)+static_cast<std::size_t>(x+support)];
                tileChanged|=pack(value)!=pack({0,0,0,original});
            }
        }
        if(!tileChanged) continue;
        checkpoint(stop);auto tile=store.create(width,height,output);
        if(tile->uniform() && tile->pixel(0,0)==source->defaultValue) tiles.erase(coordinate);
        else tiles.insert_or_assign(coordinate,std::move(tile));
        changed=true;
    }
    checkpoint(stop);if(!changed) return edit;
    if(source->revision==std::numeric_limits<std::int64_t>::max()) throw std::overflow_error("Mask revision exhausted");
    auto mask=std::shared_ptr<const RasterSnapshot>(new RasterSnapshot(Id::generate(),extent,source->defaultValue,std::move(tiles),source->revision+1),
        [metadata](const RasterSnapshot* value){delete value;});
    auto layers=input->layers();for(auto& node:layers) if(node.id==id) {node.mask=std::move(mask);break;}
    checkpoint(stop);edit.setLayers(std::move(layers));checkpoint(stop);return edit;
}
}
