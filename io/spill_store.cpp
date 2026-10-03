#include "io/spill_store.h"
#include "core/memory_admission.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <fcntl.h>
#include <iterator>
#include <limits>
#include <linux/magic.h>
#include <map>
#include <mutex>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <utility>

namespace compositor::io {
SpillError::SpillError(SpillErrorCode code, std::string message, int nativeError)
    :std::runtime_error(std::move(message)),code_(code),systemError_(nativeError,std::system_category()) {}
namespace {
constexpr std::size_t headerSize=32;
constexpr std::array<std::uint8_t,8> magic{'C','S','P','I','L','L','0','1'};
constexpr auto crcTable=[] {
    std::array<std::uint64_t,256> result{};
    for(std::size_t i=0;i<result.size();++i) {
        auto value=static_cast<std::uint64_t>(i)<<56;
        for(int bit=0;bit<8;++bit)
            value=(value<<1)^((value>>63) ? UINT64_C(0x42f0e1eba9ea3693) : 0);
        result[i]=value;
    }
    return result;
}();
std::uint64_t checksum(std::uint64_t crc, std::span<const std::uint8_t> bytes) {
    for(auto byte:bytes) crc=(crc<<8)^crcTable[static_cast<std::uint8_t>((crc>>56)^byte)];
    return crc;
}
std::array<std::uint8_t,headerSize> header(std::uint64_t size, std::uint64_t crc) {
    std::array<std::uint8_t,headerSize> bytes{};
    std::copy(magic.begin(),magic.end(),bytes.begin());
    for(unsigned i=0;i<8;++i) {
        bytes[8+i]=static_cast<std::uint8_t>(size>>(i*8));
        bytes[16+i]=static_cast<std::uint8_t>(crc>>(i*8));
    }
    return bytes;
}
[[noreturn]] void ioFailure(const char* action, int error=errno) {
    const auto code=(error==ENOSPC || error==EDQUOT) ? SpillErrorCode::DiskSpace : SpillErrorCode::Io;
    throw SpillError(code,std::string(action)+": "+std::system_category().message(error),error);
}
void cancelled(std::stop_token stop) {
    if(stop.stop_requested()) throw SpillError(SpillErrorCode::Cancelled,"Spill operation cancelled");
}
void corrupt(const char* message) { throw SpillError(SpillErrorCode::Corrupt,message); }
class Fd {
public:
    explicit Fd(int value=-1):value_(value) {}
    ~Fd() { if(value_>=0) ::close(value_); }
    Fd(const Fd&)=delete;
    Fd& operator=(const Fd&)=delete;
    int get() const { return value_; }
    void reset(int value) { if(value_>=0) ::close(value_); value_=value; }
    int release() { return std::exchange(value_,-1); }
private:
    int value_;
};
void diskFilesystem(int fd) {
    struct statfs info{};
    if(::fstatfs(fd,&info)<0) ioFailure("Inspect spill filesystem");
    if(info.f_type==TMPFS_MAGIC || info.f_type==RAMFS_MAGIC)
        throw SpillError(SpillErrorCode::UnsupportedFilesystem,"Bulk spill requires a disk-backed directory; tmpfs/ramfs rejected");
}
void openParent(Fd& fd, const std::filesystem::path& path) {
    if(!path.is_absolute() || path.native().find('\0')!=std::string::npos)
        throw SpillError(SpillErrorCode::InvalidArgument,"Spill directory must be an absolute path without NUL bytes");
    fd.reset(::open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
    if(fd.get()<0) ioFailure("Open spill directory root");
    for(const auto& component:path) {
        if(component=="/" || component=="." || component.empty()) continue;
        if(component=="..") throw SpillError(SpillErrorCode::InvalidArgument,"Spill directory cannot contain ..");
        const auto next=::openat(fd.get(),component.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        if(next<0) ioFailure("Open spill directory component");
        fd.reset(next);
    }
    diskFilesystem(fd.get());
}
std::string sessionName() {
    std::array<unsigned char,16> random{};
    std::size_t offset=0;
    while(offset<random.size()) {
        const auto count=::getrandom(random.data()+offset,random.size()-offset,0);
        if(count<0) { if(errno==EINTR) continue; ioFailure("Create random spill session name"); }
        if(count==0) ioFailure("Create random spill session name",EIO);
        offset+=static_cast<std::size_t>(count);
    }
    constexpr char hex[]="0123456789abcdef";
    std::string name="spill-";
    for(auto byte:random) { name+=hex[byte>>4]; name+=hex[byte&15]; }
    return name;
}
std::uint64_t availableBytes(const struct statvfs& info) {
    const auto blocks=static_cast<std::uint64_t>(info.f_bavail), unit=static_cast<std::uint64_t>(info.f_frsize);
    if(unit && blocks>std::numeric_limits<std::uint64_t>::max()/unit) return std::numeric_limits<std::uint64_t>::max();
    return blocks*unit;
}
}

namespace detail {
struct SpillState {
    struct Record {
        std::string name;
        std::uint64_t charge=0;
        dev_t device=0;
        ino_t inode=0;
        bool created=false, identified=false, pending=true, retired=false;
    };
    // Declared first: tracked containers and strings retire before the charge.
    std::optional<MemoryAdmission::Reservation> metadataCharge;
    std::shared_ptr<MemoryAdmission> metadataAdmission;
    bool admittingMetadata=false;
    const SpillLimits limits;
    const SpillFault fault;
    Fd parent, directory;
    std::string path;
    std::string name;
    dev_t device=0;
    ino_t inode=0;
    bool directoryCreated=false, directoryIdentified=false;
    std::uint64_t unit=0, bytes=0, pendingBytes=0, nextId=0, cleanupFailures=0;
    std::size_t activeIo=0;
    std::map<std::uint64_t,Record> records;
    mutable std::mutex mutex;
    std::condition_variable_any ready;

    SpillState(SpillLimits limitsValue, SpillFault faultValue):limits(limitsValue),fault(std::move(faultValue)) {}
    void initialize(const std::filesystem::path& parentPath) {
        const auto maximum=static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())-headerSize;
        if(!limits.maxBytes || !limits.maxPayloadBytes || limits.maxPayloadBytes>maximum ||
           limits.maxPayloadBytes>std::numeric_limits<std::size_t>::max() || !limits.maxEntries ||
           !limits.maxIoOperations || !limits.chunkBytes || limits.chunkBytes>1024*1024)
            throw SpillError(SpillErrorCode::InvalidArgument,"Invalid spill capacity, payload, entry, IO or chunk limits");
        openParent(parent,parentPath);
        for(unsigned attempt=0;attempt<16;++attempt) {
            name=sessionName();
            path=(parentPath/name).native();
            if(::mkdirat(parent.get(),name.c_str(),0700)==0) { directoryCreated=true; break; }
            if(errno!=EEXIST) ioFailure("Create private spill session directory");
        }
        if(!directoryCreated) ioFailure("Create unique spill session directory",EEXIST);
        struct stat identity{};
        if(::fstatat(parent.get(),name.c_str(),&identity,AT_SYMLINK_NOFOLLOW)<0) ioFailure("Identify spill session directory");
        device=identity.st_dev; inode=identity.st_ino; directoryIdentified=true;
        directory.reset(::openat(parent.get(),name.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
        if(directory.get()<0) ioFailure("Open private spill session directory");
        if(::fstat(directory.get(),&identity)<0) ioFailure("Inspect private spill session directory");
        if(identity.st_dev!=device || identity.st_ino!=inode || identity.st_uid!=::geteuid())
            corrupt("Spill session directory identity changed");
        diskFilesystem(directory.get());
        struct statvfs space{};
        if(::fstatvfs(directory.get(),&space)<0) ioFailure("Inspect spill allocation unit");
        unit=static_cast<std::uint64_t>(space.f_frsize);
        if(!unit) ioFailure("Invalid spill allocation unit",EINVAL);
    }
    void checkpoint(SpillIoStage stage, std::uint64_t progress, std::stop_token stop) const {
        cancelled(stop);
        if(fault) { const auto error=fault(stage,progress); if(error) ioFailure("Injected spill IO failure",error); }
        cancelled(stop);
    }
    bool blockedLocked() const {
        return std::any_of(records.begin(),records.end(),[](const auto& item){return item.second.retired;});
    }
    std::uint64_t reserve(std::uint64_t size, std::stop_token stop) {
        checkpoint(SpillIoStage::Preflight,0,stop);
        std::lock_guard lock(mutex);
        cancelled(stop);
        if(admittingMetadata) throw SpillError(SpillErrorCode::InvalidArgument,"Spill metadata admission is in progress");
        if(blockedLocked()) throw SpillError(SpillErrorCode::CleanupFailed,"Spill cleanup failed; collect retired files before writing");
        const auto length=size+headerSize;
        const auto padding=(unit-length%unit)%unit;
        if(length>std::numeric_limits<std::uint64_t>::max()-padding)
            throw SpillError(SpillErrorCode::Capacity,"Spill allocation size overflow");
        const auto charge=length+padding;
        if(charge>limits.maxBytes || bytes>limits.maxBytes-charge || records.size()>=limits.maxEntries ||
           nextId==std::numeric_limits<std::uint64_t>::max())
            throw SpillError(SpillErrorCode::Capacity,"Spill byte or entry capacity exhausted");
        struct statvfs space{};
        if(::fstatvfs(directory.get(),&space)<0) ioFailure("Preflight spill disk space");
        const auto available=availableBytes(space);
        if(available<limits.minFreeBytes || charge>available-limits.minFreeBytes ||
           pendingBytes>available-limits.minFreeBytes-charge)
            throw SpillError(SpillErrorCode::DiskSpace,"Spill filesystem free-space reserve would be exhausted",ENOSPC);
        const auto id=nextId+1;
        records.emplace(id,Record{"payload-"+std::to_string(id),charge});
        nextId=id; bytes+=charge; pendingBytes+=charge;
        return id;
    }
    // Called with mutex held. Never follows links or deletes a replacement.
    bool removeLocked(const Record& record) noexcept {
        if(!record.created) return true;
        try {
            if(!record.identified) return false;
            struct stat identity{};
            if(::fstatat(directory.get(),record.name.c_str(),&identity,AT_SYMLINK_NOFOLLOW)<0) return errno==ENOENT;
            if(identity.st_dev!=record.device || identity.st_ino!=record.inode || !S_ISREG(identity.st_mode)) return false;
            if(fault && fault(SpillIoStage::RemoveFile,0)) return false;
            return ::unlinkat(directory.get(),record.name.c_str(),0)==0 || errno==ENOENT;
        } catch(...) { return false; }
    }
    auto cleanLocked(std::map<std::uint64_t,Record>::iterator it) noexcept {
        if(removeLocked(it->second)) {
            bytes-=it->second.charge;
            if(it->second.pending) pendingBytes-=it->second.charge;
            return records.erase(it);
        }
        if(cleanupFailures!=std::numeric_limits<std::uint64_t>::max()) ++cleanupFailures;
        return std::next(it);
    }
    void retire(std::uint64_t id) noexcept {
        if(!id) return;
        std::lock_guard lock(mutex);
        const auto it=records.find(id);
        if(it==records.end()) return;
        it->second.retired=true;
        cleanLocked(it);
    }
    ~SpillState() {
        for(auto it=records.begin();it!=records.end();) it=cleanLocked(it);
        if(directoryCreated && directoryIdentified) {
            struct stat identity{};
            if(::fstatat(parent.get(),name.c_str(),&identity,AT_SYMLINK_NOFOLLOW)==0 &&
               identity.st_dev==device && identity.st_ino==inode && S_ISDIR(identity.st_mode))
                ::unlinkat(parent.get(),name.c_str(),AT_REMOVEDIR);
        }
    }
};
struct SpillEntry {
    const std::shared_ptr<SpillState> state;
    const std::uint64_t size;
    std::uint64_t id=0, crc=0;
    dev_t device=0;
    ino_t inode=0;
    std::string name;
    SpillEntry(std::shared_ptr<SpillState> stateValue, std::uint64_t sizeValue):state(std::move(stateValue)),size(sizeValue) {}
    ~SpillEntry() { state->retire(id); }
};
}
namespace {
class IoLease {
public:
    IoLease(detail::SpillState& state, std::stop_token stop):state_(state) {
        std::unique_lock lock(state_.mutex);
        if(!state_.ready.wait(lock,stop,[&]{return state_.activeIo<state_.limits.maxIoOperations;})) cancelled(stop);
        cancelled(stop);
        if(state_.admittingMetadata) throw SpillError(SpillErrorCode::InvalidArgument,"Spill metadata admission is in progress");
        ++state_.activeIo;
    }
    ~IoLease() {
        { std::lock_guard lock(state_.mutex); --state_.activeIo; }
        state_.ready.notify_all();
    }
    IoLease(const IoLease&)=delete;
    IoLease& operator=(const IoLease&)=delete;
private:
    detail::SpillState& state_;
};
void writeBytes(detail::SpillState& state, int fd, std::span<const std::uint8_t> bytes,
                std::uint64_t fileOffset, std::uint64_t progress, std::stop_token stop) {
    std::size_t offset=0;
    while(offset<bytes.size()) {
        state.checkpoint(SpillIoStage::Write,progress+(fileOffset>=headerSize ? offset : 0),stop);
        const auto count=::pwrite(fd,bytes.data()+offset,std::min(bytes.size()-offset,state.limits.chunkBytes),
                                  static_cast<off_t>(fileOffset+offset));
        if(count<0) { if(errno==EINTR) continue; ioFailure("Write spill payload"); }
        if(!count) ioFailure("Zero-length spill write",EIO);
        offset+=static_cast<std::size_t>(count);
    }
}
void readBytes(detail::SpillState& state, int fd, std::span<std::uint8_t> bytes,
               std::uint64_t fileOffset, std::uint64_t progress, std::stop_token stop) {
    std::size_t offset=0;
    while(offset<bytes.size()) {
        state.checkpoint(SpillIoStage::Read,progress+(fileOffset>=headerSize ? offset : 0),stop);
        const auto count=::pread(fd,bytes.data()+offset,std::min(bytes.size()-offset,state.limits.chunkBytes),
                                 static_cast<off_t>(fileOffset+offset));
        if(count<0) { if(errno==EINTR) continue; ioFailure("Read spill payload"); }
        if(!count) corrupt("Truncated spill payload");
        offset+=static_cast<std::size_t>(count);
    }
}
void syncFile(detail::SpillState& state, int fd, SpillIoStage stage, std::uint64_t progress, std::stop_token stop) {
    do {
        state.checkpoint(stage,progress,stop);
        if(::fsync(fd)==0) { cancelled(stop); return; }
    } while(errno==EINTR);
    ioFailure("Sync spill storage");
}
}
SpillStore::SpillStore(const std::filesystem::path& directory, SpillLimits limits, SpillFault fault)
    :state_(std::make_shared<detail::SpillState>(limits,std::move(fault))) { state_->initialize(directory); }
SpillStore::~SpillStore()=default;
SpillHandle SpillStore::put(std::span<const std::uint8_t> payload, std::stop_token stop) {
    const auto state=state_;
    cancelled(stop);
    if(payload.size()>state->limits.maxPayloadBytes)
        throw SpillError(SpillErrorCode::Capacity,"Payload exceeds spill per-payload limit");
    IoLease lease(*state,stop);
    auto entry=std::make_shared<detail::SpillEntry>(state,payload.size());
    entry->id=state->reserve(payload.size(),stop);
    { std::lock_guard lock(state->mutex); entry->name=state->records.at(entry->id).name; }
    state->checkpoint(SpillIoStage::CreateFile,0,stop);
    Fd fd(::openat(state->directory.get(),entry->name.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600));
    if(fd.get()<0) ioFailure("Create spill payload");
    { std::lock_guard lock(state->mutex); state->records.at(entry->id).created=true; }
    struct stat identity{};
    if(::fstat(fd.get(),&identity)<0) ioFailure("Identify spill payload");
    entry->device=identity.st_dev; entry->inode=identity.st_ino;
    {
        std::lock_guard lock(state->mutex);
        auto& record=state->records.at(entry->id);
        record.device=entry->device; record.inode=entry->inode; record.identified=true;
    }
    for(std::size_t offset=0;offset<payload.size();) {
        const auto chunk=payload.subspan(offset,std::min(payload.size()-offset,state->limits.chunkBytes));
        writeBytes(*state,fd.get(),chunk,headerSize+offset,offset,stop);
        entry->crc=checksum(entry->crc,chunk);
        offset+=chunk.size();
    }
    const auto encodedHeader=header(entry->size,entry->crc);
    writeBytes(*state,fd.get(),encodedHeader,0,entry->size,stop);
    syncFile(*state,fd.get(),SpillIoStage::FileSync,entry->size,stop);
    if(::fstat(fd.get(),&identity)<0) ioFailure("Inspect written spill allocation");
    {
        std::lock_guard lock(state->mutex);
        const auto charge=state->records.at(entry->id).charge;
        if(identity.st_blocks<0 || static_cast<std::uint64_t>(identity.st_blocks)>charge/512)
            throw SpillError(SpillErrorCode::Capacity,"Spill filesystem allocated more blocks than reserved");
    }
    state->checkpoint(SpillIoStage::FileClose,entry->size,stop);
    if(::close(fd.release())<0) ioFailure("Close written spill payload"); // Linux consumes the fd even on error.
    cancelled(stop);
    syncFile(*state,state->directory.get(),SpillIoStage::DirectorySync,entry->size,stop);
    {
        std::lock_guard lock(state->mutex);
        cancelled(stop);
        auto& record=state->records.at(entry->id);
        record.pending=false; state->pendingBytes-=record.charge;
    }
    return SpillHandle(std::move(entry));
}
void SpillStore::admitMetadata(std::shared_ptr<MemoryAdmission> admission) {
    if(!admission) throw SpillError(SpillErrorCode::InvalidArgument,"Missing spill metadata admission owner");
    const auto state=state_;
    {
        std::lock_guard lock(state->mutex);
        if(state->metadataAdmission) {
            if(state->metadataAdmission==admission) return;
            throw SpillError(SpillErrorCode::InvalidArgument,"Spill metadata already belongs to another admission owner");
        }
        if(state->admittingMetadata || state->nextId || state->activeIo || !state->records.empty())
            throw SpillError(SpillErrorCode::InvalidArgument,"Spill metadata must be admitted before use");
        state->admittingMetadata=true;
    }
    try {
        // Conservative requested-allocation allowances, shared with the
        // backing contract. Allocator arenas/implementation overhead remain
        // unqualified; there is no portable STL node/control-block size API.
        constexpr std::uint64_t controlAllowance=64;
        constexpr std::uint64_t nodeAllowance=4*sizeof(void*)+alignof(std::max_align_t);
        constexpr std::uint64_t payloadNameCharacters=8+20; // prefix + uint64 decimal
        constexpr std::uint64_t payloadNameAllowance=2*payloadNameCharacters+1;
        const auto add=[](std::uint64_t a,std::uint64_t b) {
            if(b>std::numeric_limits<std::uint64_t>::max()-a)
                throw SpillError(SpillErrorCode::Capacity,"Spill metadata size overflow");
            return a+b;
        };
        const auto multiply=[](std::uint64_t a,std::uint64_t b) {
            if(a && b>std::numeric_limits<std::uint64_t>::max()/a)
                throw SpillError(SpillErrorCode::Capacity,"Spill metadata size overflow");
            return a*b;
        };
        auto charge=add(add(sizeof(detail::SpillState),controlAllowance),
            add(add(state->name.capacity(),1),add(state->path.capacity(),1)));
        const auto recordCharge=add(add(sizeof(std::pair<const std::uint64_t,detail::SpillState::Record>),
            nodeAllowance),payloadNameAllowance);
        charge=add(charge,multiply(state->limits.maxEntries,recordCharge));
        // A put allocates its entry before claiming a map slot; active IO bounds
        // those transient entries even while every live record is occupied.
        const auto entries=add(state->limits.maxEntries,state->limits.maxIoOperations);
        charge=add(charge,multiply(entries,add(add(sizeof(detail::SpillEntry),controlAllowance),payloadNameAllowance)));
        auto reservation=admission->reserve(charge);
        if(!reservation) throw std::length_error("Spill metadata exceeds memory admission policy");
        reservation->commit();
        std::lock_guard lock(state->mutex);
        state->metadataCharge.emplace(std::move(*reservation));
        state->metadataAdmission=std::move(admission);
        state->admittingMetadata=false;
    } catch(...) {
        std::lock_guard lock(state->mutex);
        state->admittingMetadata=false;
        throw;
    }
}
SpillStats SpillStore::stats() const {
    std::lock_guard lock(state_->mutex);
    return {state_->bytes,state_->records.size(),state_->activeIo,state_->cleanupFailures,state_->blockedLocked()};
}
void SpillStore::collectGarbage(std::stop_token stop) {
    std::lock_guard lock(state_->mutex);
    cancelled(stop);
    for(auto it=state_->records.begin();it!=state_->records.end();) {
        cancelled(stop);
        if(it->second.retired) it=state_->cleanLocked(it); else ++it;
    }
    if(state_->blockedLocked()) throw SpillError(SpillErrorCode::CleanupFailed,"Retired spill files could not be safely removed");
}
std::filesystem::path SpillStore::sessionDirectory() const { return state_->path; }
std::uint64_t SpillHandle::size() const noexcept { return entry_ ? entry_->size : 0; }
void SpillHandle::readInto(std::span<std::uint8_t> destination, std::stop_token stop) const {
    const auto entry=entry_; // Pins both file and state throughout verification.
    cancelled(stop);
    if(!entry || destination.size()!=entry->size)
        throw SpillError(SpillErrorCode::InvalidArgument,"Spill read requires a live handle and exact-sized destination");
    const auto state=entry->state;
    IoLease lease(*state,stop);
    Fd fd(::openat(state->directory.get(),entry->name.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK));
    if(fd.get()<0) { if(errno==ELOOP) corrupt("Spill payload was replaced by a symlink"); ioFailure("Open spill payload for read"); }
    struct stat before{};
    if(::fstat(fd.get(),&before)<0) ioFailure("Inspect spill payload");
    if(!S_ISREG(before.st_mode) || before.st_dev!=entry->device || before.st_ino!=entry->inode ||
       before.st_nlink!=1 || before.st_size<0 || static_cast<std::uint64_t>(before.st_size)!=headerSize+entry->size)
        corrupt("Spill payload identity or length changed");
    std::array<std::uint8_t,headerSize> encodedHeader{};
    readBytes(*state,fd.get(),encodedHeader,0,0,stop);
    if(encodedHeader!=header(entry->size,entry->crc)) corrupt("Spill header is corrupt");
    std::uint64_t crc=0;
    for(std::size_t offset=0;offset<destination.size();) {
        auto chunk=destination.subspan(offset,std::min(destination.size()-offset,state->limits.chunkBytes));
        readBytes(*state,fd.get(),chunk,headerSize+offset,offset,stop);
        crc=checksum(crc,chunk);
        offset+=chunk.size();
    }
    if(crc!=entry->crc) corrupt("Spill payload checksum mismatch");
    struct stat after{};
    if(::fstat(fd.get(),&after)<0) ioFailure("Recheck spill payload");
    if(after.st_size!=before.st_size || after.st_nlink!=before.st_nlink ||
       after.st_mtim.tv_sec!=before.st_mtim.tv_sec || after.st_mtim.tv_nsec!=before.st_mtim.tv_nsec ||
       after.st_ctim.tv_sec!=before.st_ctim.tv_sec || after.st_ctim.tv_nsec!=before.st_ctim.tv_nsec)
        corrupt("Spill payload changed during read");
    cancelled(stop);
}
std::vector<std::uint8_t> SpillHandle::read(std::stop_token stop) const {
    cancelled(stop);
    if(!entry_) throw SpillError(SpillErrorCode::InvalidArgument,"Cannot read an empty spill handle");
    std::vector<std::uint8_t> result(static_cast<std::size_t>(entry_->size));
    readInto(result,stop);
    return result;
}
}
