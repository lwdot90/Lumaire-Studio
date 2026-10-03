#include "vulkan/tile_upload.h"
#include "io/spill_store.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace compositor {
void encodeTileUpload(const engine::Tile& tile,std::span<std::uint8_t> destination,std::stop_token stop) {
    if(destination.size()!=TileResidency::slotBytes)
        throw std::invalid_argument("Upload requires one complete tile slot");
    if(stop.stop_requested()) throw io::SpillError(io::SpillErrorCode::Cancelled,"Tile upload cancelled");
    const auto read=tile.read(stop);
    std::fill(destination.begin(),destination.end(),0);
    for(int y=0;y<read.height();++y) {
        if(stop.stop_requested()) throw io::SpillError(io::SpillErrorCode::Cancelled,"Tile upload cancelled");
        for(int x=0;x<read.width();++x) {
            const auto pixel=read.pixel(x,y);
            const auto offset=(static_cast<std::size_t>(y)*256+static_cast<std::size_t>(x))*8;
            for(std::size_t channel=0;channel<4;++channel) {
                destination[offset+channel*2]=static_cast<std::uint8_t>(pixel[channel]&255);
                destination[offset+channel*2+1]=static_cast<std::uint8_t>(pixel[channel]>>8);
            }
        }
    }
    if(stop.stop_requested()) throw io::SpillError(io::SpillErrorCode::Cancelled,"Tile upload cancelled");
}
void recordTileUpload(VkCommandBuffer command,TileUploadBuffer staging,
                      VkDeviceSize stagingOffset,TileUploadBuffer pool,std::size_t slot) {
    constexpr VkDeviceSize bytes=TileResidency::slotBytes;
    if(!command || !staging.handle || !pool.handle || staging.handle==pool.handle)
        throw std::invalid_argument("Invalid tile transfer handles");
    if(slot>std::numeric_limits<VkDeviceSize>::max()/bytes)
        throw std::invalid_argument("Tile slot offset overflow");
    const auto offset=static_cast<VkDeviceSize>(slot)*bytes;
    if(stagingOffset%4 || stagingOffset>staging.bytes || bytes>staging.bytes-stagingOffset ||
       offset>pool.bytes || bytes>pool.bytes-offset)
        throw std::invalid_argument("Tile transfer outside buffer bounds");
    const VkBufferCopy copy{stagingOffset,offset,bytes};
    vkCmdCopyBuffer(command,staging.handle,pool.handle,1,&copy);
}
}
