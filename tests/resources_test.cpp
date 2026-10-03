#include "core/resources.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>

using namespace compositor;
namespace {
constexpr std::uint64_t mib=1024*1024,gib=1024*mib;
void expect(bool value,const char* message) {if(!value) throw std::runtime_error(message);}
template<class F> void rejects(F action) {
    try {action();} catch(const std::exception&) {return;}
    throw std::runtime_error("Invalid resource configuration accepted");
}
template<class F> void eventually(F predicate) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!predicate()) {
        if(std::chrono::steady_clock::now()>deadline) throw std::runtime_error("Worker coordination timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
void profiles() {
    const auto laptop=ResourceLimits::forMachine(7948062720,8);
    expect(laptop.lowMemory && laptop.computeWorkers==2,"Actual LP8 inventory did not select bounded workers");
    expect(laptop.canonicalTiles==768*mib && laptop.cpuMips==8*mib && laptop.thumbnailMips==4*mib,"LP8 source/CPU component ceilings");
    expect(laptop.gpuTiles==8*mib && laptop.gpuMips==16*mib && laptop.gpuFrame==64*mib,"LP8 GPU component ceilings");
    expect(ResourceLimits::forMachine(8*gib,8).lowMemory,"Installed 8 GiB boundary");
    expect(ResourceLimits::forMachine(4*gib,1).canonicalTiles==512*mib,"Smaller RAM scales canonical ceiling");
    const auto unknown=ResourceLimits::forMachine(0,0);
    expect(unknown.lowMemory && unknown.computeWorkers==1,"Unknown inventory was not conservative");
    const auto workstation=ResourceLimits::forMachine(32*gib,8);
    expect(!workstation.lowMemory && workstation.computeWorkers==6 && workstation.canonicalTiles==4*gib,"Workstation ceiling/regression");
    expect(workstation.gpuFrame==256*mib && workstation.gpuMips==64*mib,"Workstation GPU ceilings changed");
    expect(ResourceLimits::forMachine(std::numeric_limits<std::uint64_t>::max(),1).canonicalTiles==4*gib,"Large RAM arithmetic overflow");
    rejects([]{WorkScheduler scheduler(0);});
    rejects([]{WorkScheduler scheduler(1,2);});
}
void priorityAndCancellation() {
    WorkScheduler scheduler(1);
    auto holder=scheduler.acquire(WorkScheduler::Priority::Processing,false);
    std::mutex mutex;std::vector<int> order;
    const auto run=[&](WorkScheduler::Priority priority,int id,std::stop_token stop) {
        auto permit=scheduler.acquire(priority,false,stop);
        if(permit) {std::lock_guard lock(mutex);order.push_back(id);}
    };
    std::jthread background([&](auto stop){run(WorkScheduler::Priority::Background,2,stop);});
    eventually([&]{return scheduler.snapshot().waiting[2]==1;});
    std::jthread processing([&](auto stop){run(WorkScheduler::Priority::Processing,1,stop);});
    eventually([&]{return scheduler.snapshot().waiting[1]==1;});
    std::jthread interactive([&](auto stop){run(WorkScheduler::Priority::Interactive,0,stop);});
    eventually([&]{return scheduler.snapshot().waiting[0]==1;});
    holder.reset();interactive.join();processing.join();background.join();
    expect(order==std::vector<int>({0,1,2}),"Queued priority inversion");

    holder=scheduler.acquire(WorkScheduler::Priority::Processing,false);
    std::atomic<bool> aborted=false;
    std::jthread stopping([&](auto stop){aborted=!scheduler.acquire(WorkScheduler::Priority::Interactive,false,stop);});
    eventually([&]{return scheduler.snapshot().waiting[0]==1;});
    stopping.request_stop();stopping.join();
    expect(aborted && scheduler.snapshot().waiting[0]==0,"Stop left queued admission behind");
    std::atomic<bool> stale=false;
    std::jthread replaced([&](auto stop){aborted=!scheduler.acquire(WorkScheduler::Priority::Interactive,false,stop,[&]{return stale.load();});});
    eventually([&]{return scheduler.snapshot().waiting[0]==1;});
    stale=true;replaced.join();
    expect(aborted && scheduler.snapshot().active==1,"Superseded work acquired a slot");
    rejects([&]{scheduler.acquire(static_cast<WorkScheduler::Priority>(9),false);});
    int checks=0;
    rejects([&]{scheduler.acquire(WorkScheduler::Priority::Interactive,false,{},[&]{if(++checks==2) throw std::runtime_error("predicate");return false;});});
    expect(scheduler.snapshot().waiting[0]==0,"Throwing cancellation predicate leaked admission");
    holder.reset();
    expect(scheduler.snapshot().active==0,"Permit release failed");
}
void heavyAndOwnership() {
    WorkScheduler scheduler(2);
    auto heavy=scheduler.acquire(WorkScheduler::Priority::Processing,true);
    std::atomic<bool> acquired=false;
    std::jthread queued([&](auto stop){auto permit=scheduler.acquire(WorkScheduler::Priority::Processing,true,stop);acquired=permit.has_value();});
    eventually([&]{return scheduler.snapshot().waiting[1]==1;});
    // An ineligible heavy job must not waste the free slot or block previews.
    auto light=scheduler.acquire(WorkScheduler::Priority::Background,false);
    expect(light && scheduler.snapshot().active==2 && scheduler.snapshot().heavy==1 && !acquired,"Heavy admission did not leave a usable second slot");
    auto moved=std::move(heavy);
    expect(scheduler.snapshot().active==2,"Moving a permit released its slot");
    moved.reset();queued.join();light.reset();
    expect(acquired && scheduler.snapshot().active==0 && scheduler.snapshot().heavy==0,"Heavy permit was not retired");
    // A permit safely retains admission state after its scheduler owner dies.
    auto surviving=[] {WorkScheduler local(1);return local.acquire(WorkScheduler::Priority::Interactive,false);}();
    expect(surviving.has_value(),"Permit ownership lost");surviving.reset();
}
void contention() {
    WorkScheduler scheduler(2);
    std::atomic<unsigned> active=0,heavy=0;
    std::atomic<bool> failure=false;
    std::vector<std::jthread> workers;
    for(unsigned i=0;i<12;++i) workers.emplace_back([&,i](auto stop) {
        for(unsigned job=0;job<40;++job) {
            const bool memoryHeavy=(i+job)%3==0;
            auto permit=scheduler.acquire(static_cast<WorkScheduler::Priority>(i%3),memoryHeavy,stop);
            if(!permit) {failure=true;return;}
            if(++active>2) failure=true;
            if(memoryHeavy && ++heavy>1) failure=true;
            std::this_thread::yield();
            if(memoryHeavy) --heavy;
            --active;
        }
    });
    for(auto& worker:workers) worker.join();
    const auto remaining=scheduler.snapshot();
    expect(!failure && remaining.active==0 && remaining.heavy==0 && remaining.waiting==std::array<unsigned,3>{},"Concurrent work exceeded/leaked admission");
}
}
int main() {
    try {
        profiles();priorityAndCancellation();heavyAndOwnership();contention();
        std::cout<<"resource profiles, shared worker bounds, priority, cancellation and permit ownership passed\n";
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
