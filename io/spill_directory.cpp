#include "io/spill_directory.h"
#include "io/spill_store.h"
#include <cerrno>
#include <fcntl.h>
#include <linux/magic.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <vector>

namespace compositor::io {
namespace {
class Fd {
public:
    explicit Fd(int value=-1):value_(value) {}
    ~Fd() {if(value_>=0) ::close(value_);}
    Fd(const Fd&)=delete;
    Fd& operator=(const Fd&)=delete;
    int get() const {return value_;}
    void reset(int value) {if(value_>=0) ::close(value_);value_=value;}
private:
    int value_;
};
[[noreturn]] void failed(const char* action,int error=errno) {
    const auto code=(error==ENOSPC || error==EDQUOT) ? SpillErrorCode::DiskSpace : SpillErrorCode::Io;
    throw SpillError(code,std::string(action)+": "+std::system_category().message(error),error);
}
void validPath(const std::filesystem::path& path) {
    if(!path.is_absolute() || path.native().find('\0')!=std::string::npos)
        throw SpillError(SpillErrorCode::InvalidArgument,"Spill storage root must be an absolute path without NUL bytes");
    if(path==path.root_path())
        throw SpillError(SpillErrorCode::InvalidArgument,"Spill storage root must name a directory below the filesystem root");
    for(const auto& component:path.relative_path())
        if(component==".." || component==".")
            throw SpillError(SpillErrorCode::InvalidArgument,"Spill storage root cannot contain . or .. components");
}
struct stat metadata(int fd) {
    struct stat info{};
    if(::fstat(fd,&info)<0) failed("Inspect spill directory ownership");
    if(!S_ISDIR(info.st_mode))
        throw SpillError(SpillErrorCode::InvalidArgument,"Spill storage component is not a directory");
    return info;
}
void trustedAncestor(int fd) {
    const auto info=metadata(fd);
    if((info.st_uid!=0 && info.st_uid!=::geteuid()) || (info.st_mode&0022)!=0)
        throw SpillError(SpillErrorCode::InvalidArgument,"Spill storage ancestors must have trusted ownership and no group/other write access");
}
void privateDirectory(int fd) {
    const auto info=metadata(fd);
    if(info.st_uid!=::geteuid() || (info.st_mode&07777)!=0700)
        throw SpillError(SpillErrorCode::InvalidArgument,"Application spill directories must be owned by the effective user with mode 0700");
}
void diskFilesystem(int fd) {
    struct statfs info{};
    if(::fstatfs(fd,&info)<0) failed("Inspect spill storage filesystem");
    const auto type=static_cast<std::uint64_t>(info.f_type);
    if(type==TMPFS_MAGIC || type==RAMFS_MAGIC)
        throw SpillError(SpillErrorCode::UnsupportedFilesystem,"Bulk spill cannot use tmpfs or ramfs; select qualified disk storage");
    switch(type) {
        case EXT4_SUPER_MAGIC: // ext2/ext3/ext4 share the same Linux type.
        case BTRFS_SUPER_MAGIC:
        case XFS_SUPER_MAGIC:
        case F2FS_SUPER_MAGIC:
            break;
        default:
            throw SpillError(SpillErrorCode::UnsupportedFilesystem,
                             "Spill storage filesystem is outside the qualified Linux disk policy (type "+std::to_string(type)+")");
    }
    struct statvfs capacity{};
    if(::fstatvfs(fd,&capacity)<0) failed("Inspect spill storage mount flags");
    if((capacity.f_flag&ST_RDONLY)!=0)
        throw SpillError(SpillErrorCode::UnsupportedFilesystem,"Spill storage filesystem is read-only",EROFS);
}
void openChild(Fd& parent,const std::filesystem::path& name,bool create,bool application,bool storage=false) {
    auto next=::openat(parent.get(),name.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    bool created=false;
    if(next<0 && errno==ENOENT && create) {
        // Reject RAM/unknown storage before making even a missing ancestor.
        diskFilesystem(parent.get());
        if(::mkdirat(parent.get(),name.c_str(),0700)==0) created=true;
        else if(errno!=EEXIST) failed("Create private spill directory");
        next=::openat(parent.get(),name.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    }
    if(next<0) {
        const auto error=errno;
        if(error==ELOOP || error==ENOTDIR)
            throw SpillError(SpillErrorCode::InvalidArgument,"Spill storage components must be real directories, never symlinks or files",error);
        failed("Open spill storage directory",error);
    }
    parent.reset(next);
    if(application || storage) diskFilesystem(parent.get());
    trustedAncestor(parent.get());
    if(created) {
        // Never chmod a reused directory. Tighten only a newly created owned
        // inode; an exceptionally restrictive umask can instead make open fail.
        const auto info=metadata(parent.get());
        if(info.st_uid!=::geteuid() || (info.st_mode&0077)!=0)
            throw SpillError(SpillErrorCode::InvalidArgument,"New spill directory ownership or private permissions changed during preparation");
        if(::fchmod(parent.get(),0700)<0) failed("Set new spill directory permissions");
        privateDirectory(parent.get());
    }
    if(application) {
        privateDirectory(parent.get());
    }
}
void walk(Fd& directory,const std::filesystem::path& path,bool create) {
    directory.reset(::open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
    if(directory.get()<0) failed("Open spill storage root");
    trustedAncestor(directory.get());
    std::vector<std::filesystem::path> components;
    for(const auto& component:path.relative_path()) if(!component.empty()) components.push_back(component);
    for(std::size_t i=0;i<components.size();++i)
        openChild(directory,components[i],create,false,i+1==components.size());
}
}
std::filesystem::path prepareSpillDirectory(const SpillDirectoryOptions& options) {
    std::filesystem::path state;
    Fd directory;
    if(options.xdgStateDirectory && !options.xdgStateDirectory->empty()) {
        state=*options.xdgStateDirectory;
        validPath(state);
        walk(directory,state,true);
    } else {
        if(!options.homeDirectory || options.homeDirectory->empty())
            throw SpillError(SpillErrorCode::InvalidArgument,"Spill storage needs absolute XDG state or an injected existing home directory");
        const auto& home=*options.homeDirectory;
        validPath(home);
        // A missing injected home is an error; only its state suffix is created.
        walk(directory,home,false);
        if(metadata(directory.get()).st_uid!=::geteuid())
            throw SpillError(SpillErrorCode::InvalidArgument,"Injected spill home must belong to the effective user");
        diskFilesystem(directory.get());
        openChild(directory,".local",true,false);
        openChild(directory,"state",true,false);
        state=home/".local"/"state";
    }
    diskFilesystem(directory.get());
    openChild(directory,"compositor",true,true);
    openChild(directory,"spill",true,true);
    return (state/"compositor"/"spill").lexically_normal();
}
}
