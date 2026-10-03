#pragma once
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include "core/memory_admission.h"

namespace compositor {
namespace engine {class TileStore;class SpillCoordinator;}
namespace io {class SpillStore;}
// Initial component ceilings, not total-process memory qualification. Canonical
// tiles include history and all tabs; toolkit/codec/driver allocations remain
// separately measured. No pool is allocated merely by selecting this profile.
struct ResourceLimits {
    bool lowMemory;
    std::size_t canonicalTiles,cpuMips,thumbnailMips,gpuTiles,gpuMips,gpuFrame;
    unsigned computeWorkers;
    std::uint64_t applicationMemory,systemHeadroom;
    static ResourceLimits forMachine(std::uint64_t physicalBytes,unsigned hardwareThreads);
    static ResourceLimits detect();
};

// Shared admission across CPU canvases, thumbnails and image/project jobs.
// Acquisition runs on workers, never the UI thread. Existing jobs are not
// forcibly preempted; queued interactive work precedes eligible lower priorities.
class WorkScheduler {
    struct State;
public:
    enum class Priority { Interactive, Processing, Background };
    struct Snapshot {unsigned active,heavy;std::array<unsigned,3> waiting;};
    class Permit {
    public:
        ~Permit();
        Permit(Permit&& other) noexcept;
        Permit& operator=(Permit&& other) noexcept;
        Permit(const Permit&)=delete;
        Permit& operator=(const Permit&)=delete;
    private:
        friend class WorkScheduler;
        Permit(std::shared_ptr<State> state,bool heavy):state_(std::move(state)),heavy_(heavy) {}
        void release() noexcept;
        std::shared_ptr<State> state_;
        bool heavy_;
    };
    explicit WorkScheduler(unsigned workers,unsigned heavyWorkers=1);
    // Empty on shutdown/supersession. Predicates must not call this scheduler.
    std::optional<Permit> acquire(Priority priority,bool heavy,std::stop_token stop={},
                                  const std::function<bool()>& cancelled={});
    Snapshot snapshot() const;
private:
    std::shared_ptr<State> state_;
};

struct RuntimeResources {
    explicit RuntimeResources(ResourceLimits profile,MemoryAdmission::Probe probe=MemorySample::read,
                              std::shared_ptr<io::SpillStore> spill={});
    const ResourceLimits limits;
    WorkScheduler compute;
    const std::shared_ptr<MemoryAdmission> memory;
    const std::shared_ptr<io::SpillStore> spill;
    const std::shared_ptr<engine::SpillCoordinator> spillCoordinator;
    const std::shared_ptr<engine::TileStore> tiles;
    std::string storageError;
};
// All default consumers in this process share one scheduler, including windows.
std::shared_ptr<RuntimeResources> defaultRuntimeResources();
}
