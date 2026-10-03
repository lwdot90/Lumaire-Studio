# Immutable session spill store

> Export note: Recorded validation results are historical source-repository observations, not qualification of this standalone repository. Commands use the standalone layout.

This component holds opaque immutable byte payloads for future tile and undo
adapters. It does not interpret pixels, change sample precision, prune history,
set application RAM budgets, schedule workers, or modify `.cproj` files.
Storage is session-local; recovery and project persistence remain separate.

## API

`io/spill_store.h` exposes the following in `compositor::io`:

| API | Contract |
| --- | --- |
| `SpillStore(directory, limits, fault)` | Creates a private, uniquely named session below an existing absolute directory. No environment-selected fallback. |
| `put(span<const uint8_t>, stop_token)` | Synchronously writes and syncs a complete immutable payload, then returns a copyable `SpillHandle`. Empty payloads are valid. |
| `SpillHandle::size()` | Original byte length, independent of file contents. |
| `SpillHandle::readInto(span<uint8_t>, stop_token)` | Destination must have exactly `size()` bytes. Checks file identity, header, length and checksum before reporting success. On failure the destination may contain partial/unverified bytes and must be discarded. |
| `SpillHandle::read(stop_token)` | Convenience allocation of exactly `size()` bytes; only returns a vector after verification. Caller must admit/account for this allocation. |
| `stats()` | Accounted disk bytes, retained file count, active IO count, cleanup failures and cleanup-blocked status. |
| `collectGarbage(stop_token)` | Retries removal of retired files only. Reports cleanup failure; never removes a file belonging to a live handle. |
| `sessionDirectory()` | Diagnostic path for testing/inspection; never an integration identifier or a persisted reference. |

A default/empty handle is false and has size zero; reading it fails with
`InvalidArgument`. Handle copies share one allocation. Store objects cannot be
copied or moved. Different handle instances and all store methods may be used
concurrently. Destroying/reassigning the same object while another thread uses
it requires caller synchronization. A read pins its payload until it returns.

`SpillLimits` requires explicit `maxBytes` and `maxPayloadBytes`. It also bounds
`maxEntries`, `maxIoOperations` and transfer `chunkBytes`, and reserves
`minFreeBytes` of filesystem available space. Capacity is charged before IO,
including concurrent writes and retired files whose cleanup failed. File header
and payload length are rounded up to the filesystem allocation unit; actual
allocated blocks are checked before publication. Filesystem directory, inode,
journal and other metadata are outside the byte charge, with file count bounded
separately. Quotas/other processes can still exhaust the filesystem after
preflight; those failures are explicit.

## Lifetime and cleanup

Each handle owns its payload and the shared session state. Dropping a store
does not revoke handles: reads continue to work until the last handle/read
releases the payload. Dropping the last reference attempts to unlink that
payload and releases its disk charge only after removal succeeds. Destructors
are nonthrowing. Failed removals stay charged, increment the cleanup-failure
counter and block new puts until `collectGarbage()` succeeds. Existing handles
remain readable. Final state destruction retries retired files and attempts to
remove the now-empty session directory.

Cleanup is descriptor-relative, never recursive. It removes only recorded
files whose device/inode still match, then the originally created empty session
directory if its identity still matches. It leaves unknown files, replacements,
other sessions and the injected parent directory alone. Release can perform
filesystem IO; retire final handles on an appropriate worker. Cleanup failures
after the last observable owner is gone leave artifacts for a future explicit
janitor; there is no automatic crash-orphan scanner in this component.

The caller supplies an existing trusted cache/state directory on local disk.
Path components may not be symlinks or `..`. The opened parent and new session
are checked with `fstatfs`; tmpfs and ramfs are rejected without an override.
There is no fallback to `/tmp`. Filesystem-type checks cannot certify the
physical backing of arbitrary FUSE, network or overlay mounts; the application
must qualify its selected storage location. Sessions use mode 0700 and files
0600 (umask may further restrict permissions); descriptors use close-on-exec.
The private directory must not be modified by outside code. Identity checks
reduce accidental replacement risk; this is not a defense against a malicious
process running as the same user or a concurrent namespace attacker.

## Integrity, publication and failure

Version 1 stores uncompressed bytes with a 32-byte little-endian header: eight
magic/version bytes `CSPILL01`, payload length (u64), CRC-64/ECMA-182 (u64), and
eight reserved zero bytes. CRC starts at zero with polynomial
`0x42f0e1eba9ea3693`, no reflection and no final xor. Length/checksum are also
retained in the handle; file-provided sizes never drive allocations. CRC covers
the entire opaque payload. Header comparison, exact file length and checksum
reject corruption/truncation/trailing bytes, including empty payloads. This
detects accidental corruption, not authenticated malicious modification.
Compression can be added behind this API later; version 1 trades disk footprint
for a dependency-free, bounded implementation and never narrows samples.

