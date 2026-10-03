#pragma once
#include "core/document.h"
#include "rendering/mip_cache.h"
#include "core/resources.h"
#include <QImage>
#include <QString>
#include <atomic>
#include <condition_variable>
#include <list>
#include <mutex>
#include <thread>

namespace compositor {
// CanvasThumbnail.swift: canvas-shaped, source-only thumbnail, independent of
// layer visibility/opacity/blend. Pixels remain positioned by the layer affine.
QImage renderThumbnail(const engine::LayerNode& layer,int canvasWidth,int canvasHeight,
                       MipCache& cache,const std::function<bool()>& cancelled={},
                       std::shared_ptr<MemoryAdmission> memory={},std::stop_token stop={});
class ThumbnailWorker {
public:
    struct Result {engine::Id id;QImage image;QString error;};
    using Completion=std::function<void(std::uint64_t,engine::DocumentPtr,std::vector<Result>)>;
    explicit ThumbnailWorker(Completion completion,std::shared_ptr<RuntimeResources> resources=defaultRuntimeResources());
    ~ThumbnailWorker();
    // One replaceable visible-row batch, bounded independently of layer count.
    void request(std::uint64_t generation,engine::DocumentPtr document,std::vector<engine::Id> layers);
    // Closing the last document needs cancellation/cache release without a
    // replacement snapshot. Clear derived storage on its owning worker.
    void suspend();
private:
    struct Request {std::uint64_t generation;engine::DocumentPtr document;std::vector<engine::Id> layers;std::stop_source cancellation;};
    struct Cached {
        std::weak_ptr<const engine::RasterSnapshot> source;
        engine::Affine placement;engine::Sampling sampling;
        int width,height;QImage image;
    };
    void run(std::stop_token stop);
    Completion completion_;
    std::shared_ptr<RuntimeResources> resources_;
    std::mutex mutex_;std::condition_variable_any changed_;
    std::optional<Request> pending_;
    std::stop_source activeStop_;
    bool clearCache_=false;
    std::atomic<std::uint64_t> latest_{0};
    MipCache mipCache_;
    std::list<Cached> cache_; // At most 128 x 72x72x4 bytes; weak source identity.
    std::jthread thread_;
};
}
