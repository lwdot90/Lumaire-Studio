#pragma once
#include "core/document.h"
#include <QString>
#include <functional>
#include <optional>
#include <stop_token>

namespace compositor::io {
struct FileIdentity {
    std::uint64_t device=0, inode=0;
    std::int64_t size=0, modifiedSeconds=0, modifiedNs=0, changedSeconds=0, changedNs=0;
    bool operator==(const FileIdentity&) const = default;
};
// Missing file returns nullopt. Symlinks and nonregular files are rejected.
std::optional<FileIdentity> fileIdentity(const QString& path);
struct LoadedProject { engine::DocumentPtr document; FileIdentity identity; };
enum class SaveStage { Write, Commit, FileSync, Replace, DirectorySync, Tile };
struct SaveOptions {
    // nullopt means destination must not exist. UI must confirm replacement and
    // supply its last observed identity; changed destinations fail explicitly.
    std::optional<FileIdentity> expected;
    std::stop_token stop;
    std::function<void(SaveStage)> checkpoint; // Failure injection; empty in production.
    // Shared application admission for transient canonical/compression/binding bytes.
    std::shared_ptr<MemoryAdmission> memory;
};
LoadedProject loadProject(const QString& path, engine::TileStore& tiles, std::stop_token stop={});
FileIdentity saveProject(const QString& path, const engine::DocumentPtr& document, const SaveOptions& options={});
}