`put()` publishes only after header/payload writes, file sync, allocated-size
validation, checked close and session-directory sync succeed. Failure or
observed cancellation returns no handle, retires the partial file, and leaves
all prior handles untouched. Caller-owned input must remain immutable for the
duration of `put()`. Keep the resident/previous authoritative copy until put
returns successfully. Successful sync is not a reopen/recovery protocol:
there is no persistent index or promise to recover sessions after a crash.

`SpillError` distinguishes `InvalidArgument`, `UnsupportedFilesystem`,
`Capacity`, `DiskSpace`, `Cancelled`, `Io`, `Corrupt` and `CleanupFailed`, with
the underlying errno in `systemError()` when present. C++ allocation failures
may also propagate. Capacity errors do not delete live data. Read errors never
replace a document revision. Resource owners decide whether to retry, pause,
retain RAM, or report disk failure.

Operations are synchronous and create no threads or background queues. A
stop-aware admission gate bounds active puts/reads per session. Cancellation is
checked while waiting and between bounded transfer chunks, around sync/close,
and immediately before publication/return. A stop racing after the final check
may observe success. Kernel calls such as fsync cannot be interrupted reliably
by a stop token; this component does not promise a 250 ms wall-clock cancel
deadline. The scheduler must stop new work promptly and let bounded in-flight
IO retire safely. `readInto()` uses constant-size internal scratch; `read()`
adds its explicitly sized returned allocation. Caller buffers, handle copies,
kernel page cache and application-wide IO concurrency remain caller-owned
accounting/scheduling concerns.

The optional `SpillFault` callback is a deterministic test seam, empty in
production. It receives an IO stage and completed payload-byte count and
returns zero or an errno to simulate failure before that operation. It can
also request cancellation. It must be thread-safe, must not reenter the store,
and must not retain input/output spans. Cleanup invokes it from nonthrowing
destructors; thrown exceptions there are treated as cleanup failure. Injection
does not replace real small-file writes or alter unrelated filesystem state.

