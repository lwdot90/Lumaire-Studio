#pragma once
#include <filesystem>
#include <optional>

namespace compositor::io {
struct SpillDirectoryOptions {
    // Unset/empty XDG state uses injected home/.local/state. Relative values
    // fail explicitly; this component never reads environment variables.
    std::optional<std::filesystem::path> xdgStateDirectory;
    std::optional<std::filesystem::path> homeDirectory;
};
// Prepare a user-owned 0700 compositor/spill suffix on supported Linux disk
// storage. Existing ancestors keep their permissions; no /tmp/RAM fallback.
// This returns a path, not a pinned directory capability. The caller must keep
// its trusted namespace stable until SpillStore reopens it. See the contract.
std::filesystem::path prepareSpillDirectory(const SpillDirectoryOptions& options);
}
