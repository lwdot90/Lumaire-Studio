#include "io/spill_store.h"
#include "core/memory_admission.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <fcntl.h>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <mutex>
#include <semaphore>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <linux/magic.h>
#include <thread>
#include <unistd.h>

using namespace compositor::io;
namespace fs=std::filesystem;
namespace {
void expect(bool condition, const char* message) { if(!condition) throw std::runtime_error(message); }
template<class F> void rejects(SpillErrorCode code, F&& action, int nativeError=0) {
    try { action(); }
    catch(const SpillError& error) {
        expect(error.code()==code,"Unexpected spill error category");
        if(nativeError) expect(error.systemError().value()==nativeError,"Underlying errno lost");
        return;
    }
    throw std::runtime_error("Expected spill failure");
}
struct Fixture {
    fs::path path;
    explicit Fixture(const fs::path& parent) {
        auto name=(parent/"spill-test-XXXXXX").string();
        const auto result=::mkdtemp(name.data());
        expect(result!=nullptr,"Create small owned disk fixture");
        path=result;
    }
    ~Fixture() {
        // Only this mkdtemp-created test tree, never the supplied build parent.
        std::error_code error;
        fs::remove_all(path,error);
    }
};
SpillLimits limits() { return {2*1024*1024,65536,0,128,2,128}; }
std::vector<std::uint8_t> bytes(std::size_t size, unsigned seed=0) {
    std::vector<std::uint8_t> result(size);
    for(std::size_t i=0;i<size;++i) result[i]=static_cast<std::uint8_t>((i*73+seed*29+(i>>8))&255);
    return result;
}
std::vector<fs::path> files(const fs::path& directory) {
    std::vector<fs::path> result;
    for(const auto& entry:fs::directory_iterator(directory)) result.push_back(entry.path());
    return result;
}
void changeByte(const fs::path& path, off_t offset) {
    const auto fd=::open(path.c_str(),O_RDWR|O_CLOEXEC);
    expect(fd>=0,"Open owned corruption fixture");
    std::uint8_t value=0;
    const auto read=::pread(fd,&value,1,offset);
    value^=0x80;
    const auto written=::pwrite(fd,&value,1,offset);
    const auto closed=::close(fd);
    expect(read==1 && written==1 && closed==0,"Mutate one fixture byte");
}
void roundTrips(const fs::path& root) {
    Fixture fixture(root);
    SpillStore store(fixture.path,limits());
    struct stat permissions{};
    expect(::stat(store.sessionDirectory().c_str(),&permissions)==0 && (permissions.st_mode&077)==0,
           "Session permissions must be private");
    const std::array<std::size_t,13> sizes{0,1,7,31,32,33,127,128,129,4095,4096,8193,65536};
    for(auto size:sizes) {
        auto input=bytes(size);
        const auto expected=input;
        auto handle=store.put(input);
        expect(bool(handle) && handle.size()==size,"Opaque payload length retained");
        std::fill(input.begin(),input.end(),0); // Input is not borrowed after put.
        expect(handle.read()==expected,"Exact byte round-trip");
        std::vector<std::uint8_t> destination(size);
        handle.readInto(destination);
        expect(destination==expected,"Caller-buffer round-trip");
        const auto paths=files(store.sessionDirectory());
        expect(paths.size()==1,"One file per immutable payload");
        expect(::stat(paths[0].c_str(),&permissions)==0 && (permissions.st_mode&077)==0,"Payload permissions must be private");
        auto copy=handle;
        const auto charge=store.stats().bytes;
        handle={};
        expect(copy.read()==expected && store.stats().bytes==charge,"Copy pins one shared allocation");
        store.collectGarbage();
        expect(copy.read()==expected,"Garbage collection preserves live handles");
        copy={};
        expect(store.stats().bytes==0 && store.stats().entries==0 && files(store.sessionDirectory()).empty(),
               "Last release removes payload and frees charge");
    }
    const std::vector<std::uint8_t> known{'1','2','3','4','5','6','7','8','9'};
    auto handle=store.put(known);
    std::ifstream file(files(store.sessionDirectory()).at(0),std::ios::binary);
    std::array<unsigned char,32> encoded{};
    file.read(reinterpret_cast<char*>(encoded.data()),encoded.size());
    expect(bool(file),"Read checksum fixture header");
    std::uint64_t crc=0;
    for(unsigned i=0;i<8;++i) crc|=std::uint64_t(encoded[16+i])<<(i*8);
    expect(crc==UINT64_C(0x6c40df5f0b497347),"CRC-64/ECMA known vector");
    SpillHandle empty;
    expect(!empty && empty.size()==0,"Default handle is empty");
    rejects(SpillErrorCode::InvalidArgument,[&]{empty.read();});
    std::vector<std::uint8_t> wrong(known.size()+1);
    rejects(SpillErrorCode::InvalidArgument,[&]{handle.readInto(wrong);});
    auto configured=limits(); configured.chunkBytes=1;
    const auto tinyInput=bytes(17);
    SpillStore tinyChunks(fixture.path,configured,[&](SpillIoStage stage,std::uint64_t progress) {
        if(stage==SpillIoStage::Write || stage==SpillIoStage::Read)
            expect(progress<=tinyInput.size(),"Fault progress excludes header-byte transfers");
        return 0;
    });
    auto tiny=tinyChunks.put(tinyInput);
    expect(tiny.read()==tinyInput,"One-byte transfer chunks preserve exact payloads");
}
void lifetimes(const fs::path& root) {
    Fixture fixture(root);
    std::ofstream(fixture.path/"unrelated")<<"keep";
    SpillHandle retained;
    fs::path session;
    const auto expected=bytes(1024,3);
    {
        SpillStore store(fixture.path,limits());
        session=store.sessionDirectory();
        retained=store.put(expected);
        SpillStore other(fixture.path,limits());
        auto otherHandle=other.put(bytes(1));
        expect(other.sessionDirectory()!=session,"Stores have isolated sessions");
        retained.read();
    }
    expect(fs::exists(session) && retained.read()==expected,"Handles outlive store destruction");
    auto copy=retained;
    retained={};
    expect(copy.read()==expected,"Last copied handle owns backing");
    copy={};
    expect(!fs::exists(session) && fs::exists(fixture.path/"unrelated"),"Final cleanup removes only owned session");
    {
        SpillStore store(fixture.path,limits());
        session=store.sessionDirectory();
        std::ofstream(session/"unknown")<<"preserve";
        auto handle=store.put(bytes(32));
    }
    expect(fs::exists(session/"unknown") && files(session).size()==1,"Final cleanup leaves unknown session contents");
    std::binary_semaphore reading(0), release(0);
    std::atomic<bool> block=true;
    auto store=std::make_unique<SpillStore>(fixture.path,limits(),[&](SpillIoStage stage,std::uint64_t) {
        if(stage==SpillIoStage::Read && block.exchange(false)) { reading.release(); release.acquire(); }
        return 0;
    });
    auto handle=store->put(expected);
    const auto readingSession=store->sessionDirectory();
    auto reader=std::async(std::launch::async,[job=handle]() mutable {
        auto result=job.read(); job={}; return result;
    });
    reading.acquire();
    handle={}; store.reset();
    const auto pinned=fs::exists(readingSession);
    release.release();
    expect(reader.get()==expected && pinned && !fs::exists(readingSession),"In-flight reader survives store and original handle release");
    Fixture movable(root);
    {
        SpillStore moved(movable.path,limits());
        const auto sessionName=moved.sessionDirectory().filename();
        auto original=moved.put(expected);
        const auto renamed=fs::path(movable.path.string()+"-renamed");
        fs::rename(movable.path,renamed); movable.path=renamed;
        auto subsequent=moved.put(bytes(32));
        expect(original.read()==expected && subsequent.read()==bytes(32),"Descriptor-relative IO survives parent rename");
        original={}; subsequent={};
        expect(files(movable.path/sessionName).empty(),"Descriptor-relative retirement uses the original parent");
    }
    expect(files(movable.path).empty(),"Descriptor-relative final cleanup survives parent rename");
}
void capacity(const fs::path& root) {
    Fixture fixture(root);
    struct statvfs space{};
    expect(::statvfs(fixture.path.c_str(),&space)==0,"Inspect test allocation unit");
    auto configured=limits();
    configured.maxBytes=static_cast<std::uint64_t>(space.f_frsize)*2;
    configured.maxPayloadBytes=1024;
    SpillStore store(fixture.path,configured);
    auto one=store.put({});
    auto two=store.put(bytes(1));
    expect(store.stats().bytes==configured.maxBytes,"Block-rounded capacity accounted");
    rejects(SpillErrorCode::Capacity,[&]{store.put(bytes(1));});
    expect(one.read().empty() && two.read()==bytes(1),"Capacity failure preserves live payloads");
    auto shared=one;
    one={};
    rejects(SpillErrorCode::Capacity,[&]{store.put({});});
    shared={};
    auto three=store.put(bytes(1));
    expect(store.stats().bytes==configured.maxBytes,"Capacity reclaimed after final release");
    rejects(SpillErrorCode::Capacity,[&]{store.put(bytes(1025));});
    configured=limits(); configured.maxEntries=1;
    SpillStore entries(fixture.path,configured);
    auto only=entries.put({});
    rejects(SpillErrorCode::Capacity,[&]{entries.put({});});
    expect(entries.stats().entries==1,"Empty payload cannot bypass entry bound");
    configured.minFreeBytes=std::numeric_limits<std::uint64_t>::max();
    SpillStore reserve(fixture.path,configured);
    rejects(SpillErrorCode::DiskSpace,[&]{reserve.put(bytes(1));},ENOSPC);
    expect(reserve.stats().entries==0,"Disk preflight rejects before reservation");
    std::binary_semaphore reserved(0), release(0);
    configured=limits(); configured.maxBytes=space.f_frsize; configured.maxIoOperations=2;
    SpillStore pending(fixture.path,configured,[&](SpillIoStage stage,std::uint64_t) {
        if(stage==SpillIoStage::CreateFile) { reserved.release(); release.acquire(); }
        return 0;
    });
    auto writer=std::async(std::launch::async,[&]{return pending.put(bytes(32));});
    reserved.acquire();
    bool rejected=false;
    try { pending.put(bytes(32)); }
    catch(const SpillError& error) { rejected=error.code()==SpillErrorCode::Capacity; }
    release.release();
    auto published=writer.get();
    expect(rejected && published.read()==bytes(32) && pending.stats().entries==1,
           "Concurrent writes cannot overcommit a pending reservation");
}
void corruptions(const fs::path& root) {
    Fixture fixture(root);
    SpillStore store(fixture.path,limits());
    const auto input=bytes(512);
    for(off_t offset:{0,8,16,24,32,511}) {
        auto handle=store.put(input);
        changeByte(files(store.sessionDirectory()).at(0),offset);
        rejects(SpillErrorCode::Corrupt,[&]{handle.read();});
    }
    for(bool append:{false,true}) {
        auto handle=store.put(input);
        const auto path=files(store.sessionDirectory()).at(0);
        if(append) { std::ofstream output(path,std::ios::binary|std::ios::app); output.put('\0'); }
        else expect(::truncate(path.c_str(),33)==0,"Truncate owned payload");
        rejects(SpillErrorCode::Corrupt,[&]{handle.read();});
    }
    {
        auto handle=store.put({});
        changeByte(files(store.sessionDirectory()).at(0),24);
        rejects(SpillErrorCode::Corrupt,[&]{handle.read();});
    }
    const auto outside=fixture.path/"outside";
    std::ofstream(outside)<<"keep";
    for(bool symlink:{false,true}) {
        auto handle=store.put(input);
        const auto path=files(store.sessionDirectory()).at(0);
        const auto original=fixture.path/"original";
        fs::rename(path,original);
        if(symlink) fs::create_symlink(outside,path);
        else fs::copy_file(original,path);
        rejects(SpillErrorCode::Corrupt,[&]{handle.read();});
        handle={};
        expect(fs::exists(path) && fs::exists(outside) && store.stats().cleanupBlocked,"Cleanup preserves replacements and symlink target");
        rejects(SpillErrorCode::CleanupFailed,[&]{store.collectGarbage();});
        fs::remove(path); fs::rename(original,path);
        store.collectGarbage();
        expect(store.stats().bytes==0 && !store.stats().cleanupBlocked,"Cleanup retry reclaims restored owned file");
    }
    {
        auto handle=store.put(input);
        const auto path=files(store.sessionDirectory()).at(0);
        const auto link=fixture.path/"hardlink";
        fs::create_hard_link(path,link);
        rejects(SpillErrorCode::Corrupt,[&]{handle.read();});
        fs::remove(link);
        expect(handle.read()==input,"Restored identity remains readable");
    }
    for(bool truncate:{false,true}) {
        fs::path path;
        bool armed=false;
        SpillStore changing(fixture.path,limits(),[&](SpillIoStage stage,std::uint64_t progress) {
            if(armed && stage==SpillIoStage::Read && progress>=128) {
                armed=false;
                if(truncate) expect(::truncate(path.c_str(),33)==0,"Truncate during owned read");
                else {
                    // Change a byte already consumed: checksum alone would
                    // match the original returned buffer; final stat must fail.
                    changeByte(path,32);
                    fs::last_write_time(path,fs::last_write_time(path)+std::chrono::seconds(1));
                }
            }
            return 0;
        });
        auto handle=changing.put(input);
        path=files(changing.sessionDirectory()).at(0); armed=true;
        rejects(SpillErrorCode::Corrupt,[&]{handle.read();});
        expect(changing.stats().activeIo==0,"Mid-read corruption releases the IO lease");
    }
}
void concurrency(const fs::path& root) {
    Fixture fixture(root);
    SpillStore store(fixture.path,limits());
    std::array<std::vector<SpillHandle>,4> retained;
    std::array<std::exception_ptr,4> errors{};
    std::array<std::jthread,4> workers;
    auto shared=store.put(bytes(8193,7));
    for(std::size_t thread=0;thread<workers.size();++thread) workers[thread]=std::jthread([&,thread] {
        try {
            for(unsigned iteration=0;iteration<8;++iteration) {
                const auto input=bytes(513+iteration,static_cast<unsigned>(thread));
                auto handle=store.put(input);
                auto copy=handle;
                expect(copy.read()==input,"Concurrent put/read exact bytes");
                expect(shared.read()==bytes(8193,7),"Concurrent shared-handle reads");
                retained[thread].push_back(std::move(handle));
            }
        } catch(...) { errors[thread]=std::current_exception(); }
    });
    for(auto& worker:workers) worker.join();
    for(auto error:errors) if(error) std::rethrow_exception(error);
    expect(store.stats().entries==33 && store.stats().activeIo==0,"Concurrent reservations and IO leases balance");
    for(std::size_t thread=0;thread<retained.size();++thread)
        for(std::size_t iteration=0;iteration<retained[thread].size();++iteration)
            expect(retained[thread][iteration].read()==bytes(513+iteration,static_cast<unsigned>(thread)),"Concurrent retained handle stays exact");
    for(auto& handles:retained) handles.clear();
    shared={};
    expect(store.stats().bytes==0 && store.stats().entries==0,"Concurrent lifetimes release all charges");
}
void cancellation(const fs::path& root) {
    Fixture fixture(root);
    std::stop_source source;
    std::atomic<bool> armed=false;
    auto target=SpillIoStage::Write;
    SpillStore store(fixture.path,limits(),[&](SpillIoStage stage,std::uint64_t progress) {
        if(armed && stage==target && (stage==SpillIoStage::DirectorySync || progress>=128)) source.request_stop();
        return 0;
    });
    const auto input=bytes(512);
    auto good=store.put(input);
    const auto charge=store.stats().bytes;
    source.request_stop();
    rejects(SpillErrorCode::Cancelled,[&]{store.put(input,source.get_token());});
    rejects(SpillErrorCode::Cancelled,[&]{good.read(source.get_token());});
    rejects(SpillErrorCode::Cancelled,[&]{store.collectGarbage(source.get_token());});
    for(auto stage:{SpillIoStage::Write,SpillIoStage::DirectorySync}) {
        source=std::stop_source{}; target=stage; armed=true;
        rejects(SpillErrorCode::Cancelled,[&]{store.put(input,source.get_token());});
        armed=false;
        expect(store.stats().bytes==charge && store.stats().entries==1,"Cancelled write removes partial file and reservation");
    }
    source=std::stop_source{}; target=SpillIoStage::Read; armed=true;
    std::vector<std::uint8_t> destination(input.size());
    rejects(SpillErrorCode::Cancelled,[&]{good.readInto(destination,source.get_token());});
    armed=false;
    expect(good.read()==input && store.stats().activeIo==0,"Cancelled read preserves immutable backing and releases IO lease");

    std::binary_semaphore entered(0), release(0);
    std::atomic<bool> block=false;
    auto configured=limits(); configured.maxIoOperations=1;
    SpillStore gated(fixture.path,configured,[&](SpillIoStage stage,std::uint64_t) {
        if(stage==SpillIoStage::CreateFile && block.exchange(false)) { entered.release(); release.acquire(); }
        return 0;
    });
    auto existing=gated.put(input);
    block=true;
    auto writer=std::async(std::launch::async,[&]{return gated.put(input);});
    entered.acquire();
    expect(gated.stats().activeIo==1,"IO gate bounds active calls");
    std::stop_source queuedStop;
    std::promise<void> queuedStarted, readStarted;
    auto queued=std::async(std::launch::async,[&] {
        queuedStarted.set_value();
        rejects(SpillErrorCode::Cancelled,[&]{gated.put(input,queuedStop.get_token());});
    });
    auto queuedRead=std::async(std::launch::async,[&] {
        readStarted.set_value();
        rejects(SpillErrorCode::Cancelled,[&]{existing.read(queuedStop.get_token());});
    });
    queuedStarted.get_future().wait(); readStarted.get_future().wait();
    const auto waitingWrite=queued.wait_for(std::chrono::milliseconds(20));
    const auto waitingRead=queuedRead.wait_for(std::chrono::milliseconds(20));
    queuedStop.request_stop();
    const auto ready=queued.wait_for(std::chrono::seconds(2));
    const auto readReady=queuedRead.wait_for(std::chrono::seconds(2));
    release.release(); // Always unblock the owned fixture before assertions.
    auto completed=writer.get();
    queued.get();
    queuedRead.get();
    expect(waitingWrite==std::future_status::timeout && waitingRead==std::future_status::timeout &&
           ready==std::future_status::ready && readReady==std::future_status::ready && completed.read()==input,
           "Queued put/read cancellation wakes without waiting for ongoing IO");
    expect(gated.stats().entries==2 && gated.stats().activeIo==0,"Cancelled queued calls do not reserve files");
}
void failures(const fs::path& root) {
    Fixture fixture(root);
    bool armed=false;
    auto target=SpillIoStage::Write;
    int injected=EIO;
    std::uint64_t threshold=0;
    SpillStore store(fixture.path,limits(),[&](SpillIoStage stage,std::uint64_t progress) {
        return armed && stage==target && progress>=threshold ? injected : 0;
    });
    const auto input=bytes(512);
    auto good=store.put(input);
    const auto charge=store.stats().bytes;
    for(int error:{EIO,ENOSPC,EDQUOT}) {
        injected=error;
        const auto code=error==EIO ? SpillErrorCode::Io : SpillErrorCode::DiskSpace;
        for(auto stage:{SpillIoStage::Preflight,SpillIoStage::CreateFile,SpillIoStage::Write,
                       SpillIoStage::FileSync,SpillIoStage::FileClose,SpillIoStage::DirectorySync}) {
            target=stage; threshold=stage==SpillIoStage::Write ? 128 : 0; armed=true;
            rejects(code,[&]{store.put(input);},error);
            armed=false;
            expect(store.stats().bytes==charge && store.stats().entries==1 && files(store.sessionDirectory()).size()==1,
                   "Failed write removes partial output without altering prior handles");
            expect(good.read()==input,"Prior payload survives write/sync/close/disk failure");
        }
    }
    target=SpillIoStage::Read; injected=EIO; threshold=128; armed=true;
    rejects(SpillErrorCode::Io,[&]{good.read();},EIO);
    armed=false;
    expect(good.read()==input,"Read failure is retryable without backing changes");
    auto retired=store.put(input);
    target=SpillIoStage::RemoveFile; injected=EACCES; threshold=0; armed=true;
    const auto beforeRelease=store.stats().bytes;
    retired={};
    expect(store.stats().bytes==beforeRelease && store.stats().cleanupBlocked && store.stats().cleanupFailures==1,
           "Failed cleanup remains charged and observable");
    rejects(SpillErrorCode::CleanupFailed,[&]{store.put(input);});
    rejects(SpillErrorCode::CleanupFailed,[&]{store.collectGarbage();});
    expect(good.read()==input,"Cleanup failure does not revoke live payloads");
    armed=false;
    store.collectGarbage();
    expect(store.stats().bytes==charge && !store.stats().cleanupBlocked && store.stats().entries==1,"Cleanup retry frees only retired payload");
    // A thrown test callback is also contained by nonthrowing retirement.
    SpillStore throwingStore(fixture.path,limits(),[](SpillIoStage stage,std::uint64_t) -> int {
        if(stage==SpillIoStage::RemoveFile) throw std::runtime_error("Injected cleanup exception");
        return 0;
    });
    auto handle=throwingStore.put(input);
    handle={};
    expect(throwingStore.stats().cleanupBlocked,"Destructor contains cleanup callback exception");
    bool failBoth=false;
    SpillStore partial(fixture.path,limits(),[&](SpillIoStage stage,std::uint64_t progress) {
        if(failBoth && stage==SpillIoStage::Write && progress>=128) return ENOSPC;
        if(failBoth && stage==SpillIoStage::RemoveFile) return EACCES;
        return 0;
    });
    auto prior=partial.put(input);
    const auto priorCharge=partial.stats().bytes;
    failBoth=true;
    rejects(SpillErrorCode::DiskSpace,[&]{partial.put(input);},ENOSPC);
    expect(partial.stats().bytes==priorCharge*2 && partial.stats().entries==2 && partial.stats().cleanupBlocked,
           "Failed partial-file cleanup keeps its pending reservation charged");
    expect(prior.read()==input,"Combined disk and cleanup failure preserves prior backing");
    failBoth=false;
    partial.collectGarbage();
    expect(partial.stats().bytes==priorCharge && partial.stats().entries==1,"Retry frees failed write reservation");
    auto retry=partial.put(input);
    expect(retry.read()==input,"Writing resumes after failed partial-file cleanup succeeds");
}
void metadataAdmission(const fs::path& root) {
    Fixture fixture(root);
    constexpr std::uint64_t budget=8*1024*1024;
    auto memory=std::make_shared<compositor::MemoryAdmission>(budget,0,[] {
        return compositor::MemorySample{budget*2,0};
    });
    SpillHandle retained;
    std::uint64_t charge=0;
    {
        SpillStore store(fixture.path,limits());
        store.admitMetadata(memory);
        const auto admitted=memory->snapshot();
        charge=admitted.committedCpu;
        expect(charge>0 && admitted.pendingCpu==0 && admitted.pendingGpu==0 && admitted.committedGpu==0,
            "Spill metadata did not retain a committed CPU reservation");
        store.admitMetadata(memory);
        expect(memory->snapshot().committedCpu==charge,"Repeated metadata admission charged twice");
        auto other=std::make_shared<compositor::MemoryAdmission>(budget,0,[] {
            return compositor::MemorySample{budget*2,0};
        });
        rejects(SpillErrorCode::InvalidArgument,[&]{store.admitMetadata(other);});
        expect(other->snapshot().committedCpu==0,"Rejected admission owner acquired a charge");
        retained=store.put(bytes(17));
        expect(memory->snapshot().committedCpu==charge,"Payload entries escaped the prepaid metadata bound");
    }
    expect(memory->snapshot().committedCpu==charge && retained.read()==bytes(17),
        "Final handle did not retain metadata admission after store destruction");
    retained={};
    expect(memory->snapshot().committedCpu==0,"Final handle retirement leaked metadata charge");

    SpillStore deniedStore(fixture.path,limits());
    bool reclaimed=false;
    auto deniedMemory=std::make_shared<compositor::MemoryAdmission>(1,0,[] {
        return compositor::MemorySample{budget*2,0};
    });
    deniedMemory->setReclaimer([&](std::uint64_t){reclaimed=true;});
    bool denied=false;
    try {deniedStore.admitMetadata(deniedMemory);}
    catch(const std::length_error&) {denied=true;}
    expect(denied && !reclaimed && deniedMemory->snapshot().committedCpu==0 &&
        deniedMemory->snapshot().pendingCpu==0 && deniedStore.stats().entries==0,
        "Metadata denial changed accounting or attempted recursive reclamation");
    deniedStore.admitMetadata(memory);
    auto admittedAfterFailure=deniedStore.put(bytes(5));
    expect(admittedAfterFailure.read()==bytes(5),"Failed metadata admission prevented a valid retry");
    SpillStore late(fixture.path,limits());
    auto existing=late.put(bytes(3));
    rejects(SpillErrorCode::InvalidArgument,[&]{late.admitMetadata(memory);});
    expect(existing.read()==bytes(3),"Late metadata rejection damaged existing backing");

    bool failCleanup=true;
    auto cleanupMemory=std::make_shared<compositor::MemoryAdmission>(budget,0,[] {
        return compositor::MemorySample{budget*2,0};
    });
    {
        SpillStore cleanup(fixture.path,limits(),[&](SpillIoStage stage,std::uint64_t) {
            return stage==SpillIoStage::RemoveFile && failCleanup ? EACCES : 0;
        });
        cleanup.admitMetadata(cleanupMemory);
        const auto cleanupCharge=cleanupMemory->snapshot().committedCpu;
        auto handle=cleanup.put(bytes(9));
        handle={};
        expect(cleanup.stats().cleanupBlocked && cleanupMemory->snapshot().committedCpu==cleanupCharge,
            "Failed cleanup discarded metadata admission for retained records");
        failCleanup=false;
        cleanup.collectGarbage();
        expect(cleanupMemory->snapshot().committedCpu==cleanupCharge,
            "Garbage collection discarded live store metadata admission");
    }
    expect(cleanupMemory->snapshot().committedCpu==0,"Cleanup state retirement leaked its metadata charge");
    auto huge=limits();
    huge.maxEntries=std::numeric_limits<std::size_t>::max();
    SpillStore overflow(fixture.path,huge);
    const auto before=memory->snapshot();
    rejects(SpillErrorCode::Capacity,[&]{overflow.admitMetadata(memory);});
    expect(memory->snapshot().committedCpu==before.committedCpu &&
        memory->snapshot().pendingCpu==before.pendingCpu,"Metadata bound overflow changed the ledger");
}
void directories(const fs::path& root) {
    Fixture fixture(root);
    auto configured=limits(); configured.maxBytes=0;
    rejects(SpillErrorCode::InvalidArgument,[&]{SpillStore bad(fixture.path,configured);});
    configured=limits(); configured.chunkBytes=0;
    rejects(SpillErrorCode::InvalidArgument,[&]{SpillStore bad(fixture.path,configured);});
    configured=limits(); configured.maxPayloadBytes=std::numeric_limits<std::uint64_t>::max();
    rejects(SpillErrorCode::InvalidArgument,[&]{SpillStore bad(fixture.path,configured);});
    rejects(SpillErrorCode::InvalidArgument,[&]{SpillStore bad("relative",limits());});
    rejects(SpillErrorCode::InvalidArgument,[&]{SpillStore bad(fixture.path/"..",limits());});
    rejects(SpillErrorCode::Io,[&]{SpillStore bad(fixture.path/"missing",limits());});
    const auto link=fixture.path/"directory-link";
    fs::create_directory_symlink(fixture.path,link);
    rejects(SpillErrorCode::Io,[&]{SpillStore bad(link,limits());});
    rejects(SpillErrorCode::Io,[&]{SpillStore bad(link/"subdirectory",limits());});
    const auto nul=fixture.path.string()+std::string("\0hidden",7);
    rejects(SpillErrorCode::InvalidArgument,[&]{SpillStore bad(fs::path(nul),limits());});
    bool testedTmpfs=false;
    for(const auto& candidate:{fs::path("/tmp"),fs::path("/dev/shm")}) {
        struct statfs info{};
        if(::statfs(candidate.c_str(),&info)==0 && (info.f_type==TMPFS_MAGIC || info.f_type==RAMFS_MAGIC)) {
            rejects(SpillErrorCode::UnsupportedFilesystem,[&]{SpillStore bad(candidate,limits());});
            testedTmpfs=true;
        }
    }
    expect(testedTmpfs,"No existing tmpfs/ramfs available for rejection test (do not mount one)");
    expect(files(fixture.path).size()==1,"Rejected directories leave no session artifacts");
}
}
int main(int argc, char** argv) {
    try {
        if(argc!=2) throw std::runtime_error("Usage: spill_store_test /absolute/disk-backed/build-directory");
        const fs::path root=argv[1];
        expect(root.is_absolute() && fs::is_directory(root),"Supply an existing absolute disk-backed fixture directory");
        roundTrips(root); std::cout<<"PASS exact byte round-trips and CRC known vector\n";
        lifetimes(root); std::cout<<"PASS handle lifetimes and isolated cleanup\n";
        capacity(root); std::cout<<"PASS byte/entry bounds and disk reserve\n";
        corruptions(root); std::cout<<"PASS corruption, truncation and replacement rejection\n";
        concurrency(root); std::cout<<"PASS concurrent put/read and shared handle lifetimes\n";
        cancellation(root); std::cout<<"PASS cancellation during IO, before publication and while queued\n";
        failures(root); std::cout<<"PASS simulated write/sync/close/read/disk and cleanup failures\n";
        metadataAdmission(root); std::cout<<"PASS metadata admission, retained handles, denial and cleanup accounting\n";
        directories(root); std::cout<<"PASS injected directory validation and tmpfs rejection\n";
        std::cout<<"All 9 spill store test groups passed (fixtures <= 64 KiB per payload).\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
