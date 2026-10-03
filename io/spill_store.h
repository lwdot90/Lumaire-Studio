#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace compositor { class MemoryAdmission; }

namespace compositor::io {
enum class SpillErrorCode {
    InvalidArgument, UnsupportedFilesystem, Capacity, DiskSpace,
    Cancelled, Io, Corrupt, CleanupFailed
};
class SpillError final : public std::runtime_error {
public:
    SpillError(SpillErrorCode code, std::string message, int nativeError=0);
    SpillErrorCode code() const noexcept { return code_; }
    std::error_code systemError() const noexcept { return systemError_; }
private:
    SpillErrorCode code_;
    std::error_code systemError_;
};

struct SpillLimits {
    std::uint64_t maxBytes=0;
    std::uint64_t maxPayloadBytes=0;
    std::uint64_t minFreeBytes=64*1024*1024;
    std::size_t maxEntries=4096;
    std::size_t maxIoOperations=2;
    std::size_t chunkBytes=64*1024;
};
struct SpillStats {
    std::uint64_t bytes=0;
    std::size_t entries=0, activeIo=0;
    std::uint64_t cleanupFailures=0;
    bool cleanupBlocked=false;
};
enum class SpillIoStage { Preflight, CreateFile, Write, FileSync, FileClose, DirectorySync, Read, RemoveFile };
// Test seam: return zero or an errno. Must be thread-safe and must not reenter
// this store. The callback and its captures must survive all retained handles.
using SpillFault=std::function<int(SpillIoStage,std::uint64_t)>;
namespace detail { struct SpillState; struct SpillEntry; }

class SpillHandle final {
public:
    SpillHandle()=default;
    explicit operator bool() const noexcept { return bool(entry_); }
    std::uint64_t size() const noexcept;
    // Exact-sized caller buffer. Discard its contents if verification fails.
    void readInto(std::span<std::uint8_t> destination, std::stop_token stop={}) const;
    std::vector<std::uint8_t> read(std::stop_token stop={}) const;
private:
    friend class SpillStore;
    explicit SpillHandle(std::shared_ptr<const detail::SpillEntry> entry):entry_(std::move(entry)) {}
    std::shared_ptr<const detail::SpillEntry> entry_;
};

// Synchronous, session-local opaque storage. A handle retains its backing
// beyond the store's lifetime. See docs/spill-store-contract.md before use.
class SpillStore final {
public:
    SpillStore(const std::filesystem::path& directory, SpillLimits limits, SpillFault fault={});
    ~SpillStore();
    SpillStore(const SpillStore&)=delete;
    SpillStore& operator=(const SpillStore&)=delete;
    SpillStore(SpillStore&&)=delete;
    SpillStore& operator=(SpillStore&&)=delete;
    SpillHandle put(std::span<const std::uint8_t> payload, std::stop_token stop={});
    // Optional runtime accounting: call before the first put. Repeating the
    // same owner is idempotent; switching owners or admitting late is rejected.
    void admitMetadata(std::shared_ptr<MemoryAdmission> admission);
    SpillStats stats() const;
    void collectGarbage(std::stop_token stop={});
    std::filesystem::path sessionDirectory() const;
private:
    std::shared_ptr<detail::SpillState> state_;
};
}
