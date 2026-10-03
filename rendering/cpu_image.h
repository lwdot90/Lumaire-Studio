#pragma once
#include "core/document.h"
#include "core/viewport.h"
#include <QImage>
#include <functional>
#include <stop_token>

namespace compositor {
class MipCache;
enum class CpuRenderMode { Regions, Reference };
struct CpuRenderStats {
    std::uint64_t blocks=0,layerSamples=0;
    std::size_t viewportLayers=0;
};
std::uint64_t cpuImageAllocationBytes(int width,int height);
std::uint64_t cpuRenderScratchBytes(const engine::DocumentSnapshot& document);
// Scalar reference presentation. Cancellation returns a null image. Allocation
// is bounded to the viewport, never a full-resolution document backing image.
// Production callers reserve image and cpuRenderScratchBytes first and supply
// an admitted mip cache; the local cache is intended for standalone tests.
QImage renderCpuImage(const engine::DocumentSnapshot& document, const Viewport& view,
                      int width, int height, const std::function<bool()>& cancelled={},
                      CpuRenderMode mode=CpuRenderMode::Regions,CpuRenderStats* stats=nullptr,MipCache* mipCache=nullptr,std::stop_token stop={});
}
