#include "vulkan/renderer.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

// Exercise the actual render worker's mailbox, detach barrier and shutdown
// without Qt, a window, or a GPU. Null-surface requests are intentionally
// discarded before any Vulkan device calls. Real GPU execution is tested by
// lifecycle_test with validation; this test isolates our synchronization for
// TSan from uninstrumented Qt/DBus/raster worker internals.
int main() {
    std::atomic<int> unexpectedCallbacks=0;
    for(int cycle=0;cycle<12;++cycle) {
        compositor::VulkanRenderer renderer(VK_NULL_HANDLE,
            [&](std::string,bool){++unexpectedCallbacks;},
            [&](compositor::FrameTiming){++unexpectedCallbacks;},
            [&](bool){++unexpectedCallbacks;},{});
        std::vector<std::thread> producers;
        for(int producer=0;producer<3;++producer) producers.emplace_back([&,producer] {
            for(int i=0;i<2000;++i) {
                compositor::FrameRequest request;
                request.width=640+producer; request.height=480;
                request.view.center={double(i),double(producer)};
                request.requestedNs=compositor::monotonicNs();
                renderer.request(request);
            }
        });
        for(int i=0;i<50;++i) renderer.detach();
        for(auto& producer:producers) producer.join();
        renderer.detach();
        renderer.request({}); // Shutdown with a possible pending request.
    }
    if(unexpectedCallbacks.load()!=0) {
        std::cerr<<"A surface-less request unexpectedly reached GPU execution\n";
        return EXIT_FAILURE;
    }
    std::cout<<"render mailbox, concurrent submission, detach and shutdown passed\n";
}
