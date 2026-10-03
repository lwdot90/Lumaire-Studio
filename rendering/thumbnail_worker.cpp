#include "rendering/thumbnail_worker.h"
#include "rendering/admitted_image.h"
#include "rendering/layer_sampler.h"
#include "rendering/display_pixel.h"

namespace compositor {
QImage renderThumbnail(const engine::LayerNode& source,int canvasWidth,int canvasHeight,
                       MipCache& cache,const std::function<bool()>& cancelled,std::shared_ptr<MemoryAdmission> memory,std::stop_token stop) {
    engine::Extent{0,0,canvasWidth,canvasHeight}.validate();
    if(source.folder || !source.raster) throw std::invalid_argument("Thumbnail needs a raster layer");
    if(stop.stop_requested() || (cancelled && cancelled())) return {};
    constexpr int box=36,backing=2;
    const double fit=double(box)/std::max(canvasWidth,canvasHeight);
    const int width=backing*std::max(1,static_cast<int>(std::round(canvasWidth*fit)));
    const int height=backing*std::max(1,static_cast<int>(std::round(canvasHeight*fit)));
    std::optional<MemoryAdmission::Reservation> pixels;
    if(memory) pixels=memory->require(static_cast<std::uint64_t>(width)*static_cast<std::uint64_t>(height)*4);
    const double step=double(canvasWidth)/width;
    auto layer=source;layer.parent.reset();layer.siblingOrder=0;layer.visible=true;
    layer.opacity=1;layer.blend=engine::BlendMode::Normal;
    const engine::LayerStack stack({layer});CpuLayerSampler sampler(stack,{step,step},cache);
    const auto region=sampler.region({.5*step,.5*step,(width-.5)*step,(height-.5)*step});
    QImage image(width,height,QImage::Format_RGB32);
    if(image.isNull()) throw std::bad_alloc();
    const float backgrounds[]{engine::decodeSrgb(.32f),engine::decodeSrgb(.22f)};
    for(int y=0;y<height;++y) {
        if(stop.stop_requested() || (cancelled && cancelled())) return {};
        auto* row=reinterpret_cast<std::uint32_t*>(image.scanLine(y));
        for(int x=0;x<width;++x) {
            const auto pixel=sampler.evaluate(region,(x+.5)*step,(y+.5)*step,cancelled,stop);
            if(!pixel) return {};
            row[x]=displayArgb(*pixel,backgrounds[(x/12+y/12)&1],x,y);
        }
    }
    image.setDevicePixelRatio(backing);
    if(stop.stop_requested() || (cancelled && cancelled())) return {};
    if(pixels) return admittedImage(std::move(image),std::move(*pixels));
    return image;
}
ThumbnailWorker::ThumbnailWorker(Completion completion,std::shared_ptr<RuntimeResources> resources)
    :completion_(std::move(completion)),resources_(resources ? std::move(resources) : defaultRuntimeResources()),mipCache_(resources_->limits.thumbnailMips,resources_->memory),
     thread_([this](std::stop_token stop){run(stop);}) {}
ThumbnailWorker::~ThumbnailWorker() {
    {std::lock_guard lock(mutex_);activeStop_.request_stop();}
    thread_.request_stop();changed_.notify_all();thread_.join();
}
void ThumbnailWorker::request(std::uint64_t generation,engine::DocumentPtr document,std::vector<engine::Id> layers) {
    if(generation==0 || !document || layers.size()>64) throw std::invalid_argument("Invalid thumbnail batch");
    std::lock_guard lock(mutex_);activeStop_.request_stop();activeStop_=std::stop_source{};
    latest_=generation;pending_=Request{generation,std::move(document),std::move(layers),activeStop_};changed_.notify_all();
}
void ThumbnailWorker::suspend() {
    std::lock_guard lock(mutex_);activeStop_.request_stop();latest_=0;pending_.reset();clearCache_=true;changed_.notify_all();
}
void ThumbnailWorker::run(std::stop_token stop) {
    while(!stop.stop_requested()) {
        std::optional<Request> request;
        bool clear=false;
        {
            std::unique_lock lock(mutex_);changed_.wait(lock,stop,[this]{return pending_.has_value() || clearCache_;});
            if(stop.stop_requested()) return;
            request=std::move(pending_);pending_.reset();
            clear=clearCache_;clearCache_=false;
        }
        if(clear) {mipCache_.clear();cache_.clear();}
        if(!request) continue;
        const auto jobStop=request->cancellation.get_token();
        std::stop_callback shutdown(stop,[source=request->cancellation]{source.request_stop();});
        const auto cancelled=[&]{return jobStop.stop_requested() || latest_.load()!=request->generation;};
        if(request->layers.empty()) {mipCache_.clear();cache_.clear();}
        else mipCache_.prune(*request->document);
        std::vector<Result> results;
        for(const auto& id:request->layers) {
            if(cancelled()) break;
            try {
                auto permit=resources_->compute.acquire(WorkScheduler::Priority::Background,false,jobStop,cancelled);
                if(!permit) break;
                auto scratch=resources_->memory->require(2*1024*1024);
                const auto& layer=request->document->layer(id);
                if(layer.folder || !layer.raster) throw std::invalid_argument("Thumbnail needs a raster layer");
                const auto width=request->document->width,height=request->document->height;
                auto found=std::find_if(cache_.begin(),cache_.end(),[&](const Cached& entry) {
                    return entry.source.lock()==layer.raster && entry.placement==layer.localToDocument &&
                        entry.sampling==layer.sampling && entry.width==width && entry.height==height;
                });
                QImage image;
                if(found!=cache_.end()) {image=found->image;cache_.splice(cache_.begin(),cache_,found);}
                else {
                    image=renderThumbnail(layer,width,height,mipCache_,cancelled,resources_->memory,jobStop);
                    if(image.isNull()) break;
                    cache_.push_front({layer.raster,layer.localToDocument,layer.sampling,width,height,image});
                    if(cache_.size()>128) cache_.pop_back();
                }
                results.push_back({id,std::move(image),{}});
            } catch(const std::exception& e) {results.push_back({id,{},QString::fromUtf8(e.what())});}
        }
        if(!cancelled()) completion_(request->generation,request->document,std::move(results));
    }
}
}
