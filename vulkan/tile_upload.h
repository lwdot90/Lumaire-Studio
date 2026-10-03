#pragma once
#include "rendering/tile_residency.h"
#include <span>
#include <stop_token>
#include <vulkan/vulkan.h>

namespace compositor {
// Packs one immutable tile into a full canonical slot with transparent padding.
// The admitted read lease is held only while writing staging. No additional
// canonical payload allocation or format conversion: half bits, including extended/subnormal
// values, are preserved in explicit little-endian order. Destination is exactly
// slotBytes bytes and must not overlap the tile's storage.
void encodeTileUpload(const engine::Tile& tile,std::span<std::uint8_t> destination,std::stop_token stop={});
struct TileUploadBuffer { VkBuffer handle; VkDeviceSize bytes; };
// Records one slot transfer, never submits/waits. Source is a prepared slot at
// stagingOffset; destination is the pool slot. Caller guarantees distinct
// nonaliasing allocations, transfer usage flags, a recording command outside a
// render pass, and retired destination/staging reuse. Flush host writes before
// submission; provide transfer-to-consumer barriers and retain both allocations
// until completion. All calls operate on the same ordered queue as residency.
void recordTileUpload(VkCommandBuffer command,TileUploadBuffer staging,
                      VkDeviceSize stagingOffset,TileUploadBuffer pool,std::size_t slot);
}
