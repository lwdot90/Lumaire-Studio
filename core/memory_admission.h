#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

namespace compositor {
struct MemorySample {
    std::optional<std::uint64_t> available,resident;
    static MemorySample parse(std::string_view meminfo,std::string_view status);
    static MemorySample read();
};

// Reservations cover future allocation peaks. Commit only after materializing
// the allocation; retain the committed charge until its backing is released.
// All tracked CPU/GPU backing is conservatively additional to RSS, including
// resident bytes: the probe cannot prove allocation-level overlap. This does
// not instrument hidden library allocations or guarantee against another
// process allocating after the probe.
class MemoryAdmission {
    struct State;
public:
    enum class Kind { Cpu, Gpu };
    enum class Failure { None, ApplicationLimit, SystemHeadroom, MissingProbe };
    struct Snapshot {
        std::uint64_t pendingCpu,pendingGpu,committedCpu,committedGpu;
    };
    class Reservation {
    public:
        ~Reservation();
        Reservation(Reservation&& other) noexcept;
        Reservation& operator=(Reservation&& other) noexcept;
        Reservation(const Reservation&)=delete;
        Reservation& operator=(const Reservation&)=delete;
        void commit() noexcept;
        std::uint64_t bytes() const {return bytes_;}
    private:
        friend class MemoryAdmission;
        Reservation(std::shared_ptr<State> state,Kind kind,std::uint64_t bytes)
            :state_(std::move(state)),kind_(kind),bytes_(bytes) {}
        void release() noexcept;
        std::shared_ptr<State> state_;
        Kind kind_;
        std::uint64_t bytes_;
        bool committed_=false;
    };
    // Called under the admission mutex. A probe must not call this admission's
    // methods or commit/release its reservations. A reservation has one owner;
    // its move, commit and destruction must not occur concurrently.
    using Probe=std::function<MemorySample()>;
    MemoryAdmission(std::uint64_t applicationLimit,std::uint64_t systemHeadroom,Probe probe=MemorySample::read);
    std::optional<Reservation> reserve(std::uint64_t bytes,Kind kind=Kind::Cpu,Failure* failure=nullptr);
    Reservation require(std::uint64_t bytes,Kind kind=Kind::Cpu);
    // Worker-only retry hook. Invoked outside the ledger mutex after denial.
    // It must not allocate through this admission or wait for busy readers.
    void setReclaimer(std::function<void(std::uint64_t)> reclaim);
    Snapshot snapshot() const;
    std::uint64_t applicationLimit() const;
    static const char* message(Failure failure);
private:
    std::shared_ptr<State> state_;
};
}