Linux syscall details: [filesystem type checks](https://man7.org/linux/man-pages/man2/statfs.2.html),
[available space](https://man7.org/linux/man-pages/man3/statvfs.3.html),
[file/directory sync](https://man7.org/linux/man-pages/man2/fsync.2.html), and
[checked close without retry on Linux](https://man7.org/linux/man-pages/man2/close.2.html).

## Integration boundary

1. Add `io/spill_store.cpp` to a Qt/Vulkan-independent IO target and link
   `Threads::Threads`; register `tests/spill_store_test.cpp` separately.
2. Supply a verified disk-backed XDG cache/state directory and limits from the
   application resource policy. Estimate operation/output/recovery space
   separately; do not treat this store's reserve as a whole-job preflight.
3. Keep a `SpillHandle` in each immutable backing record shared by current
   snapshots, history and outstanding jobs. Release/prune through those owners.
4. On an IO worker, put the canonical bytes before evicting their resident
   copy. Admit rehydration memory, read and verify, then install the reconstructed
   tile only after the entire read succeeds.
5. Implement recovery and streaming project save independently, then qualify
   pressure, coexistence, cancellation latency and final typed sample fidelity.

This component's tests cannot establish LP01–LP05/P17–P21 compatibility.
Application integration, compression, crash-orphan policy and those release
gates remain pending.

## Handoff evidence

Completed component validation on 2026-10-02. Owned source files:

- [Public API](../io/spill_store.h)
- [Implementation](../io/spill_store.cpp)
- [Standalone tests](../tests/spill_store_test.cpp)
- This contract and handoff.

Dependencies: C++20 standard library (including stop-aware condition variables),
Linux libc/syscalls (`openat`, `mkdirat`, `pread`/`pwrite`, `fstat`/`fstatat`,
`fstatfs`, `fstatvfs`, `getrandom`, `fsync`, `close`, `unlinkat`) and POSIX threads.
No Qt, Vulkan, zstd, SQLite, OpenSSL or additional downloaded dependency is
required. Tests use the standard library and existing disk/tmpfs mounts only.
Compiler versions: GCC 16.2.1 and Clang 22.1.8. Fixture storage was btrfs under
the separate build directory; `/tmp` and `/dev/shm` exercised tmpfs rejection.

Run these exact commands from `.`. Each compiler invocation
processes its translation units sequentially; there are no concurrent compiler
jobs and no top-level CMake changes.

```sh
mkdir -p build/spill-agent
g++ -std=c++20 -O2 -g -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror -pthread -I. io/spill_store.cpp tests/spill_store_test.cpp -o build/spill-agent/spill_store_test
build/spill-agent/spill_store_test ./build/spill-agent

clang++ -std=c++20 -O1 -g -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer -I. io/spill_store.cpp tests/spill_store_test.cpp -o build/spill-agent/spill_store_test_asan
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 build/spill-agent/spill_store_test_asan ./build/spill-agent

clang++ -std=c++20 -O1 -g -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror -pthread -fsanitize=thread -fno-omit-frame-pointer -I. io/spill_store.cpp tests/spill_store_test.cpp -o build/spill-agent/spill_store_test_tsan
TSAN_OPTIONS=halt_on_error=1 build/spill-agent/spill_store_test_tsan ./build/spill-agent
```

| Final configuration | Build | Test result |
| --- | --- | --- |
| GCC optimized with warnings as errors | Exit 0 | Exit 0; all 8 groups passed |
| Clang ASan + UBSan + LeakSanitizer | Exit 0 | Exit 0; all 8 groups passed; no sanitizer/leak report |
| Clang ThreadSanitizer | Exit 0 | Exit 0; all 8 groups passed; no race report |

LeakSanitizer cannot run under the execution sandbox's ptrace tracing. The
initial traced run passed the assertions but exited 1 at LeakSanitizer shutdown;
the final ASan/UBSan/leak command above ran outside that traced sandbox with
approved execution and passed. ThreadSanitizer passed in the normal sandbox.
No claim about untested syscall fault combinations follows from these runs.

The eight groups cover exact arbitrary bytes/empty/boundary lengths and the
CRC known vector; one-byte chunks and fault progress; shared handles, active
reads after store destruction and parent renaming; byte/entry/reserve limits and
concurrent pending reservations; header/payload corruption, truncation, trailing
bytes, replacement/symlink/hardlink and mid-read mutation; four small concurrent
put/read callers; pre-cancellation, partial-write/read cancellation,
pre-publication cancellation and stop-aware queued put/read admission; injected
EIO/ENOSPC/EDQUOT at preflight/create/write/sync/close/directory-sync, read errors,
nonthrowing cleanup errors and combined disk/cleanup failure; and invalid,
missing, symlinked and memory-backed storage directories. Largest payload is
64 KiB; there is no full-disk/full-memory stress, mount change or desktop setting
change. Fixtures are uniquely created beneath the supplied build directory and
only their owned trees are removed afterward.

The source API is ready for the integration steps above. The integration owner
must add the build target/CTest registration (pass a known disk-backed fixture
directory), set total resource policy, connect immutable backing records and
rehydration, and implement recovery/streamed save. No existing CMake, engine,
render, UI, project-format or document file was modified. No document wiring,
application memory/worker policy or LP qualification is delivered here.

## Optional runtime metadata admission

`admitMetadata(shared_ptr<MemoryAdmission>)` prepays a committed CPU reservation
before the first operation. Repeating the same owner is idempotent; null,
different, concurrent and late owners are rejected. Admission denial changes no
ledger or entries, permits a later valid admission, and uses non-reclaiming
`reserve` to avoid synchronous storage-reclamation recursion. Legacy standalone
stores may omit the hook; runtime-created stores must call it before publishing
or using the store.

The bound includes `sizeof(SpillState)`, existing session-name/native-path string
capacities plus terminators, a 64-byte shared-control allowance, `maxEntries`
record values with a tree-node allowance of four pointers plus maximum alignment,
and `maxEntries+maxIoOperations` entry objects with shared-control and name
allowances. Each persistent payload name uses at most 28 characters (`payload-`
plus 20 decimal uint64 digits); the allocation allowance is twice that length
plus a terminator for both record and entry copies. Checked additions and
multiplications reject overflow before admission. The native session path is
stored as a string to avoid a second persistent filesystem-path component cache;
`sessionDirectory()` still returns the original filesystem path value.

These explicit node/control/string-growth allowances conservatively cover the
qualified Linux standard-library requested allocations. They do not certify
allocator arena fragmentation, implementation-private allocation overhead,
arbitrary user fault-callback captures, or another STL implementation. Fixed
state/session construction happens before this hook and is retrospectively
charged; admission precedes all payload-entry allocation in runtime use. The
reservation is declared before tracked storage, so containers and strings are
destroyed before its charge retires. The shared state keeps that reservation
through store destruction while any handle/read survives, and through failed
cleanup while retained records survive. Payload bytes and read buffers remain
separate from this metadata charge.

A ninth small test group covers admission lifetime after owner destruction,
idempotence, rejected owner replacement, denial without reclamation, retry,
late-admission rejection, failed-cleanup accounting and checked-size overflow.
Parent compilation and execution remain required for this newly added group;
earlier eight-group results do not establish its result.
