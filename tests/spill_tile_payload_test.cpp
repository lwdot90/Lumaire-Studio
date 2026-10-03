#include "core/spill_tile_payload.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

using namespace compositor;
using namespace compositor::engine;
using namespace compositor::io;
namespace fs=std::filesystem;
namespace {
constexpr std::uint64_t abundant=2*1024*1024;
void expect(bool okay,const char* message) {if(!okay) throw std::runtime_error(message);}
template<class Error,class F> void rejects(F action) {
    try {action();} catch(const Error&) {return;}
    throw std::runtime_error("Expected typed tile failure");
}
template<class F> void rejectsSpill(SpillErrorCode code,F action) {
    try {action();} catch(const SpillError& error) {expect(error.code()==code,"Wrong spill failure category");return;}
    throw std::runtime_error("Expected spill tile IO failure");
}
void ledger(const MemoryAdmission& memory,std::uint64_t committed=0) {
    const auto sample=memory.snapshot();
    expect(sample.committedCpu==committed && !sample.pendingCpu && !sample.committedGpu && !sample.pendingGpu,
        "Typed tile memory charges do not match retained metadata/resident blocks");
}
struct Fixture {
    fs::path path;
    explicit Fixture(const fs::path& parent) {
        auto pattern=(parent/"spill-tile-test-XXXXXX").string();
        const auto created=::mkdtemp(pattern.data()); expect(created!=nullptr,"Create owned disk tile fixture"); path=created;
    }
    ~Fixture() {std::error_code error;fs::remove_all(path,error);}
};
struct Environment {
    std::shared_ptr<std::atomic<std::uint64_t>> available=std::make_shared<std::atomic<std::uint64_t>>(abundant);
    std::shared_ptr<MemoryAdmission> memory=std::make_shared<MemoryAdmission>(abundant,0,
        [sample=available] {return MemorySample{sample->load(),0};});
};
std::shared_ptr<SpillStore> storeAt(const Fixture& fixture,SpillFault fault={}) {
    return std::make_shared<SpillStore>(fixture.path,SpillLimits{2*1024*1024,65536,0,128,2,32},std::move(fault));
}
void append(std::vector<std::uint8_t>& bytes,PackedPixel pixel) {
    for(const auto channel:pixel) {bytes.push_back(static_cast<std::uint8_t>(channel&255));bytes.push_back(static_cast<std::uint8_t>(channel>>8));}
}
const std::array<PackedPixel,6> samples{
    PackedPixel{},PackedPixel{0x0001,0x3800,0xb800,0x3c00},PackedPixel{0x7bff,0x0400,0x4000,0x3800},
    PackedPixel{0x3c00,0x3400,0x3000,0x3c00},PackedPixel{0xbc00,0x3555,0x7bff,0x0001},PackedPixel{0x0002,0,0x3c00,0x3bff}
};
std::vector<std::uint8_t> denseBytes(int width,int height) {
    std::vector<std::uint8_t> bytes;bytes.reserve(static_cast<std::size_t>(width)*static_cast<std::size_t>(height)*8);
    for(int y=0;y<height;++y) for(int x=0;x<width;++x) append(bytes,samples[static_cast<std::size_t>(x+3*y)%samples.size()]);
    return bytes;
}
void exact(std::span<const std::uint8_t> actual,std::span<const std::uint8_t> expected) {
    expect(std::equal(actual.begin(),actual.end(),expected.begin(),expected.end()),"Canonical stored bytes changed");
}
std::uint64_t metadata() {return SpillTilePayload::metadataCharge()+SpillBacking::stateCharge();}
std::uint64_t resident(std::size_t size) {return SpillBacking::residentCharge(size);}
fs::path onlyFile(const SpillStore& store) {
    fs::path path;std::size_t count=0;
    for(const auto& entry:fs::directory_iterator(store.sessionDirectory())) {path=entry.path();++count;}
    expect(count==1,"Expected exactly one spill tile file");return path;
}
void flip(const fs::path& path,std::streamoff offset) {
    std::fstream file(path,std::ios::in|std::ios::out|std::ios::binary);expect(bool(file),"Open owned corrupt tile fixture");
    file.seekg(offset);char byte=0;file.read(&byte,1);expect(bool(file),"Read corrupt fixture byte");
    byte=static_cast<char>(static_cast<unsigned char>(byte)^0x80u);
    file.seekp(offset);file.write(&byte,1);file.flush();expect(bool(file),"Write corrupt fixture byte");
}
void denseAndConstant(const fs::path& root) {
    static_assert(std::is_same_v<decltype(std::declval<const SpillTileReadLease&>().storedBytes()),std::span<const std::uint8_t>>);
    static_assert(std::is_copy_constructible_v<SpillTilePayload> && std::is_copy_constructible_v<SpillTileReadLease>);
    expect(SpillTilePayload::encoding=="rgba16f-le","Legacy working encoding identifier changed");
    Fixture fixture(root);Environment environment;auto store=storeAt(fixture);
    for(const auto& dimensions:{std::pair{7,3},std::pair{1,256},std::pair{256,1}}) {
        const auto [width,height]=dimensions;const auto bytes=denseBytes(width,height);
        auto payload=SpillTilePayload::create(width,height,SpillTileKind::Dense,bytes,environment.memory,store);
        auto copy=payload;expect(payload.sameIdentity(copy),"Copy did not share immutable typed identity");
        const auto lease=payload.resident();expect(lease && lease.width()==width && lease.height()==height && lease.kind()==SpillTileKind::Dense,"Dense lease metadata");
        exact(lease.storedBytes(),bytes);ledger(*environment.memory,metadata()+resident(bytes.size()));
        // Placement belongs to the caller; local pixels are unchanged at a
        // negative world origin and no TileCoord enters this payload API.
        constexpr int originX=-513,originY=-2;
        for(int y=0;y<height;++y) for(int x=0;x<width;++x) {
            const int worldX=originX+x,worldY=originY+y;
            const auto expected=samples[static_cast<std::size_t>(x+3*y)%samples.size()];
            expect(lease.pixel(worldX-originX,worldY-originY)==expected && lease.linearPixel(x,y)==unpack(expected),"Partial/local pixel or exact linear unpack changed");
        }
        std::vector<std::uint8_t> full(bytes.size(),0xff);lease.copyCanonicalBytes(full);exact(full,bytes);
        rejects<std::out_of_range>([&]{lease.pixel(-1,0);});rejects<std::out_of_range>([&]{lease.pixel(width,0);});
        rejects<std::out_of_range>([&]{lease.linearPixel(0,height);});
        expect(payload.spill() && payload.kind()==SpillTileKind::Dense && !payload.status().resident,"Spill changed dense classification");
        expect(payload.sameIdentity(copy) && !copy.spill(),"Spill changed identity or repeated disk-only eviction");
        const auto hydrated=copy.resident();exact(hydrated.storedBytes(),bytes);exact(lease.storedBytes(),bytes);
        ledger(*environment.memory,metadata()+2*resident(bytes.size()));
    }
    ledger(*environment.memory);
    {
        std::vector<std::uint8_t> bytes;append(bytes,samples[2]);
        auto payload=SpillTilePayload::create(256,256,SpillTileKind::Constant,bytes,environment.memory,store);
        const auto lease=payload.resident();expect(lease.kind()==SpillTileKind::Constant && lease.storedBytes().size()==8,"Constant occupies one packed pixel");
        expect(lease.pixel(255,255)==samples[2] && lease.canonicalByteCount()==256u*256u*8u,"Constant extent and exact value");
        auto outputCharge=environment.memory->require(lease.canonicalByteCount());
        std::vector<std::uint8_t> full(lease.canonicalByteCount());outputCharge.commit();lease.copyCanonicalBytes(full);
        for(std::size_t i=0;i<full.size();i+=8) exact(std::span(full).subspan(i,8),bytes);
        expect(payload.spill() && payload.kind()==SpillTileKind::Constant,"Constant classification changed on eviction");
        expect(payload.resident().pixel(255,255)==samples[2],"Constant rehydration changed samples");
        auto restored=SpillTilePayload::fromSpill(256,256,SpillTileKind::Constant,payload.spillHandle(),environment.memory);
        expect(restored.kind()==SpillTileKind::Constant && restored.resident().pixel(255,255)==samples[2],"Typed constant disk factory changed classification/value");
    }
    ledger(*environment.memory);
    {
        std::vector<std::uint8_t> bytes;append(bytes,samples[3]);
        auto dense=SpillTilePayload::create(1,1,SpillTileKind::Dense,bytes,environment.memory,store);
        auto constant=SpillTilePayload::create(1,1,SpillTileKind::Constant,bytes,environment.memory,store);
        expect(dense.kind()!=constant.kind() && !dense.sameIdentity(constant),"Equal byte lengths inferred constant classification/identity");
    }
    ledger(*environment.memory);
}
void invalidInput(const fs::path& root) {
    Fixture fixture(root);Environment environment;auto store=storeAt(fixture);const auto bytes=denseBytes(2,3);
    for(const auto& dimensions:{std::pair{0,1},std::pair{1,0},std::pair{-1,1},std::pair{257,1},std::pair{1,257}})
        rejects<std::invalid_argument>([&]{SpillTilePayload::create(dimensions.first,dimensions.second,SpillTileKind::Dense,bytes,environment.memory,store);});
    rejects<std::invalid_argument>([&]{SpillTilePayload::create(2,3,static_cast<SpillTileKind>(7),bytes,environment.memory,store);});
    rejects<std::invalid_argument>([&]{SpillTilePayload::create(2,3,SpillTileKind::Dense,std::span(bytes).first(47),environment.memory,store);});
    rejects<std::invalid_argument>([&]{SpillTilePayload::create(2,3,SpillTileKind::Constant,bytes,environment.memory,store);});
    rejects<std::invalid_argument>([&]{SpillTilePayload::create(2,3,SpillTileKind::Dense,bytes,{},store);});
    rejectsSpill(SpillErrorCode::InvalidArgument,[&]{SpillTilePayload::create(2,3,SpillTileKind::Dense,bytes,environment.memory,{});});
    for(const auto invalid:{PackedPixel{0x8000,0,0,0x3c00},PackedPixel{0x7c00,0,0,0x3c00},PackedPixel{0x7e00,0,0,0x3c00},
            PackedPixel{0,0,0,0x4000},PackedPixel{0,0,0,0xbc00},PackedPixel{0x3c00,0,0,0}}) {
        auto bad=bytes;bad.resize(bad.size()-8);append(bad,invalid);
        rejects<std::domain_error>([&]{SpillTilePayload::create(2,3,SpillTileKind::Dense,bad,environment.memory,store);});
        rejects<std::domain_error>([&]{SpillTilePayload::create(2,3,SpillTileKind::Constant,std::span(bad).last(8),environment.memory,store);});
        ledger(*environment.memory);
        auto handle=store->put(bad);
        rejects<std::domain_error>([&]{SpillTilePayload::fromSpill(2,3,SpillTileKind::Dense,handle,environment.memory);});
        expect(handle.read()==bad,"Typed rejection consumed or changed caller disk handle");ledger(*environment.memory);
    }
    auto shortHandle=store->put(std::span(bytes).first(8));
    rejects<std::invalid_argument>([&]{SpillTilePayload::fromSpill(2,3,SpillTileKind::Dense,shortHandle,environment.memory);});
    rejects<std::invalid_argument>([&]{SpillTilePayload::fromSpill(2,3,SpillTileKind::Dense,{},environment.memory);});
    SpillTilePayload empty;SpillTileReadLease lease;
    expect(!empty && !lease && !empty.width() && !empty.height() && !empty.size() && !empty.sameIdentity(empty),"Default typed payload state");
    rejects<std::logic_error>([&]{empty.kind();});rejects<std::logic_error>([&]{empty.resident();});rejects<std::logic_error>([&]{empty.spill();});
    rejects<std::out_of_range>([&]{lease.pixel(0,0);});rejects<std::invalid_argument>([&]{lease.copyCanonicalBytes({});});
    ledger(*environment.memory);
}
void lifetimes(const fs::path& root) {
    Fixture fixture(root);Environment environment;auto store=storeAt(fixture);const auto bytes=denseBytes(9,5);
    auto payload=SpillTilePayload::create(9,5,SpillTileKind::Dense,bytes,environment.memory,store);auto copy=payload;
    auto oldLease=payload.resident();expect(payload.spill(),"Spill lifetime fixture");auto handle=payload.spillHandle();
    payload={};expect(copy.resident().pixel(8,4)==samples[static_cast<std::size_t>(8+3*4)%samples.size()],"Copied payload lost resident access");
    expect(copy.spill(),"Evict rehydrated lifetime fixture");copy={};
    ledger(*environment.memory,resident(bytes.size()));
    std::weak_ptr<SpillStore> weakStore=store;store.reset();expect(weakStore.expired(),"Typed disk-only handle retained store wrapper");
    auto restored=SpillTilePayload::fromSpill(9,5,SpillTileKind::Dense,handle,environment.memory);
    auto restoredCopy=restored;expect(restored.sameIdentity(restoredCopy),"Restored copies lost identity");
    auto newLease=restored.resident();exact(newLease.storedBytes(),bytes);exact(oldLease.storedBytes(),bytes);
    handle={};restored={};restoredCopy={};ledger(*environment.memory,2*resident(bytes.size()));
    oldLease={};ledger(*environment.memory,resident(bytes.size()));
    expect(newLease.linearPixel(8,4)==unpack(samples[static_cast<std::size_t>(8+3*4)%samples.size()]),"Lease lost pixels after typed/store owners died");
    newLease={};ledger(*environment.memory);
}
void admissionAndReadFailures(const fs::path& root) {
    Fixture fixture(root);Environment environment;auto store=storeAt(fixture);const auto bytes=denseBytes(7,3);
    for(const auto cap:{SpillTilePayload::metadataCharge()-1,metadata()+resident(bytes.size())-1}) {
        auto memory=std::make_shared<MemoryAdmission>(cap,0,[] {return MemorySample{abundant,0};});
        rejects<std::length_error>([&]{SpillTilePayload::create(7,3,SpillTileKind::Dense,bytes,memory,store);});
        ledger(*memory);expect(store->stats().entries==0,"Denied creation wrote disk backing");
    }
    auto payload=SpillTilePayload::create(7,3,SpillTileKind::Dense,bytes,environment.memory,store);const auto lease=payload.resident();
    payload.spill();const auto retained=metadata()+resident(bytes.size());ledger(*environment.memory,retained);
    environment.available->store(0);
    rejects<std::length_error>([&]{payload.resident();});ledger(*environment.memory,retained);
    expect(!payload.status().resident && payload.status().spilled,"Refused rehydration changed authority");exact(lease.storedBytes(),bytes);
    environment.available->store(abundant);flip(onlyFile(*store),32);
    rejectsSpill(SpillErrorCode::Corrupt,[&]{payload.resident();});ledger(*environment.memory,retained);
    expect(!payload.status().resident && payload.status().spilled,"Corrupt read published typed resident data");exact(lease.storedBytes(),bytes);
    auto handle=payload.spillHandle();
    auto isolated=std::make_shared<MemoryAdmission>(abundant,0,[] {return MemorySample{abundant,0};});
    rejectsSpill(SpillErrorCode::Corrupt,[&]{SpillTilePayload::fromSpill(7,3,SpillTileKind::Dense,handle,isolated);});ledger(*isolated);
}
void failureAndCancellation(const fs::path& root) {
    Fixture fixture(root);Environment environment;std::atomic<bool> diskFull=false;
    auto store=storeAt(fixture,[&](SpillIoStage stage,std::uint64_t) {return diskFull && stage==SpillIoStage::Write ? ENOSPC : 0;});
    const auto bytes=denseBytes(7,3);
    rejects<std::bad_alloc>([&]{SpillTilePayload::create(7,3,SpillTileKind::Dense,bytes,environment.memory,store,{},
        [](SpillBackingStage stage,std::uint64_t) {if(stage==SpillBackingStage::PublishResident) throw std::bad_alloc();});});
    ledger(*environment.memory);
    std::stop_source pre;pre.request_stop();
    rejectsSpill(SpillErrorCode::Cancelled,[&]{SpillTilePayload::create(7,3,SpillTileKind::Dense,bytes,environment.memory,store,pre.get_token());});ledger(*environment.memory);
    auto payload=SpillTilePayload::create(7,3,SpillTileKind::Dense,bytes,environment.memory,store);const auto charge=metadata()+resident(bytes.size());
    diskFull=true;rejectsSpill(SpillErrorCode::DiskSpace,[&]{payload.spill();});diskFull=false;
    expect(payload.status().resident && !payload.status().spilled && !store->stats().entries,"Failed spill dropped resident authority");ledger(*environment.memory,charge);
    rejectsSpill(SpillErrorCode::Cancelled,[&]{payload.spill(pre.get_token());});
    rejectsSpill(SpillErrorCode::Cancelled,[&]{payload.resident(pre.get_token());});ledger(*environment.memory,charge);
    auto lease=payload.resident();std::vector<std::uint8_t> destination(bytes.size(),0x5a);
    rejectsSpill(SpillErrorCode::Cancelled,[&]{lease.copyCanonicalBytes(destination,pre.get_token());});
    expect(std::all_of(destination.begin(),destination.end(),[](auto value){return value==0x5a;}),"Pre-canceled canonical copy changed destination");
    rejects<std::invalid_argument>([&]{lease.copyCanonicalBytes(std::span(destination).first(destination.size()-1));});exact(lease.storedBytes(),bytes);
    {
        std::stop_source stop;bool armed=false;
        auto canceled=SpillTilePayload::create(7,3,SpillTileKind::Dense,bytes,environment.memory,store,{},
            [&](SpillBackingStage stage,std::uint64_t) {if(armed && stage==SpillBackingStage::PublishSpill) stop.request_stop();});
        armed=true;const auto before=environment.memory->snapshot().committedCpu;
        rejectsSpill(SpillErrorCode::Cancelled,[&]{canceled.spill(stop.get_token());});
        expect(canceled.status().resident && !canceled.status().spilled,"Canceled spill published a handle/dropped cache");ledger(*environment.memory,before);
        exact(canceled.resident().storedBytes(),bytes);
    }
    ledger(*environment.memory,charge);
}
void concurrentReaders(const fs::path& root) {
    Fixture fixture(root);Environment environment;auto store=storeAt(fixture);const auto bytes=denseBytes(9,5);
    auto payload=SpillTilePayload::create(9,5,SpillTileKind::Dense,bytes,environment.memory,store);
    std::atomic<bool> failed=false;std::vector<std::jthread> workers;
    for(unsigned worker=0;worker<4;++worker) workers.emplace_back([copy=payload,&failed,&bytes,worker] {
        try {
            for(unsigned round=0;round<20;++round) {
                if((round+worker)%3==0) copy.spill();
                auto lease=copy.resident();exact(lease.storedBytes(),bytes);
                if(lease.pixel(8,4)!=samples[static_cast<std::size_t>(8+3*4)%samples.size()]) failed=true;
            }
        } catch(...) {failed=true;}
    });
    for(auto& worker:workers) {worker.join();}
    workers.clear();expect(!failed,"Concurrent typed leases changed samples or failed");
    payload.spill();ledger(*environment.memory,metadata());expect(store->stats().entries==1,"Concurrent typed transitions duplicated spill files");
    payload={};ledger(*environment.memory);expect(store->stats().entries==0,"Last typed owner retained spill disk payload");
}
}
int main(int argc,char** argv) {
    try {
        expect(argc==2,"Pass one existing absolute disk-backed fixture directory");const fs::path root(argv[1]);
        expect(root.is_absolute() && fs::is_directory(root),"Fixture directory must exist and be absolute");
        denseAndConstant(root);invalidInput(root);lifetimes(root);admissionAndReadFailures(root);failureAndCancellation(root);concurrentReaders(root);
        std::cout<<"typed rgba16f-le pixels, constants, validation, admitted leases, identity, spill lifetimes/failure and concurrency passed\n";
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
