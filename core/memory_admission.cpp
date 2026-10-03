#include "core/memory_admission.h"
#include <charconv>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace compositor {
namespace {
std::optional<std::uint64_t> kilobytes(std::string_view data,std::string_view field) {
    std::istringstream input{std::string(data)};std::string line;
    std::optional<std::uint64_t> result;
    while(std::getline(input,line)) {
        std::istringstream row(line);std::string key,value,unit,extra;
        if(!(row>>key) || key!=field) continue;
        if(result || !(row>>value>>unit) || unit!="kB" || (row>>extra)) return std::nullopt;
        std::uint64_t number=0;
        const auto parsed=std::from_chars(value.data(),value.data()+value.size(),number);
        if(parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() || number>std::numeric_limits<std::uint64_t>::max()/1024)
            return std::nullopt;
        result=number*1024;
    }
    return result;
}
std::uint64_t add(std::uint64_t a,std::uint64_t b,bool& overflow) {
    if(b>std::numeric_limits<std::uint64_t>::max()-a) {overflow=true;return std::numeric_limits<std::uint64_t>::max();}
    return a+b;
}
std::string readFile(const char* path) {
    std::ifstream file(path);std::ostringstream text;
    if(file) text<<file.rdbuf();
    return text.str();
}
}
MemorySample MemorySample::parse(std::string_view meminfo,std::string_view status) {
    return {kilobytes(meminfo,"MemAvailable:"),kilobytes(status,"VmRSS:")};
}
MemorySample MemorySample::read() {
    return parse(readFile("/proc/meminfo"),readFile("/proc/self/status"));
}
struct MemoryAdmission::State {
    State(std::uint64_t limit,std::uint64_t headroom,Probe callback)
        :applicationLimit(limit),systemHeadroom(headroom),probe(std::move(callback)) {}
    const std::uint64_t applicationLimit,systemHeadroom;
    Probe probe;
    std::function<void(std::uint64_t)> reclaimer;
    Snapshot bytes{};
    mutable std::mutex mutex;
};
MemoryAdmission::MemoryAdmission(std::uint64_t applicationLimit,std::uint64_t systemHeadroom,Probe probe)
    :state_(std::make_shared<State>(applicationLimit,systemHeadroom,std::move(probe))) {
    if(applicationLimit==0 || !state_->probe) throw std::invalid_argument("Invalid memory admission policy");
}
std::optional<MemoryAdmission::Reservation> MemoryAdmission::reserve(std::uint64_t bytes,Kind kind,Failure* failure) {
    if(kind!=Kind::Cpu && kind!=Kind::Gpu) throw std::invalid_argument("Invalid memory category");
    const auto state=state_;
    // Serialize the sample with ledger changes: two callers cannot both spend
    // the same available headroom while their allocations are still pending.
    std::lock_guard lock(state->mutex);
    const auto sample=state->probe();
    const auto fail=[&](Failure reason)->std::optional<Reservation> {if(failure) *failure=reason;return std::nullopt;};
    if(!sample.available || !sample.resident) return fail(Failure::MissingProbe);
    const auto& ledger=state->bytes;
    bool overflow=false;
    const auto pending=add(ledger.pendingCpu,ledger.pendingGpu,overflow);
    const auto backing=add(ledger.committedCpu,ledger.committedGpu,overflow);
    const auto future=add(backing,pending,overflow);
    const auto charge=add(*sample.resident,future,overflow);
    if(overflow || charge>state->applicationLimit || bytes>state->applicationLimit-charge) return fail(Failure::ApplicationLimit);
    // RSS does not identify which tracked allocations are resident. Reserve for
    // all live backing becoming resident; unrelated RSS must not hide swapped
    // tiles or images. This intentionally counts resident tracked bytes twice.
    if(overflow || *sample.available<state->systemHeadroom || future>*sample.available-state->systemHeadroom ||
       bytes>*sample.available-state->systemHeadroom-future) return fail(Failure::SystemHeadroom);
    auto& counter=kind==Kind::Cpu ? state->bytes.pendingCpu : state->bytes.pendingGpu;
    counter+=bytes;
    if(failure) *failure=Failure::None;
    return Reservation(state,kind,bytes);
}
void MemoryAdmission::Reservation::commit() noexcept {
    if(!state_ || committed_) return;
    std::lock_guard lock(state_->mutex);
    auto& pending=kind_==Kind::Cpu ? state_->bytes.pendingCpu : state_->bytes.pendingGpu;
    auto& committed=kind_==Kind::Cpu ? state_->bytes.committedCpu : state_->bytes.committedGpu;
    pending-=bytes_;committed+=bytes_;committed_=true;
}
MemoryAdmission::Reservation MemoryAdmission::require(std::uint64_t bytes,Kind kind) {
    Failure failure=Failure::None;
    auto reservation=reserve(bytes,kind,&failure);
    if(!reservation && failure!=Failure::MissingProbe) {
        std::function<void(std::uint64_t)> reclaim;
        {std::lock_guard lock(state_->mutex);reclaim=state_->reclaimer;}
        if(reclaim) {
            // Avoid recursive reclamation if an IO seam accidentally admits.
            static thread_local bool reclaiming=false;
            if(!reclaiming) {
                struct Reset {bool& value;~Reset(){value=false;}} reset{reclaiming};
                reclaiming=true;reclaim(bytes);
                reservation=reserve(bytes,kind,&failure);
            }
        }
    }
    if(!reservation) throw std::length_error(message(failure));
    return std::move(*reservation);
}
void MemoryAdmission::Reservation::release() noexcept {
    if(!state_) return;
    {
        std::lock_guard lock(state_->mutex);
        auto& ledger=state_->bytes;
        auto& counter=kind_==Kind::Cpu ? (committed_ ? ledger.committedCpu : ledger.pendingCpu)
                                      : (committed_ ? ledger.committedGpu : ledger.pendingGpu);
        counter-=bytes_;
    }
    state_.reset();
}
MemoryAdmission::Reservation::~Reservation() {release();}
MemoryAdmission::Reservation::Reservation(Reservation&& other) noexcept
    :state_(std::move(other.state_)),kind_(other.kind_),bytes_(other.bytes_),committed_(other.committed_) {}
MemoryAdmission::Reservation& MemoryAdmission::Reservation::operator=(Reservation&& other) noexcept {
    if(this!=&other) {release();state_=std::move(other.state_);kind_=other.kind_;bytes_=other.bytes_;committed_=other.committed_;}
    return *this;
}
void MemoryAdmission::setReclaimer(std::function<void(std::uint64_t)> reclaim) {
    std::lock_guard lock(state_->mutex);state_->reclaimer=std::move(reclaim);
}
MemoryAdmission::Snapshot MemoryAdmission::snapshot() const {std::lock_guard lock(state_->mutex);return state_->bytes;}
std::uint64_t MemoryAdmission::applicationLimit() const {return state_->applicationLimit;}
const char* MemoryAdmission::message(Failure failure) {
    switch(failure) {
        case Failure::None:return "Memory admitted";
        case Failure::ApplicationLimit:return "Application memory envelope exhausted";
        case Failure::SystemHeadroom:return "Not enough available system memory; preserved desktop headroom";
        case Failure::MissingProbe:return "Cannot verify available system memory";
    }
    return "Invalid memory admission result";
}
}
