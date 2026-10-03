#pragma once
#include "rendering/cpu_image.h"
#include "rendering/admitted_image.h"
#include "rendering/mip_cache.h"
#include "core/resources.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace compositor {
class CpuWorker {
public:
    using Completion=std::function<void(std::uint64_t,QImage,QString)>;
    explicit CpuWorker(Completion completion,std::shared_ptr<RuntimeResources> resources=defaultRuntimeResources())
        :completion_(std::move(completion)),resources_(resources ? std::move(resources) : defaultRuntimeResources()),mipCache_(resources_->limits.cpuMips,resources_->memory),
         thread_([this](std::stop_token stop){run(stop);}) {}
    ~CpuWorker() {
        {std::lock_guard guard(mutex_);activeStop_.request_stop();}
        thread_.request_stop();changed_.notify_all();thread_.join();
    }
    void request(std::uint64_t generation,engine::DocumentPtr document,Viewport view,int width,int height) {
        if(generation==0 || !document) throw std::invalid_argument("Invalid CPU render request");
        std::lock_guard guard(mutex_);activeStop_.request_stop();activeStop_=std::stop_source{};
        latest_=generation;pending_=Request{generation,std::move(document),view,width,height,activeStop_};changed_.notify_all();
    }
    // Cancellation and cache clearing stay on the owning worker. Hidden tabs
    // must not keep obsolete source snapshots alive through derived mips.
    void suspend() {
        std::lock_guard guard(mutex_);activeStop_.request_stop();latest_=0;pending_.reset();clearCache_=true;changed_.notify_all();
    }
private:
    struct Request { std::uint64_t generation; engine::DocumentPtr document; Viewport view; int width,height;std::stop_source cancellation; };
    void run(std::stop_token stop) {
        while(!stop.stop_requested()) {
            std::optional<Request> request;
            bool clear=false;
            {
                std::unique_lock lock(mutex_);
                changed_.wait(lock,stop,[this]{return pending_.has_value() || clearCache_;});
                if(stop.stop_requested()) return;
                request=std::move(pending_); pending_.reset();
                clear=clearCache_;clearCache_=false;
            }
            if(clear) mipCache_.clear();
            if(!request) continue;
            const auto jobStop=request->cancellation.get_token();
            std::stop_callback shutdown(stop,[source=request->cancellation]{source.request_stop();});
            try {
                mipCache_.prune(*request->document);
                const auto cancelled=[&]{return jobStop.stop_requested() || latest_.load()!=request->generation;};
                auto permit=resources_->compute.acquire(WorkScheduler::Priority::Interactive,false,jobStop,cancelled);
                if(!permit) continue;
                auto memory=resources_->memory->require(cpuImageAllocationBytes(request->width,request->height));
                QImage image;
                {
                    auto scratch=resources_->memory->require(cpuRenderScratchBytes(*request->document));
                    image=renderCpuImage(*request->document,request->view,request->width,request->height,
                        cancelled,CpuRenderMode::Regions,nullptr,&mipCache_,jobStop);
                }
                if(image.isNull() || cancelled()) continue;
                image=admittedImage(std::move(image),std::move(memory));
                permit.reset();
                if(!image.isNull() && !cancelled()) completion_(request->generation,std::move(image),{});
            } catch(const std::exception& error) {
                if(!jobStop.stop_requested() && latest_.load()==request->generation) completion_(request->generation,{},QString::fromUtf8(error.what()));
            }
        }
    }
    Completion completion_;
    std::shared_ptr<RuntimeResources> resources_;
    std::mutex mutex_;
    std::condition_variable_any changed_;
    std::optional<Request> pending_;
    std::stop_source activeStop_;
    bool clearCache_=false;
    std::atomic<std::uint64_t> latest_{0};
    MipCache mipCache_;
    std::jthread thread_;
};
}
