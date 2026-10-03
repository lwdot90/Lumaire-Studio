#include "io/spill_directory.h"
#include "io/spill_store.h"
#include <array>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <linux/magic.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

using namespace compositor::io;
namespace fs=std::filesystem;
namespace {
void expect(bool condition,const char* message) {if(!condition) throw std::runtime_error(message);}
template<class F> void rejects(SpillErrorCode code,F action,int nativeError=0) {
    try {action();}
    catch(const SpillError& error) {
        expect(error.code()==code,"Unexpected directory-policy error category");
        if(nativeError) expect(error.systemError().value()==nativeError,"Directory-policy errno was lost");
        return;
    }
    throw std::runtime_error("Unsafe directory configuration was accepted");
}
struct Fixture {
    fs::path path;
    explicit Fixture(const fs::path& parent) {
        auto pattern=(parent/"spill-directory-test-XXXXXX").string();
        const auto result=::mkdtemp(pattern.data());
        expect(result!=nullptr,"Cannot create owned disk fixture");path=result;
    }
    ~Fixture() {std::error_code error;fs::remove_all(path,error);}
};
mode_t mode(const fs::path& path) {
    struct stat info{};
    expect(::stat(path.c_str(),&info)==0,"Cannot inspect owned fixture permissions");
    return info.st_mode&07777;
}
void setMode(const fs::path& path,mode_t bits) {
    expect(::chmod(path.c_str(),bits)==0,"Cannot set owned fixture permissions");
}
void privateDirectory(const fs::path& path) {
    struct stat info{};
    expect(::stat(path.c_str(),&info)==0 && S_ISDIR(info.st_mode) && info.st_uid==::geteuid() &&
           (info.st_mode&07777)==0700,"Application directory is not private and owned");
}
std::string contents(const fs::path& path) {
    std::ifstream input(path);std::string text;std::getline(input,text);
    expect(bool(input),"Cannot read preservation fixture");return text;
}
void explicitStateAndStore(const fs::path& root) {
    Fixture fixture(root);
    const auto state=fixture.path/"state";fs::create_directory(state);setMode(state,0755);
    std::ofstream(state/"unrelated")<<"keep state file\n";
    const auto prepared=prepareSpillDirectory({state,fs::path("unused relative home")});
    expect(prepared==state/"compositor"/"spill" && mode(state)==0755,"Explicit state lost precedence or changed ancestor permissions");
    privateDirectory(state/"compositor");privateDirectory(prepared);
    std::ofstream(prepared/"unrelated")<<"keep spill file\n";
    SpillStore store(prepared,{8192,64,0,8,1,64});
    const std::array<std::uint8_t,5> input{0,255,17,0,19};
    auto handle=store.put(input);
    for(unsigned i=0;i<3;++i) {
        expect(prepareSpillDirectory({state,std::nullopt})==prepared,"Repeated directory preparation changed location");
        expect(handle.read()==std::vector<std::uint8_t>(input.begin(),input.end()),"Preparation damaged a live spill session");
    }
    expect(contents(state/"unrelated")=="keep state file" && contents(prepared/"unrelated")=="keep spill file",
           "Preparation changed unrelated files");
}
void newStateAndHome(const fs::path& root) {
    Fixture fixture(root);
    const auto state=fixture.path/"new"/"nested"/"state";
    const auto prepared=prepareSpillDirectory({state,std::nullopt});
    for(const auto& path:{fixture.path/"new",fixture.path/"new"/"nested",state,state/"compositor",prepared}) privateDirectory(path);
    const auto home=fixture.path/"home";fs::create_directory(home);setMode(home,0755);
    fs::create_directory(home/".local");setMode(home/".local",0755);
    std::ofstream(home/".local"/"unrelated")<<"another application\n";
    const auto fallback=prepareSpillDirectory({std::nullopt,home});
    expect(fallback==home/".local"/"state"/"compositor"/"spill","Injected home fallback has the wrong suffix");
    expect(mode(home)==0755 && mode(home/".local")==0755,"Home fallback changed existing ancestor permissions");
    privateDirectory(home/".local"/"state");privateDirectory(fallback.parent_path());privateDirectory(fallback);
    expect(prepareSpillDirectory({fs::path{},home})==fallback,"Empty XDG state did not use injected home");
    expect(contents(home/".local"/"unrelated")=="another application","Home fallback altered another application file");
}
void invalidRoots(const fs::path& root) {
    Fixture fixture(root);
    rejects(SpillErrorCode::InvalidArgument,[]{prepareSpillDirectory({});});
    rejects(SpillErrorCode::InvalidArgument,[&]{prepareSpillDirectory({fs::path("relative"),fixture.path});});
    rejects(SpillErrorCode::InvalidArgument,[]{prepareSpillDirectory({std::nullopt,fs::path("relative home")});});
    rejects(SpillErrorCode::InvalidArgument,[]{prepareSpillDirectory({fs::path{},fs::path{}});});
    rejects(SpillErrorCode::InvalidArgument,[]{prepareSpillDirectory({fs::path("/"),std::nullopt});});
    rejects(SpillErrorCode::InvalidArgument,[&]{prepareSpillDirectory({fixture.path/".."/"state",std::nullopt});});
    rejects(SpillErrorCode::InvalidArgument,[&]{prepareSpillDirectory({fixture.path/"."/"state",std::nullopt});});
    const auto nul=fs::path(fixture.path.string()+std::string("\0hidden",7));
    rejects(SpillErrorCode::InvalidArgument,[&]{prepareSpillDirectory({nul,std::nullopt});});
    const auto absent=fixture.path/"missing-home";
    rejects(SpillErrorCode::Io,[&]{prepareSpillDirectory({std::nullopt,absent});},ENOENT);
    expect(!fs::exists(absent) && fs::is_empty(fixture.path),"Rejected root configuration created directories");
}
void linksAndFiles(const fs::path& root) {
    Fixture fixture(root);
    const auto target=fixture.path/"target";fs::create_directory(target);setMode(target,0700);
    const auto link=fixture.path/"link";fs::create_directory_symlink(target,link);
    rejects(SpillErrorCode::InvalidArgument,[&]{prepareSpillDirectory({link,std::nullopt});});
    rejects(SpillErrorCode::InvalidArgument,[&]{prepareSpillDirectory({link/"state",std::nullopt});});
    expect(fs::is_empty(target),"Symlink rejection wrote into its target");
    const auto state=fixture.path/"state";fs::create_directory(state);setMode(state,0700);
    fs::create_directory_symlink(target,state/"compositor");
    rejects(SpillErrorCode::InvalidArgument,[&]{prepareSpillDirectory({state,std::nullopt});});
    fs::remove(state/"compositor");fs::create_directory(state/"compositor");setMode(state/"compositor",0700);
    fs::create_directory_symlink(target,state/"compositor"/"spill");
    rejects(SpillErrorCode::InvalidArgument,[&]{prepareSpillDirectory({state,std::nullopt});});
    expect(fs::is_empty(target),"Application suffix symlink rejection wrote into its target");
    const auto file=fixture.path/"file";std::ofstream(file)<<"keep regular file\n";
    rejects(SpillErrorCode::InvalidArgument,[&]{prepareSpillDirectory({file,std::nullopt});});
    expect(contents(file)=="keep regular file","Non-directory rejection changed its file");
}
void unsafePermissions(const fs::path& root) {
    Fixture fixture(root);
    const auto state=fixture.path/"state";fs::create_directory(state);setMode(state,0755);
    const auto application=state/"compositor";fs::create_directory(application);setMode(application,0750);
    rejects(SpillErrorCode::InvalidArgument,[&]{prepareSpillDirectory({state,std::nullopt});});
    expect(mode(application)==0750 && !fs::exists(application/"spill"),"Policy repaired or extended an unsafe existing application directory");
    setMode(application,0700);
    fs::create_directory(application/"spill");setMode(application/"spill",0755);
    rejects(SpillErrorCode::InvalidArgument,[&]{prepareSpillDirectory({state,std::nullopt});});
    expect(mode(application/"spill")==0755,"Policy chmodded an existing spill directory");
    const auto writable=fixture.path/"writable";fs::create_directory(writable);setMode(writable,0777);
    rejects(SpillErrorCode::InvalidArgument,[&]{prepareSpillDirectory({writable/"state",std::nullopt});});
    expect(mode(writable)==0777 && !fs::exists(writable/"state"),"Policy changed an untrusted writable ancestor");
}
void unsupportedStorage() {
    bool memoryTested=false;
    for(const auto& candidate:{fs::path("/tmp"),fs::path("/dev/shm")}) {
        struct statfs info{};
        if(::statfs(candidate.c_str(),&info)!=0 || (info.f_type!=TMPFS_MAGIC && info.f_type!=RAMFS_MAGIC)) continue;
        const auto existed=fs::exists(candidate/"compositor");
        rejects(SpillErrorCode::UnsupportedFilesystem,[&]{prepareSpillDirectory({candidate,std::nullopt});});
        expect(fs::exists(candidate/"compositor")==existed,"RAM rejection modified an application namespace");
        memoryTested=true;
    }
    expect(memoryTested,"No existing RAM filesystem available for read-only rejection (do not mount one)");
    rejects(SpillErrorCode::UnsupportedFilesystem,[]{prepareSpillDirectory({fs::path("/proc"),std::nullopt});});
}
}
int main(int argc,char** argv) {
    try {
        if(argc!=2) throw std::runtime_error("Usage: spill_directory_test /absolute/disk-backed/build-directory");
        const fs::path root=argv[1];
        expect(root.is_absolute() && fs::is_directory(root),"Supply an existing absolute disk-backed build fixture directory");
        explicitStateAndStore(root);newStateAndHome(root);invalidRoots(root);linksAndFiles(root);
        unsafePermissions(root);unsupportedStorage();
        std::cout<<"spill directory roots, private creation, unchanged ancestors, session preservation and storage rejection passed\n";
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
