# Application spill directory policy

> Export note: Recorded validation results are historical source-repository observations, not qualification of this standalone repository. Commands use the standalone layout.

Status: implemented for component integration; no builds or tests executed by this agent. Application wiring and storage qualification remain pending.

`io/spill_directory.h` exposes `prepareSpillDirectory(SpillDirectoryOptions)` and returns an absolute path for the existing `SpillStore`. Options inject `xdgStateDirectory` and `homeDirectory` as optional paths. The module never reads process environment variables and never selects `/tmp`, a runtime directory or a memory fallback.

An explicit nonempty XDG state path has precedence. It must be absolute and contain no NUL, `.` or `..` components; a relative configured value fails rather than silently choosing another location. Missing/empty XDG state uses injected absolute home plus `.local/state`. Home must already exist and belong to the effective user. Missing/empty home with no usable XDG state is an explicit error. A filesystem-root-only path is rejected. The prepared application suffix is `compositor/spill`.

Traversal starts at `/` and uses descriptor-relative `openat` with `O_DIRECTORY`, `O_NOFOLLOW` and `O_CLOEXEC`. Every opened ancestor must be owned by root or the effective user and have no group/other write bits. Existing home, `.local`, state and other ancestor modes are preserved; ordinary 0755 ancestors are supported. The `compositor` and `spill` directories must be effective-user-owned with exactly mode 0700, including no special bits. Unsafe existing directories are rejected, never repaired by chmod.

Missing state-path components and application suffixes are created with `mkdirat(...,0700)`. Only newly created owned directories may have their owner permissions finalized with descriptor-relative `fchmod`. An exceptionally restrictive umask can make reopening a new directory fail; the policy reports the failure instead of changing process umask. Partial preparation can leave newly created private ancestors for a later retry. No recursive cleanup, orphan deletion, file overwrite or change to an existing ancestor occurs. Repeated preparation leaves unrelated files and live spill sessions intact.

The selected state directory, application suffixes and any parent immediately before creation must be writable storage from the current Linux filesystem policy:

| Accepted type | Policy limit |
| --- | --- |
| ext2/ext3/ext4 family | These report the same Linux filesystem magic; the policy cannot distinguish journaling configuration. |
| btrfs | Actual backing, mount options, reserve and durability still require qualification. |
| XFS | Actual backing, mount options, reserve and durability still require qualification. |
| F2FS | Actual backing, mount options, reserve and durability still require qualification. |

tmpfs and ramfs are explicitly rejected. Overlay, FUSE, network filesystems, pseudo filesystems and unrecognized types are rejected by default; adding support requires a written storage qualification decision and implementation change. Read-only mounts are rejected. No mount, remount or storage override is performed.

Filesystem type alone cannot certify physical disk backing: accepted filesystems can sit on RAM-backed block devices, unusual virtual storage or externally configured mounts. This is a conservative filesystem allowlist, not proof of physical media or a measured compatibility result. The coordinator must qualify the actual selected location and keep the existing store's capacity/free-space and IO-failure checks. Directory preparation does not preflight a whole job or guarantee future space.

The returned path is not a pinned directory capability. Descriptors protect this preparation walk, but `SpillStore` subsequently reopens the path. A trusted ancestor can be renamed, replaced or remounted between those operations; the selected inode/filesystem may therefore change. Same-user/root namespace interference is outside this component's guarantee. The integration owner must keep the namespace stable and revalidate as needed; a future descriptor-based store constructor would remove the path-reopen boundary. No changes to the existing store/backing APIs are delivered here.

Failures reuse `SpillError`: invalid roots/symlinks/ownership/permissions use `InvalidArgument`, excluded/read-only storage uses `UnsupportedFilesystem`, capacity errno uses `DiskSpace`, and other syscall failures use `Io` with their original errno. An error never triggers a fallback directory.

Add `io/spill_directory.cpp` to `compositor_spill`. Add `tests/spill_directory_test.cpp` as `spill_directory_test`, link `compositor_spill`, and register CTest with `${CMAKE_CURRENT_BINARY_DIR}` as its sole argument and a 30-second timeout. Existing libc/Linux headers and C++20 suffice; no downloaded dependencies, Qt or Vulkan are required.

The tests create uniquely named owned fixture trees only beneath the supplied disk-backed build directory. They inject every home/state path, retain live store data across repeated preparation, preserve unrelated files and modes, and check missing/relative roots, symlink/file components and unsafe permissions. Existing `/tmp`, `/dev/shm` and `/proc` are inspected only for read-only rejection; no directory/file is created there, no mount is changed and the user's real home/environment paths are never selected. Fixture payloads are five bytes. Root performs serial build/sanitizer qualification after integration.

## Parent integration validation — 2026-10-02

Registered in CMake and run by the coordinator with one compiler/test job. The
combined GCC application suite passed 21/21, independent core suite 13/13,
Clang ASan/UBSan with leak detection 21/21, and targeted Clang TSan suite 10/10.
This component passed in each configuration. Agent authorship/manual review
above is distinct from these parent execution results. Commands and source
snapshot are in combined qualification (historical source-repository record; not included).
Application eviction and low-resource release qualification remain pending.
