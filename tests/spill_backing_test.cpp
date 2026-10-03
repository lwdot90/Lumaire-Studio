#include "io/spill_backing.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <fcntl.h>
#include <future>
#include <iostream>
#include <limits>
#include <linux/magic.h>
#include <new>
#include <semaphore>
#include <source_location>
#include <sstream>
#include <stdexcept>
#include <sys/vfs.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <vector>

using namespace compositor;
using namespace compositor::io;
namespace fs=std::filesystem;
namespace {
constexpr std::uint64_t abundant=2*1024*1024, headroom=64;
void expect(bool value, const char* message) { if(!value) throw std::runtime_error(message); }
template<class Error,class F> void rejects(F&& action) {
    try { action(); } catch(const Error&) { return; }
    throw std::runtime_error("Expected backing operation failure");
}
template<class F> void rejectsSpill(SpillErrorCode code, F&& action) {
    try { action(); }
    catch(const SpillError& error) { expect(error.code()==code,"Unexpected spill backing error category"); return; }
    throw std::runtime_error("Expected spill backing failure");
}
void ledger(const MemoryAdmission& memory, std::uint64_t committed, std::uint64_t pending=0,
            const std::source_location where=std::source_location::current()) {
    const auto snapshot=memory.snapshot();
    if(snapshot.committedCpu==committed && snapshot.pendingCpu==pending && !snapshot.pendingGpu && !snapshot.committedGpu) return;
    std::ostringstream message;
    message<<where.function_name()<<':'<<where.line()<<": expected CPU committed="<<committed<<" pending="<<pending
        <<"; got committed="<<snapshot.committedCpu<<" pending="<<snapshot.pendingCpu
        <<" GPU committed="<<snapshot.committedGpu<<" pending="<<snapshot.pendingGpu;
    throw std::runtime_error(message.str());
}
struct Fixture {
    fs::path path;
    explicit Fixture(const fs::path& parent) {
        auto pattern=(parent/"spill-backing-test-XXXXXX").string();
        const auto created=::mkdtemp(pattern.data());
        expect(created!=nullptr,"Create small owned backing fixture");
        path=created;
    }
    ~Fixture() {
        // This uniquely created fixture tree only; never the build parent.
        std::error_code error;
        fs::remove_all(path,error);
    }
};
struct Probe {
    std::atomic<std::uint64_t> available{abundant}, resident{0}, calls{0};
    std::atomic<bool> missing{false}, fail{false};
};
struct Environment {
    std::shared_ptr<Probe> probe=std::make_shared<Probe>();
    std::shared_ptr<MemoryAdmission> memory=std::make_shared<MemoryAdmission>(abundant,headroom,[sample=probe] {
        ++sample->calls;
        if(sample->fail) throw std::runtime_error("Injected memory probe failure");
        if(sample->missing) return MemorySample{std::nullopt,0};
        return MemorySample{sample->available.load(),sample->resident.load()};
    });
};
SpillLimits limits() { return {2*1024*1024,65536,0,64,2,128}; }
std::shared_ptr<SpillStore> storeAt(const Fixture& fixture, SpillFault fault={}) {
    return std::make_shared<SpillStore>(fixture.path,limits(),std::move(fault));
}
std::vector<std::uint8_t> bytes(std::size_t count, unsigned seed=0) {
    std::vector<std::uint8_t> result(count);
    for(std::size_t i=0;i<count;++i) result[i]=static_cast<std::uint8_t>((i*73+seed*29+(i>>8))&255);
    return result;
}
void exact(const ResidentLease& lease, std::span<const std::uint8_t> expected) {
    expect(bool(lease) && lease.size()==expected.size() &&
        std::equal(lease.bytes().begin(),lease.bytes().end(),expected.begin(),expected.end()),"Lease bytes are not exact");
}
fs::path onlyFile(const SpillStore& store) {
    fs::path path;
    std::size_t count=0;
    for(const auto& entry:fs::directory_iterator(store.sessionDirectory())) { path=entry.path(); ++count; }
    expect(count==1,"Expected one immutable disk payload");
    return path;
}
void flip(const fs::path& path, off_t offset) {
    const auto fd=::open(path.c_str(),O_RDWR|O_CLOEXEC);
    expect(fd>=0,"Open owned corrupt-read fixture");
    std::uint8_t byte=0;
    const auto read=::pread(fd,&byte,1,offset);
    byte^=0x80;
    const auto written=::pwrite(fd,&byte,1,offset);
    const auto closed=::close(fd);
    expect(read==1 && written==1 && closed==0,"Change one owned spill byte");
}
struct Blocker {
    std::atomic<bool> armed{false};
    std::binary_semaphore entered{0}, release{0};
    void block() {
        if(armed.exchange(false)) { entered.release(); release.acquire(); }
    }
    void wait() { expect(entered.try_acquire_for(std::chrono::seconds(2)),"Worker did not reach its controlled checkpoint"); }
};
struct ReleaseOnExit {
    Blocker* blocker;
    explicit ReleaseOnExit(Blocker& target):blocker(&target) {}
    void release() { if(auto* target=std::exchange(blocker,nullptr)) target->release.release(); }
    ~ReleaseOnExit() { release(); }
    ReleaseOnExit(const ReleaseOnExit&)=delete;
    ReleaseOnExit& operator=(const ReleaseOnExit&)=delete;
};

void apiAndRoundTrips(const fs::path& root) {
    static_assert(std::is_same_v<decltype(std::declval<const ResidentLease&>().bytes()),std::span<const std::uint8_t>>);
    static_assert(std::is_copy_constructible_v<ResidentLease> && std::is_copy_constructible_v<SpillBacking>);
    Fixture fixture(root); Environment environment;
    std::atomic<unsigned> creates=0;
    auto store=storeAt(fixture,[&](SpillIoStage stage,std::uint64_t) { if(stage==SpillIoStage::CreateFile) ++creates; return 0; });
    SpillBacking empty;
    ResidentLease emptyLease;
    expect(!empty && !emptyLease && !empty.spillHandle() && empty.size()==0 && emptyLease.bytes().empty() &&
        emptyLease.chargedBytes()==0 && !empty.status().busy,"Default backing/lease state differs");
    rejectsSpill(SpillErrorCode::InvalidArgument,[&]{empty.resident();});
    rejectsSpill(SpillErrorCode::InvalidArgument,[&]{empty.spill();});
    rejectsSpill(SpillErrorCode::InvalidArgument,[&]{SpillBacking::create(bytes(1),{},store);});
    rejectsSpill(SpillErrorCode::InvalidArgument,[&]{SpillBacking::create(bytes(1),environment.memory,{});});
    rejectsSpill(SpillErrorCode::InvalidArgument,[&]{SpillBacking::fromSpill({},environment.memory);});
    rejects<std::length_error>([]{SpillBacking::residentCharge(std::numeric_limits<std::uint64_t>::max());});
    ledger(*environment.memory,0);
    for(std::size_t size:std::array<std::size_t,9>{0,1,31,127,128,129,4095,4096,8193}) {
        auto input=bytes(size);
        const auto expected=input;
        const auto residentCharge=SpillBacking::residentCharge(size), stateCharge=SpillBacking::stateCharge();
        auto backing=SpillBacking::create(input,environment.memory,store);
        auto alias=backing;
        std::fill(input.begin(),input.end(),0);
        const auto probes=environment.probe->calls.load();
        auto old=alias.resident(); exact(old,expected);
        auto copy=old;
        expect(old.chargedBytes()==residentCharge && backing.size()==size && backing.status().resident &&
            !backing.status().spilled && !backing.status().busy,"Initial resident publication differs");
        expect(environment.probe->calls==probes,"Cache hit performed admission or allocated a new payload");
        ledger(*environment.memory,stateCharge+residentCharge);
        const auto before=creates.load();
        expect(backing.spill(),"First spill must drop cached resident copy");
        exact(old,expected); exact(copy,expected);
        ledger(*environment.memory,stateCharge+residentCharge);
        expect(!backing.status().resident && backing.status().spilled && !backing.status().busy &&
            backing.status().residentCharge==0 && creates==before+1,"First spill publication differs");
        auto disk=backing.spillHandle();
        expect(bool(disk) && disk.read()==expected && !backing.spill() && creates==before+1,"Duplicate spill rewrote immutable backing");
        auto fresh=alias.resident(); exact(fresh,expected);
        if(size) expect(old.bytes().data()!=fresh.bytes().data(),"Rehydration overwrote leased storage");
        ledger(*environment.memory,stateCharge+residentCharge*2);
        old={}; copy={};
        ledger(*environment.memory,stateCharge+residentCharge);
        expect(backing.spill() && creates==before+1,"Cached rehydration should reuse published disk handle");
        exact(fresh,expected); fresh={};
        ledger(*environment.memory,stateCharge);
        alias={}; backing={};
        ledger(*environment.memory,0);
        expect(store->stats().entries==1,"Exported handle must preserve disk after backing destruction");
        disk={};
        expect(store->stats().entries==0,"Final disk reference did not clean up");
    }
}
void ownerLifetimes(const fs::path& root) {
    Fixture fixture(root); Environment environment;
    auto store=storeAt(fixture);
    const auto session=store->sessionDirectory();
    const auto expected=bytes(513,2);
    auto observer=*environment.memory; // Same ledger, independently owned wrapper.
    std::weak_ptr<MemoryAdmission> admissionOwner=environment.memory;
    std::weak_ptr<SpillStore> storeOwner=store;
    ResidentLease retained;
    SpillBacking survivor;
    {
        auto original=SpillBacking::create(expected,environment.memory,store);
        retained=original.resident(); survivor=original;
        original.spill();
        store.reset(); environment.memory.reset();
    }
    expect(!admissionOwner.expired() && !storeOwner.expired(),"Backing lost its injected owners");
    exact(retained,expected);
    auto current=survivor.resident(); exact(current,expected); current={};
    auto exported=survivor.spillHandle();
    survivor={};
    expect(admissionOwner.expired() && storeOwner.expired() && fs::exists(session),"Exported disk handle or owner lifetime differs");
    ledger(observer,retained.chargedBytes());
    exported={};
    expect(!fs::exists(session),"Resident-only lease unnecessarily retained the disk session");
    exact(retained,expected); retained={};
    ledger(observer,0);

    Environment importedEnvironment;
    auto sourceStore=storeAt(fixture);
    auto source=sourceStore->put(expected);
    auto imported=SpillBacking::fromSpill(source,importedEnvironment.memory);
    ledger(*importedEnvironment.memory,SpillBacking::stateCharge());
    source={}; sourceStore.reset();
    auto loaded=imported.resident(); exact(loaded,expected);
    expect(imported.spill(),"Imported disk handle should support cache eviction without its store owner");
    exact(loaded,expected); loaded={}; imported={};
    ledger(*importedEnvironment.memory,0);
}
void memoryRefusal(const fs::path& root) {
    Fixture fixture(root); Environment environment;
    auto store=storeAt(fixture);
    const auto input=bytes(512);
    const auto stateCharge=SpillBacking::stateCharge(), residentCharge=SpillBacking::residentCharge(input.size());
    environment.probe->available=headroom;
    rejects<std::length_error>([&]{SpillBacking::create(input,environment.memory,store);});
    ledger(*environment.memory,0);
    environment.probe->available=headroom+stateCharge;
    rejects<std::length_error>([&]{SpillBacking::create(input,environment.memory,store);});
    ledger(*environment.memory,0);
    expect(store->stats().entries==0,"Resident admission refusal created disk backing");
    auto existing=store->put(input);
    environment.probe->available=headroom;
    rejects<std::length_error>([&]{SpillBacking::fromSpill(std::move(existing),environment.memory);});
    expect(bool(existing) && existing.read()==input,"Refused import consumed its caller's existing disk handle");
    ledger(*environment.memory,0); existing={};
    environment.probe->available=abundant;
    auto backing=SpillBacking::create(input,environment.memory,store);
    auto old=backing.resident(); backing.spill();
    environment.probe->available=headroom+stateCharge+residentCharge;
    rejects<std::length_error>([&]{backing.resident();});
    exact(old,input);
    ledger(*environment.memory,stateCharge+residentCharge);
    expect(!backing.status().resident && backing.status().spilled && !backing.status().busy && store->stats().entries==1,
        "Rehydration refusal lost disk backing or retained a transition");
    old={};
    auto lease=backing.resident(); exact(lease,input);
    environment.probe->available=0;
    const auto probes=environment.probe->calls.load();
    auto cached=backing.resident(); exact(cached,input);
    expect(probes==environment.probe->calls,"Existing cache hit should not reallocate under pressure");
    cached={}; lease={}; backing.spill();
    environment.probe->available=abundant; environment.probe->missing=true;
    rejects<std::length_error>([&]{backing.resident();});
    ledger(*environment.memory,stateCharge);
    environment.probe->missing=false; environment.probe->resident=abundant;
    rejects<std::length_error>([&]{backing.resident();});
    ledger(*environment.memory,stateCharge);
    environment.probe->resident=0; environment.probe->fail=true;
    rejects<std::runtime_error>([&]{backing.resident();});
    expect(!backing.status().busy && backing.status().spilled,"Probe failure changed published backing");
    environment.probe->fail=false;
    auto recovered=backing.resident(); exact(recovered,input);
}
void allocationFailures(const fs::path& root) {
    Fixture fixture(root); Environment environment;
    auto store=storeAt(fixture);
    const auto input=bytes(512);
    const auto stateCharge=SpillBacking::stateCharge(), residentCharge=SpillBacking::residentCharge(input.size());
    for(auto stage:{SpillBackingStage::AllocateResident,SpillBackingStage::PublishResident}) {
        rejects<std::bad_alloc>([&] {
            SpillBacking::create(input,environment.memory,store,{},[&](SpillBackingStage reached,std::uint64_t size) {
                if(reached==stage) {
                    expect(size==input.size(),"Allocation checkpoint size differs");
                    if(stage==SpillBackingStage::AllocateResident) ledger(*environment.memory,stateCharge,residentCharge);
                    else ledger(*environment.memory,stateCharge+residentCharge);
                    throw std::bad_alloc{};
                }
            });
        });
        ledger(*environment.memory,0);
    }
    std::atomic<bool> armed=false;
    auto target=SpillBackingStage::AllocateResident;
    auto backing=SpillBacking::create(input,environment.memory,store,{},[&](SpillBackingStage reached,std::uint64_t) {
        if(armed && reached==target) throw std::bad_alloc{};
    });
    auto old=backing.resident(); backing.spill();
    for(auto stage:{SpillBackingStage::AllocateResident,SpillBackingStage::PublishResident}) {
        target=stage; armed=true;
        rejects<std::bad_alloc>([&]{backing.resident();});
        armed=false;
        exact(old,input); ledger(*environment.memory,stateCharge+residentCharge);
        expect(!backing.status().resident && !backing.status().busy && backing.spillHandle().read()==input,
            "Failed allocation or publication lost backing or published partial bytes");
    }
    auto restored=backing.resident(); exact(restored,input);
    auto second=SpillBacking::create(input,environment.memory,store,{},[](SpillBackingStage stage,std::uint64_t) {
        if(stage==SpillBackingStage::PublishSpill) throw std::bad_alloc{};
    });
    const auto before=environment.memory->snapshot().committedCpu;
    const auto entries=store->stats().entries;
    rejects<std::bad_alloc>([&]{second.spill();});
    exact(second.resident(),input);
    expect(!second.spillHandle() && second.status().resident && store->stats().entries==entries,
        "Failure after put success replaced resident backing or leaked a published handle");
    ledger(*environment.memory,before);
}
void cancellation(const fs::path& root) {
    Fixture fixture(root); Environment environment;
    std::stop_source source;
    std::atomic<bool> armed=false, adapterArmed=false;
    auto ioTarget=SpillIoStage::Write;
    auto adapterTarget=SpillBackingStage::PublishSpill;
    auto store=storeAt(fixture,[&](SpillIoStage stage,std::uint64_t progress) {
        if(armed && stage==ioTarget && (stage==SpillIoStage::DirectorySync || progress>=128)) source.request_stop();
        return 0;
    });
    const auto input=bytes(512);
    auto backing=SpillBacking::create(input,environment.memory,store,{},[&](SpillBackingStage stage,std::uint64_t) {
        if(adapterArmed && stage==adapterTarget) source.request_stop();
    });
    auto old=backing.resident();
    const auto baseline=environment.memory->snapshot().committedCpu;
    source.request_stop();
    rejectsSpill(SpillErrorCode::Cancelled,[&]{backing.spill(source.get_token());});
    rejectsSpill(SpillErrorCode::Cancelled,[&]{backing.resident(source.get_token());});
    rejectsSpill(SpillErrorCode::Cancelled,[&]{SpillBacking::create(input,environment.memory,store,source.get_token());});
    for(auto stage:{SpillIoStage::Write,SpillIoStage::DirectorySync}) {
        source=std::stop_source{}; ioTarget=stage; armed=true;
        rejectsSpill(SpillErrorCode::Cancelled,[&]{backing.spill(source.get_token());});
        armed=false;
        expect(!backing.spillHandle() && backing.status().resident && !backing.status().busy && store->stats().entries==0,
            "Canceled write lost the resident copy or published disk backing");
        exact(old,input); ledger(*environment.memory,baseline);
    }
    source=std::stop_source{}; adapterArmed=true;
    rejectsSpill(SpillErrorCode::Cancelled,[&]{backing.spill(source.get_token());});
    adapterArmed=false;
    expect(!backing.spillHandle() && store->stats().entries==0,"Late spill cancellation retained a new disk handle");
    ledger(*environment.memory,baseline);
    backing.spill();
    source=std::stop_source{}; ioTarget=SpillIoStage::Read; armed=true;
    rejectsSpill(SpillErrorCode::Cancelled,[&]{backing.resident(source.get_token());});
    armed=false;
    expect(!backing.status().resident && backing.status().spilled && !backing.status().busy,"Canceled read published a resident cache");
    exact(old,input); ledger(*environment.memory,baseline);
    source=std::stop_source{}; adapterTarget=SpillBackingStage::PublishResident; adapterArmed=true;
    rejectsSpill(SpillErrorCode::Cancelled,[&]{backing.resident(source.get_token());});
    adapterArmed=false;
    expect(!backing.status().resident && store->stats().entries==1,"Canceled verified read lost disk backing");
    ledger(*environment.memory,baseline);
    auto recovered=backing.resident(); exact(recovered,input);
    source.request_stop();
    auto handle=backing.spillHandle();
    rejectsSpill(SpillErrorCode::Cancelled,[&]{SpillBacking::fromSpill(handle,environment.memory,source.get_token());});
    expect(handle.read()==input,"Canceled import consumed existing disk handle");
}
void diskFailuresAndCorruption(const fs::path& root) {
    Fixture fixture(root); Environment environment;
    std::atomic<bool> armed=false;
    int error=EIO;
    auto target=SpillIoStage::Write;
    auto store=storeAt(fixture,[&](SpillIoStage stage,std::uint64_t progress) {
        return armed && stage==target && (stage!=SpillIoStage::Write || progress>=128) ? error : 0;
    });
    const auto input=bytes(512);
    auto backing=SpillBacking::create(input,environment.memory,store);
    auto old=backing.resident();
    const auto baseline=environment.memory->snapshot().committedCpu;
    for(int injected:{EIO,ENOSPC,EDQUOT}) {
        error=injected;
        for(auto stage:{SpillIoStage::Write,SpillIoStage::FileSync,SpillIoStage::FileClose,SpillIoStage::DirectorySync}) {
            target=stage; armed=true;
            rejectsSpill(injected==EIO ? SpillErrorCode::Io : SpillErrorCode::DiskSpace,[&]{backing.spill();});
            armed=false;
            expect(backing.status().resident && !backing.spillHandle() && !backing.status().busy && store->stats().entries==0,
                "Failed write/sync/close/disk allocation replaced resident backing");
            exact(backing.resident(),input); exact(old,input); ledger(*environment.memory,baseline);
        }
    }
    backing.spill();
    target=SpillIoStage::Read; error=EIO; armed=true;
    rejectsSpill(SpillErrorCode::Io,[&]{backing.resident();});
    armed=false;
    ledger(*environment.memory,baseline);
    const auto path=onlyFile(*store);
    for(off_t offset:{0,32}) {
        flip(path,offset);
        rejectsSpill(SpillErrorCode::Corrupt,[&]{backing.resident();});
        expect(!backing.status().resident && backing.status().spilled && !backing.status().busy && store->stats().entries==1,
            "Corrupt read changed the existing disk backing or published unverified bytes");
        ledger(*environment.memory,baseline); exact(old,input);
        flip(path,offset);
    }
    auto recovered=backing.resident(); exact(recovered,input);
    ledger(*environment.memory,baseline+SpillBacking::residentCharge(input.size()));

    std::atomic<bool> failBoth=false;
    auto blockedStore=storeAt(fixture,[&](SpillIoStage stage,std::uint64_t progress) {
        if(failBoth && stage==SpillIoStage::Write && progress>=128) return ENOSPC;
        if(failBoth && stage==SpillIoStage::RemoveFile) return EACCES;
        return 0;
    });
    auto preserved=SpillBacking::create(input,environment.memory,blockedStore);
    const auto before=environment.memory->snapshot().committedCpu;
    failBoth=true;
    rejectsSpill(SpillErrorCode::DiskSpace,[&]{preserved.spill();});
    expect(preserved.status().resident && !preserved.spillHandle() && !preserved.status().busy &&
        blockedStore->stats().cleanupBlocked,"Failed partial cleanup lost the sole resident backing");
    exact(preserved.resident(),input); ledger(*environment.memory,before);
    rejectsSpill(SpillErrorCode::CleanupFailed,[&]{preserved.spill();});
    failBoth=false; blockedStore->collectGarbage();
    expect(preserved.spill(),"Spill did not recover after explicit garbage collection");
    auto retry=preserved.resident(); exact(retry,input);
}
void concurrentAccess(const fs::path& root) {
    Fixture fixture(root); Environment environment;
    std::atomic<unsigned> creates=0, allocations=0;
    Blocker blocker;
    auto store=storeAt(fixture,[&](SpillIoStage stage,std::uint64_t) { if(stage==SpillIoStage::CreateFile) ++creates; return 0; });
    const auto input=bytes(1024,3);
    auto backing=SpillBacking::create(input,environment.memory,store,{},[&](SpillBackingStage stage,std::uint64_t) {
        if(stage==SpillBackingStage::AllocateResident) ++allocations;
        if(stage==SpillBackingStage::PublishResident) blocker.block();
    });
    std::array<std::exception_ptr,4> errors{};
    std::array<std::jthread,4> workers;
    std::array<bool,4> spilled{};
    std::barrier start(4);
    for(std::size_t i=0;i<workers.size();++i) workers[i]=std::jthread([&,i,job=backing] {
        try { start.arrive_and_wait(); spilled[i]=job.spill(); }
        catch(...) { errors[i]=std::current_exception(); }
    });
    for(auto& worker:workers) worker.join();
    for(auto failure:errors) if(failure) std::rethrow_exception(failure);
    expect(std::count(spilled.begin(),spilled.end(),true)==1 && creates==1 && store->stats().entries==1,
        "Concurrent spills published duplicate disk backing");
    ledger(*environment.memory,SpillBacking::stateCharge());
    std::array<ResidentLease,4> leases;
    blocker.armed=true;
    for(std::size_t i=0;i<workers.size();++i) workers[i]=std::jthread([&,i,job=backing] {
        try { start.arrive_and_wait(); leases[i]=job.resident(); }
        catch(...) { errors[i]=std::current_exception(); }
    });
    ReleaseOnExit release(blocker);
    // If this assertion fails, release all workers before unwinding the guard.
    bool reached=blocker.entered.try_acquire_for(std::chrono::seconds(2));
    const auto observed=backing.status();
    const auto observedAllocations=allocations.load();
    const auto observedMemory=environment.memory->snapshot();
    release.release();
    for(auto& worker:workers) worker.join();
    expect(reached && observed.busy && !observed.resident && observed.spilled && observedAllocations==2,
        "Concurrent cache misses allocated or published before full verification");
    expect(observedMemory.committedCpu==SpillBacking::stateCharge()+SpillBacking::residentCharge(input.size()) &&
        observedMemory.pendingCpu==0,"Private rehydration charge was not retained");
    for(auto failure:errors) if(failure) std::rethrow_exception(failure);
    for(const auto& lease:leases) { exact(lease,input); expect(lease.bytes().data()==leases[0].bytes().data(),"Concurrent reads published duplicate resident blocks"); }
    expect(allocations==2,"Concurrent readers allocated more than one rehydration");
    for(std::size_t i=0;i<workers.size();++i) workers[i]=std::jthread([&,i,job=backing] {
        try {
            for(unsigned iteration=0;iteration<8;++iteration) {
                if((iteration+i)%2) job.spill();
                else { auto lease=job.resident(); exact(lease,input); }
            }
        } catch(...) { errors[i]=std::current_exception(); }
    });
    for(auto& worker:workers) worker.join();
    for(auto failure:errors) if(failure) std::rethrow_exception(failure);
    expect(creates==1 && store->stats().entries==1 && !backing.status().busy,"Mixed concurrent reads/evictions replaced disk backing");
    for(auto& lease:leases) { exact(lease,input); lease={}; }
    backing={};
    ledger(*environment.memory,0);
    expect(store->stats().entries==0,"Concurrent owner cleanup retained disk or memory backing");
}
void waitingAndWorkerLifetime(const fs::path& root) {
    Fixture fixture(root); Environment environment;
    Blocker writing, reading;
    auto store=storeAt(fixture,[&](SpillIoStage stage,std::uint64_t) {
        if(stage==SpillIoStage::FileSync) writing.block();
        return 0;
    });
    const auto input=bytes(512);
    auto backing=SpillBacking::create(input,environment.memory,store,{},[&](SpillBackingStage stage,std::uint64_t) {
        if(stage==SpillBackingStage::PublishResident) reading.block();
    });
    auto old=backing.resident();
    writing.armed=true;
    auto writer=std::async(std::launch::async,[job=backing]{return job.spill();});
    ReleaseOnExit releaseWrite(writing);
    writing.wait();
    expect(backing.status().busy && backing.status().resident && !backing.spillHandle(),"Spill handle escaped before successful put");
    auto duringWrite=backing.resident(); exact(duringWrite,input);
    expect(duringWrite.bytes().data()==old.bytes().data(),"Read during spill replaced its resident bytes");
    releaseWrite.release(); expect(writer.get(),"Controlled spill failed");
    exact(old,input); duringWrite={}; old={};

    reading.armed=true;
    auto reader=std::async(std::launch::async,[job=backing]{return job.resident();});
    ReleaseOnExit releaseRead(reading);
    reading.wait();
    std::stop_source stop;
    std::promise<void> readStarted, spillStarted;
    auto queuedRead=std::async(std::launch::async,[&,job=backing] {
        readStarted.set_value(); rejectsSpill(SpillErrorCode::Cancelled,[&]{job.resident(stop.get_token());});
    });
    auto queuedSpill=std::async(std::launch::async,[&,job=backing] {
        spillStarted.set_value(); rejectsSpill(SpillErrorCode::Cancelled,[&]{job.spill(stop.get_token());});
    });
    readStarted.get_future().wait(); spillStarted.get_future().wait();
    const auto pendingRead=queuedRead.wait_for(std::chrono::milliseconds(20));
    const auto pendingSpill=queuedSpill.wait_for(std::chrono::milliseconds(20));
    stop.request_stop();
    const auto readyRead=queuedRead.wait_for(std::chrono::seconds(2));
    const auto readySpill=queuedSpill.wait_for(std::chrono::seconds(2));
    releaseRead.release();
    auto completed=reader.get(); queuedRead.get(); queuedSpill.get(); exact(completed,input);
    expect(pendingRead==std::future_status::timeout && pendingSpill==std::future_status::timeout &&
        readyRead==std::future_status::ready && readySpill==std::future_status::ready,
        "Queued backing operations did not cancel independently of active rehydration");
    completed={}; backing.spill();

    auto observer=*environment.memory;
    const auto session=store->sessionDirectory();
    reading.armed=true;
    auto surviving=std::async(std::launch::async,[job=backing]() mutable {
        auto lease=job.resident(); job={}; return lease;
    });
    ReleaseOnExit releaseSurviving(reading);
    reading.wait();
    backing={}; environment.memory.reset(); store.reset();
    const auto pinned=fs::exists(session);
    releaseSurviving.release();
    auto retained=surviving.get(); exact(retained,input);
    expect(pinned && !fs::exists(session),"Active job lifetime prematurely cleaned disk or retained it after ownership ended");
    ledger(observer,retained.chargedBytes()); retained={}; ledger(observer,0);
}
}
int main(int argc, char** argv) {
    try {
        if(argc!=2) throw std::runtime_error("Usage: spill_backing_test /absolute/disk-backed/fixture-directory");
        const fs::path root=argv[1];
        expect(root.is_absolute() && fs::is_directory(root),"Supply an existing absolute fixture directory");
        struct statfs filesystem{};
        expect(::statfs(root.c_str(),&filesystem)==0 && filesystem.f_type!=TMPFS_MAGIC && filesystem.f_type!=RAMFS_MAGIC,
            "Backing fixtures require an existing disk-backed directory");
        apiAndRoundTrips(root); std::cout<<"PASS exact opaque bytes, API and admitted read-only leases\n";
        ownerLifetimes(root); std::cout<<"PASS lease/backing/disk lifetimes beyond original owners\n";
        memoryRefusal(root); std::cout<<"PASS memory refusal, pinned charges and recovery\n";
        allocationFailures(root); std::cout<<"PASS allocation/publication failure rollback\n";
        cancellation(root); std::cout<<"PASS cancellation during IO and before publication\n";
        diskFailuresAndCorruption(root); std::cout<<"PASS write/disk failures and corrupt read preservation\n";
        concurrentAccess(root); std::cout<<"PASS concurrent reads/spills/rehydration and single publication\n";
        waitingAndWorkerLifetime(root); std::cout<<"PASS queued cancellation, reads during spill and worker ownership\n";
        std::cout<<"All 8 spill backing test groups passed (fixtures <= 8193 bytes per payload).\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
