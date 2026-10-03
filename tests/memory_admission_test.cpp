#include "core/memory_admission.h"
#include <array>
#include <atomic>
#include <barrier>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using namespace compositor;
namespace {
using Kind=MemoryAdmission::Kind;
using Failure=MemoryAdmission::Failure;
constexpr auto maximum=std::numeric_limits<std::uint64_t>::max();
void expect(bool value,const char* message) {if(!value) throw std::runtime_error(message);}
template<class F> void rejects(F action) {
    try {action();} catch(const std::exception&) {return;}
    throw std::runtime_error("Invalid memory admission operation accepted");
}
bool same(MemoryAdmission::Snapshot a,MemoryAdmission::Snapshot b) {
    return a.pendingCpu==b.pendingCpu && a.pendingGpu==b.pendingGpu &&
           a.committedCpu==b.committedCpu && a.committedGpu==b.committedGpu;
}
void ledger(const MemoryAdmission& admission,std::uint64_t pendingCpu=0,std::uint64_t pendingGpu=0,
            std::uint64_t committedCpu=0,std::uint64_t committedGpu=0) {
    expect(same(admission.snapshot(),{pendingCpu,pendingGpu,committedCpu,committedGpu}),
           "Memory ownership ledger differs from live reservations");
}
void denied(MemoryAdmission& admission,std::uint64_t bytes,Failure expected,Kind kind=Kind::Cpu) {
    const auto before=admission.snapshot();
    auto failure=Failure::None;
    expect(!admission.reserve(bytes,kind,&failure),"Unsafe allocation was admitted");
    expect(failure==expected,"Allocation denial has the wrong reason");
    expect(same(before,admission.snapshot()),"Denied reservation changed accounting");
}

void parsing() {
    const auto sample=MemorySample::parse("MemTotal: 90 kB\n MemAvailable:\t123 kB\nOther: bad\n",
                                          "Name: compositor\nVmRSS: 7 kB\nVmSize: 999 kB\n");
    expect(sample.available==123*1024 && sample.resident==7*1024,"Memory fields were not converted from kB");
    const auto zero=MemorySample::parse("MemAvailable: 0 kB","VmRSS: 0 kB");
    expect(zero.available==0 && zero.resident==0,"Zero-valued memory fields were treated as missing");
    const auto absent=MemorySample::parse("MemTotal: 20 kB\nMemAvailableExtra: 3 kB",
                                         "Name: test\nVmRSSExtra: 3 kB");
    expect(!absent.available && !absent.resident,"Missing fields or prefix matches were accepted");
    const std::array<std::string,13> invalid{
        "", "1", "1 B", "1 KB", "1 kB extra", "-1 kB", "+1 kB", "1.5 kB",
        "1x kB", "x1 kB", "18446744073709551616 kB", "18014398509481984 kB", "18446744073709551615 kB"
    };
    for(const auto& value:invalid) {
        const auto available=MemorySample::parse("MemAvailable: "+value+"\n","VmRSS: 2 kB\n");
        expect(!available.available && available.resident==2048,"Invalid available field affected RSS or was accepted");
        const auto resident=MemorySample::parse("MemAvailable: 2 kB\n","VmRSS: "+value+"\n");
        expect(resident.available==2048 && !resident.resident,"Invalid RSS field affected available memory or was accepted");
    }
    const auto duplicate=MemorySample::parse("MemAvailable: 0 kB\nMemAvailable: 1 kB\n",
                                           "VmRSS: 2 kB\nVmRSS: 2 kB\n");
    expect(!duplicate.available && !duplicate.resident,"Duplicate memory fields were accepted");
    const auto malformedDuplicate=MemorySample::parse("MemAvailable: 1 kB\nMemAvailable: x kB\n",
                                                     "VmRSS: x kB\nVmRSS: 1 kB\n");
    expect(!malformedDuplicate.available && !malformedDuplicate.resident,"Malformed duplicate memory field was ignored");
    const auto largest=std::to_string(maximum/1024)+" kB\n";
    const auto boundary=MemorySample::parse("MemAvailable: "+largest,"VmRSS: "+largest);
    expect(boundary.available==(maximum/1024)*1024 && boundary.resident==(maximum/1024)*1024,
           "Largest representable kB field was rejected or overflowed");
}

void policyAndProbeFailures() {
    rejects([]{MemoryAdmission admission(0,0,[]{return MemorySample{100,0};});});
    rejects([]{MemoryAdmission admission(100,0,MemoryAdmission::Probe{});});
    MemorySample sample{100,10};
    unsigned calls=0;
    bool throwProbe=false;
    MemoryAdmission admission(100,20,[&] {
        ++calls;
        if(throwProbe) throw std::runtime_error("Injected probe failure");
        return sample;
    });
    expect(admission.applicationLimit()==100,"Application policy value changed");
    rejects([&]{admission.reserve(1,static_cast<Kind>(99));});
    expect(calls==0,"Invalid allocation kind sampled the system");
    for(const auto failure:{Failure::None,Failure::ApplicationLimit,Failure::SystemHeadroom,Failure::MissingProbe})
        expect(std::string_view(MemoryAdmission::message(failure)).size()>0,"Admission result has no diagnostic");
    sample.available.reset();
    denied(admission,1,Failure::MissingProbe);
    sample={100,std::nullopt};
    denied(admission,1,Failure::MissingProbe,Kind::Gpu);
    sample={std::nullopt,std::nullopt};
    denied(admission,0,Failure::MissingProbe);
    sample={100,10};
    auto failure=Failure::MissingProbe;
    auto hold=admission.reserve(20,Kind::Cpu,&failure);
    expect(hold && failure==Failure::None,"Successful reservation retained an old failure result");
    ledger(admission,20);
    throwProbe=true;
    rejects([&]{admission.reserve(5);});
    ledger(admission,20);
    throwProbe=false;
    sample={19,10};
    denied(admission,0,Failure::SystemHeadroom);
    sample={100,101};
    denied(admission,0,Failure::ApplicationLimit);
    sample={100,10};
    hold.reset();
    auto zero=admission.reserve(0);
    expect(zero && zero->bytes()==0,"Zero-byte ownership was not admitted under a valid policy");
    zero->commit();zero->commit();zero.reset();
    ledger(admission);
    expect(calls>=8,"Probe values were not sampled for successive requests");
}

void applicationAccounting() {
    MemorySample sample{1000,100};
    MemoryAdmission admission(500,100,[&]{return sample;});
    auto cpu=admission.reserve(120);
    auto gpu=admission.reserve(30,Kind::Gpu);
    expect(cpu && gpu && cpu->bytes()==120 && gpu->bytes()==30,"Pending CPU/GPU reservations failed");
    ledger(admission,120,30);
    cpu->commit();cpu->commit();
    ledger(admission,0,30,120);
    gpu->commit();
    ledger(admission,0,0,120,30);
    auto peak=admission.reserve(250);
    expect(peak.has_value(),"RSS plus all tracked backing failed at the exact application limit");
    ledger(admission,250,0,120,30);
    denied(admission,1,Failure::ApplicationLimit,Kind::Gpu);
    peak.reset();
    sample.resident=250;
    auto measuredPeak=admission.reserve(100);
    expect(measuredPeak.has_value(),"Measured RSS or additional GPU accounting is incorrect");
    denied(admission,1,Failure::ApplicationLimit);
    measuredPeak.reset();gpu.reset();cpu.reset();
    ledger(admission);
}

void swappedBackingCannotHideBehindRss() {
    constexpr std::uint64_t GiB=1024ull*1024*1024;
    MemorySample sample{3*GiB,0};
    MemoryAdmission admission(2*GiB,GiB/2,[&]{return sample;});
    auto tiles=admission.require(GiB);tiles.commit();
    // The RSS may belong entirely to toolkit/codec pages while tiles are
    // swapped out. No byte-level residency proof is available from procfs.
    sample={GiB,GiB};
    denied(admission,GiB/4,Failure::ApplicationLimit);
    sample.resident=0;
    denied(admission,GiB/4,Failure::SystemHeadroom);
    ledger(admission,0,0,GiB);
}

void sharedHeadroom() {
    MemorySample sample{100,0};
    MemoryAdmission admission(1000,20,[&]{return sample;});
    auto cpu=admission.reserve(50);
    auto gpu=admission.reserve(30,Kind::Gpu);
    expect(cpu && gpu,"Exact shared headroom could not be reserved");
    denied(admission,1,Failure::SystemHeadroom);
    denied(admission,1,Failure::SystemHeadroom,Kind::Gpu);
    cpu->commit();
    ledger(admission,0,30,50);
    denied(admission,1,Failure::SystemHeadroom);
    gpu->commit();
    ledger(admission,0,0,50,30);
    denied(admission,1,Failure::SystemHeadroom);
    gpu.reset();
    sample={50,50};
    denied(admission,0,Failure::SystemHeadroom);
    sample.available=100;
    auto next=admission.reserve(30);
    expect(next.has_value(),"Conservative CPU backing headroom failed at the exact limit");
    denied(admission,1,Failure::SystemHeadroom);
    next.reset();
    sample.available=50;
    sample.resident=0;
    denied(admission,0,Failure::SystemHeadroom);
    cpu.reset();
    auto recovered=admission.reserve(30,Kind::Gpu);
    expect(recovered.has_value(),"Released backing did not restore headroom");
    recovered->commit();
    denied(admission,1,Failure::SystemHeadroom);
    recovered.reset();ledger(admission);
}

void requiredClaims() {
    MemorySample sample{100,0};
    MemoryAdmission admission(60,20,[&]{return sample;});
    {
        auto required=admission.require(40);
        ledger(admission,40);
        required.commit();
        ledger(admission,0,0,40);
        for(const unsigned scenario:{0u,1u,2u}) {
            const auto before=admission.snapshot();
            sample=scenario==0 ? MemorySample{100,0} : scenario==1 ? MemorySample{50,0}
                                                                          : MemorySample{std::nullopt,0};
            bool threw=false;
            try {admission.require(scenario==0 ? 21 : 1,Kind::Gpu);}
            catch(const std::length_error& error) {threw=true;expect(*error.what()!=0,"Required reservation lacks a diagnostic");}
            expect(threw && same(before,admission.snapshot()),"Required admission failed without a safe rollback");
        }
    }
    ledger(admission);
}

void reclamationRetry() {
    unsigned probes=0,hooks=0;
    MemoryAdmission admission(60,0,[&]{++probes;return MemorySample{1000,0};});
    auto backing=admission.reserve(40);
    expect(backing.has_value(),"Reclamation setup was not admitted");
    backing->commit();
    admission.setReclaimer([&](std::uint64_t requested) {
        ++hooks;
        expect(requested==30,"Reclaimer received a different requested allocation size");
        ledger(admission,0,0,40);
        // Snapshot, release and callback replacement all lock the ledger. This
        // callback would deadlock if require retained its admission lock.
        backing.reset();
        admission.setReclaimer({});
        ledger(admission);
    });
    auto recovered=admission.require(30,Kind::Gpu);
    expect(hooks==1 && probes==3,"Reclamation did not make exactly one retry after denial");
    ledger(admission,0,30);
    recovered.commit();
    ledger(admission,0,0,0,30);

    // Optional admission is a non-reclaiming query; callers using require own
    // the worker-side synchronous IO/retry policy.
    admission.setReclaimer([&](std::uint64_t){++hooks;});
    denied(admission,31,Failure::ApplicationLimit);
    expect(hooks==1,"Optional reserve unexpectedly ran synchronous reclamation");
}

void ineffectiveAndMissingReclamation() {
    MemorySample sample{100,0};
    unsigned probes=0,hooks=0;
    MemoryAdmission admission(60,20,[&]{++probes;return sample;});
    auto backing=admission.require(40);
    backing.commit();
    admission.setReclaimer([&](std::uint64_t requested) {
        ++hooks;
        expect(requested==21,"Ineffective reclaimer received incorrect request size");
        ledger(admission,0,0,40);
    });
    const auto before=admission.snapshot();
    rejects([&]{admission.require(21);});
    expect(hooks==1 && probes==3 && same(before,admission.snapshot()),
        "Ineffective reclamation retried repeatedly or altered live backing");
    sample.available.reset();
    rejects([&]{admission.require(21);});
    expect(hooks==1 && probes==4 && same(before,admission.snapshot()),
        "Missing memory probe triggered reclamation or modified accounting");
    sample={50,0};
    admission.setReclaimer([&](std::uint64_t requested) {
        ++hooks;
        expect(requested==1,"Headroom retry received incorrect request size");
        ledger(admission,0,0,40);
        sample.available=100;
    });
    auto headroom=admission.require(1);
    expect(hooks==2 && probes==6,"System headroom denial did not reprobe exactly once");
    ledger(admission,1,0,40);
}

void recursiveAndThrowingReclamation() {
    unsigned probes=0,hooks=0;
    MemoryAdmission admission(60,0,[&]{++probes;return MemorySample{1000,0};});
    auto backing=admission.reserve(40);
    expect(backing.has_value(),"Recursive reclamation setup failed");
    backing->commit();
    admission.setReclaimer([&](std::uint64_t) {
        ++hooks;
        const auto before=admission.snapshot();
        // Deliberate contract violation exercises the defensive recursion guard.
        rejects([&]{admission.require(30);});
        expect(same(before,admission.snapshot()),"Recursive denial changed the ledger");
    });
    rejects([&]{admission.require(30);});
    expect(hooks==1 && probes==4,"Recursive reclamation exceeded one outer hook and retry");
    ledger(admission,0,0,40);
    admission.setReclaimer([&](std::uint64_t) {
        ++hooks;
        ledger(admission,0,0,40);
        throw std::runtime_error("Injected reclamation failure");
    });
    bool injected=false;
    try {admission.require(30);}
    catch(const std::runtime_error& error) {injected=std::string_view(error.what())=="Injected reclamation failure";}
    expect(injected && hooks==2 && probes==5,"Throwing reclaimer failed to propagate without a retry");
    ledger(admission,0,0,40);
    // Exception unwinding must restore the thread-local guard for later calls.
    admission.setReclaimer([&](std::uint64_t) {++hooks;backing.reset();});
    auto recovered=admission.require(30);
    expect(hooks==3 && probes==7,"Throwing hook left reclamation disabled on its thread");
    ledger(admission,30);
}

void ownershipAndRollback() {
    static_assert(!std::is_copy_constructible_v<MemoryAdmission::Reservation>);
    static_assert(!std::is_copy_assignable_v<MemoryAdmission::Reservation>);
    static_assert(std::is_nothrow_move_constructible_v<MemoryAdmission::Reservation>);
    static_assert(std::is_nothrow_move_assignable_v<MemoryAdmission::Reservation>);
    MemoryAdmission admission(200,0,[]{return MemorySample{200,0};});
    auto source=admission.reserve(20);
    auto target=admission.reserve(30,Kind::Gpu);
    expect(source && target,"Move ownership setup failed");
    source->commit();
    *target=std::move(*source);
    ledger(admission,0,0,20);
    source->commit();source.reset();
    ledger(admission,0,0,20);
    auto* self=&*target;
    *target=std::move(*self);
    ledger(admission,0,0,20);
    target.reset();ledger(admission);

    source=admission.reserve(20,Kind::Gpu);
    target=admission.reserve(30);
    expect(source && target,"Pending move ownership setup failed");
    target->commit();
    *target=std::move(*source);
    source.reset();
    ledger(admission,0,20);
    {
        auto moved=std::move(*target);
        target.reset();
        ledger(admission,0,20);
        moved.commit();moved.commit();
        ledger(admission,0,0,0,20);
    }
    ledger(admission);
    for(const bool commit:{false,true}) {
        bool acquired=false;
        rejects([&] {
            auto temporary=admission.reserve(40);
            expect(temporary.has_value(),"Rollback setup failed");
            acquired=true;
            if(commit) temporary->commit();
            throw std::runtime_error("Injected allocation/caller failure");
        });
        expect(acquired,"Rollback path did not obtain its reservation");
        ledger(admission);
    }
    auto surviving=[] {
        MemoryAdmission local(100,0,[]{return MemorySample{100,0};});
        return local.reserve(30,Kind::Gpu);
    }();
    expect(surviving.has_value(),"Reservation did not retain its admission owner");
    surviving->commit();surviving.reset();
}

void integerBoundaries() {
    MemoryAdmission all(maximum,0,[]{return MemorySample{maximum,0};});
    auto largest=all.reserve(maximum);
    expect(largest && largest->bytes()==maximum,"Maximum CPU reservation overflowed");
    denied(all,1,Failure::ApplicationLimit,Kind::Gpu);
    largest->commit();
    ledger(all,0,0,maximum);
    denied(all,1,Failure::ApplicationLimit);
    largest.reset();ledger(all);
    auto cpu=all.reserve(maximum-1);
    auto gpu=all.reserve(1,Kind::Gpu);
    expect(cpu && gpu,"Mixed CPU/GPU sum at uint64 boundary was rejected");
    cpu->commit();gpu->commit();
    ledger(all,0,0,maximum-1,1);
    denied(all,1,Failure::ApplicationLimit);
    cpu.reset();gpu.reset();ledger(all);
    auto largestGpu=all.reserve(maximum,Kind::Gpu);
    expect(largestGpu.has_value(),"Maximum GPU reservation overflowed");
    largestGpu->commit();
    ledger(all,0,0,0,maximum);
    denied(all,1,Failure::ApplicationLimit);
    largestGpu.reset();ledger(all);
    MemoryAdmission one(maximum,maximum-1,[]{return MemorySample{maximum,0};});
    auto byte=one.reserve(1);
    expect(byte.has_value(),"Exact one-byte system headroom boundary failed");
    denied(one,1,Failure::SystemHeadroom,Kind::Gpu);
    byte.reset();ledger(one);
    MemoryAdmission none(maximum,maximum,[]{return MemorySample{maximum-1,0};});
    denied(none,0,Failure::SystemHeadroom);
    MemorySample sample{maximum,maximum-1};
    MemoryAdmission rss(maximum,0,[&]{return sample;});
    auto final=rss.reserve(1,Kind::Gpu);
    expect(final.has_value(),"RSS plus GPU at uint64 boundary failed");
    final->commit();
    denied(rss,1,Failure::ApplicationLimit);
    sample.resident=maximum;
    denied(rss,0,Failure::ApplicationLimit);
    final.reset();ledger(rss);
}

void concurrentClaims() {
    constexpr unsigned workerCount=16;
    for(const bool systemLimit:{false,true}) for(unsigned round=0;round<4;++round) {
        std::atomic<unsigned> calls=0,accepted=0;
        std::atomic<bool> failure=false;
        MemoryAdmission admission(systemLimit ? 1000 : 64,systemLimit ? 36 : 0,[&] {
            ++calls;
            return MemorySample{systemLimit ? 100u : 1000u,0};
        });
        std::barrier start(workerCount+1),attempted(workerCount+1),release(workerCount+1);
        std::vector<std::jthread> workers;
        for(unsigned i=0;i<workerCount;++i) workers.emplace_back([&,i] {
            start.arrive_and_wait();
            std::optional<MemoryAdmission::Reservation> claim;
            try {
                auto reason=Failure::None;
                claim=admission.reserve(8,i%2 ? Kind::Gpu : Kind::Cpu,&reason);
                if(claim) {
                    ++accepted;
                    if(reason!=Failure::None) failure=true;
                    if(round%2 && i%3) claim->commit();
                } else if(reason!=(systemLimit ? Failure::SystemHeadroom : Failure::ApplicationLimit)) failure=true;
            } catch(...) {failure=true;}
            attempted.arrive_and_wait();
            release.arrive_and_wait();
        });
        start.arrive_and_wait();attempted.arrive_and_wait();
        const auto held=admission.snapshot();
        const bool bounded=held.pendingCpu+held.pendingGpu+held.committedCpu+held.committedGpu==64;
        release.arrive_and_wait();
        for(auto& worker:workers) worker.join();
        expect(!failure && accepted==8 && calls==workerCount && bounded,
               "Concurrent claims overspent shared application/system capacity");
        ledger(admission);
    }
}
}
int main() {
    try {
        parsing();policyAndProbeFailures();applicationAccounting();swappedBackingCannotHideBehindRss();sharedHeadroom();requiredClaims();
        reclamationRetry();ineffectiveAndMissingReclamation();recursiveAndThrowingReclamation();
        ownershipAndRollback();integerBoundaries();concurrentClaims();
        std::cout<<"memory probes, bounded reclamation retries, CPU/GPU accounting, ownership and bounded concurrency passed\n";
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
