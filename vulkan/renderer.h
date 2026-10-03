#pragma once
// Include prototypes before any Qt Vulkan header defines VK_NO_PROTOTYPES.
#include <vulkan/vulkan.h>
#include "core/viewport.h"
#include "core/document.h"
#include "core/resources.h"
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <stop_token>
#include <thread>

namespace compositor {
struct FrameRequest {
    VkSurfaceKHR surface=VK_NULL_HANDLE;
    int width=0,height=0;
    Viewport view;
    Point cursor;
    bool showCursor=false;
    std::int64_t requestedNs=0;
    engine::DocumentPtr document;
    // Assigned by the renderer mailbox; callers do not manage this token.
    std::stop_token stop;
};
struct FrameTiming { std::int64_t requestedNs=0,submittedNs=0,completedNs=0; double gpuNs=-1; };
class VulkanRenderer {
public:
    using Diagnostic=std::function<void(std::string,bool)>;
    VulkanRenderer(VkInstance instance,Diagnostic diagnostic,std::function<void(FrameTiming)> complete,
                   std::function<void(bool)> presentHook,std::string deviceName={},
                   ResourceLimits limits=ResourceLimits::detect(),std::shared_ptr<MemoryAdmission> memory={});
    ~VulkanRenderer();
    VulkanRenderer(const VulkanRenderer&)=delete;
    VulkanRenderer& operator=(const VulkanRenderer&)=delete;
    void request(FrameRequest request);
    // Lifecycle barrier only: releases every resource referencing the old surface.
    void detach();
private:
    struct State;
    std::unique_ptr<State> state_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<FrameRequest> pending_;
    std::stop_source cancellation_{std::nostopstate};
    bool quit_=false,detach_=false,failed_=false;
    std::uint64_t detachGeneration_=0,completedDetachGeneration_=0;
    std::thread thread_;
    void run();
};
}
