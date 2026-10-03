# Immutable opaque spill backing

> Export note: Recorded validation results are historical source-repository observations, not qualification of this standalone repository. Commands use the standalone layout.

This adapter combines the existing [SpillStore](spill-store-contract.md) and
`core/MemoryAdmission` for one immutable byte payload. It interprets no samples,
changes no precision, and has no dependency on Tile, history, rendering, UI or
the native project format. Operations run synchronously on the caller's worker.

## API and ownership

The API in `io/spill_backing.h` belongs to `compositor::io`:

| API | Behavior |
| --- | --- |
| `SpillBacking::create(bytes, memory, store, stop, fault)` | Admit and copy exact caller-owned bytes into immutable resident backing. Memory admission and store are required. Empty payloads are valid. |
| `SpillBacking::fromSpill(handle, memory, stop, fault)` | Retain an existing nonempty handle without reading its bytes. Does not consume the input handle. Integrity is checked on rehydration. No store wrapper is needed. |
| `resident(stop)` | Return a copyable `ResidentLease` exposing only `span<const uint8_t>`. Rehydrate on a cache miss, using `readInto()` on an admitted private buffer. |
| `spill(stop)` | Write on this calling worker if no disk copy exists, publish the successful handle, then drop the resident cache. Return true if a cached copy was dropped, false if already disk-only. Later evictions reuse the immutable disk handle. |
| `spillHandle()` | Copy the currently published disk handle; empty until the first successful spill. A returned handle independently retains the file. |
| `size()` / `status()` | Original immutable byte count and a mutex-protected resident/spilled/busy/cache-charge snapshot. |
| `stateCharge()` / `residentCharge(size)` | Charge formulas for planning and tests; allocations perform their own admission. |
| `ResidentLease::bytes()` / `size()` / `chargedBytes()` | Read-only view, exact payload length and its retained resident charge. Default leases are false and expose an empty span. |

Backing copies share one state, resident cache and disk handle. Different copies
may be used concurrently. Destroying or assigning the same wrapper while
another thread reads that wrapper requires caller synchronization. Each method
pins its state for its full duration. A default backing is false; resident/spill
operations on it fail with `SpillErrorCode::InvalidArgument`.

The source span supplied to `create()` must stay alive and unchanged until the
factory returns. It is not retained afterward. A failed factory does not alter
that source or consume the handle supplied to `fromSpill()`.

A lease owns its resident block, including its committed reservation, rather
than borrowing the backing's cache. It remains valid after spill, cache
eviction, backing destruction, admission-owner destruction, or destruction of
the caller's store owner. A lease copy shares the same allocation and charge.
Keep the lease alive for the entire use of a span obtained from it. Resident
leases need not retain the disk file once no backing or SpillHandle needs it.
Backing copies and exported SpillHandles do retain disk lifetime independently.

## Memory and lifetime guarantees

Every state allocation first requires a CPU reservation for `sizeof(State) +
64` bytes. Every resident allocation first requires a separate CPU reservation
for `payload size + sizeof(ResidentBlock) + 64` bytes, with checked length and
addition. The 64-byte allowances cover estimated shared-pointer control
metadata; they are not a measurement of allocator/closure/platform overhead.
The existing admission policy also conservatively accounts measured RSS.
No mutable input allocation is adopted or silently exempted from admission.

Resident storage is a fixed-size byte array, without vector spare capacity.
Reservations commit only after their allocations materialize. Each reservation
is declared before its backing members, so destruction releases those bytes
before releasing their charge. Refusal or failed construction unwinds pending
charges. Failed reads/cancellation unwind both materialized buffers and their
charges before allowing another cache-miss transition to proceed.

State metadata stays charged while backing copies/operations need it, including
when disk-only. Resident charges stay live until the cache and every lease
release their block. If an old lease survives a spill, a new rehydration may
need a second full resident allocation; both are admitted and charged. Refusal
keeps disk backing and all old leases valid. Cache hits require no new payload
allocation or admission. `status().residentCharge` describes the current cache,
not all older leased blocks; use the shared MemoryAdmission ledger for totals.
Do not double-charge these allocations in an integration adapter.

Caller input, caller containers/handle copies, callback captures, filesystem
page cache and uninstrumented library/platform overhead need the parent's
broader memory policy and measurement. This is not a whole-application RAM
measurement or scheduler.

## Publication, concurrency and failure

At most one spill or cache-miss rehydration transitions a particular backing at
a time. Waiters use a stop-aware condition variable. A cache hit may acquire a
lease while a spill is writing; the writer pins those original resident bytes.
Other transitions wait and recheck state, so concurrent successful spills do
not create duplicate disk handles and concurrent misses do not allocate or
publish duplicate resident caches. No state mutex is held across admission,
byte copying, filesystem IO or fault callbacks.

Initial copying checks cancellation between 64 KiB chunks. Spill publication
happens only after `put()` returns successfully and a final adapter cancellation
check passes. The disk handle is installed before dropping the cached block.
Existing leases continue owning that block. Rehydration allocates admitted
memory and calls `readInto()`; only complete successful integrity verification
and the final cancellation check permit cache publication or lease return.
Unverified bytes never escape as a lease.

Cancellation, admission refusal, allocation failure, injected failure, disk-full
or corruption leaves the operation's previous published backing intact and
releases temporary resources. Another concurrent successful operation may
legitimately change cache residency. A failed disk read retains its existing
disk handle for diagnosis/retry; it cannot recover corrupt bytes by itself.
Errors from SpillStore propagate, adapter cancellation uses `SpillError` with
`Cancelled`, memory refusal is the existing `std::length_error`, and allocation
failure is `std::bad_alloc` (or the allocator's related exception).

The optional `SpillBackingFault(stage, size)` test callback runs after resident
admission/before allocation, before verified resident publication, or before
spill-handle publication. It may throw (including `bad_alloc`) or request stop.
It must be thread-safe, must not reenter residency/transition operations on this
backing, and its captures must outlive every backing using it. It may inspect
the separate admission ledger. It is empty in production.

Existing SpillStore guarantees/limits remain authoritative. Reusing a previously
published disk handle assumes that private immutable spill storage has not been
modified externally; eviction does not reread that file. Kernel IO/admission
probes cannot be forcibly stopped, and a stop racing after the final check may
observe success. No 250 ms cancellation deadline is claimed. Final releases
may perform spill cleanup on the releasing thread; the parent should retire
disk-backed owners on an appropriate worker.

## Parent build and integration handoff

Only these four new files are owned by this task:

- `io/spill_backing.h`
- `io/spill_backing.cpp`
- `tests/spill_backing_test.cpp`
- `docs/spill-backing-contract.md`

The parent must compile `io/spill_backing.cpp`, link it with `io/spill_store.cpp`,
`core/memory_admission.cpp` and `Threads::Threads`, and expose `linux/` as an
include root. Existing targets currently place SpillStore in `compositor_spill`
and MemoryAdmission in `compositor_engine`. A separate backing target can link
those targets; its source has no Tile/document/render dependency. An alternative
is a small shared memory-admission library arranged by the integration owner.
Do not create a dependency cycle by making the engine consume a backing target
which also links that engine. Register the standalone test with an explicit
absolute disk-backed fixture directory and a finite timeout.

Authored on 2026-10-02. The author supplied the implementation and eight test
groups without running builds, leaving serial execution to the parent. The
parent has now registered `compositor_spill_backing`/`spill_backing_test` and
passed all eight groups in GCC debug, Clang ASan/UBSan with leak detection and
Clang TSan. All three runs exited 0 with no sanitizer or race report. The memory
admission target is separate from the engine to avoid a future storage cycle.

The test groups cover:

- Exact opaque/empty/boundary bytes, immutable input copying, read-only lease
  type, cache hits, charge sharing and reused disk handles.
- Backing copies, leases and exported handles surviving original owners;
  disk-only imports after the store wrapper is destroyed.
- Refusal before state/resident allocation, refusal while an old lease remains
  charged, missing/probe/application-envelope failure and recovery.
- Injected `bad_alloc` before allocation and before resident/spill publication,
  checking pending/committed rollback and previous backing.
- Pre-cancellation, cancellation during write/read, and cancellation after put
  success or full read verification but before adapter publication.
- EIO/ENOSPC/EDQUOT during write/sync/close, corrupt header/payload reads, and
  combined disk/cleanup failure preserving the sole resident copy.
- Four small concurrent callers, single disk/resident publication and mixed
  spill/read transitions retaining exact bytes and charges.
- Cached reads during a blocked spill, queued cancellation independent of
  blocked rehydration, and active job/lease lifetime after owners are released.

Largest fixture payload is 8193 bytes. Tests create only uniquely owned fixture
trees under the supplied disk directory, use existing filesystems and simulated
probes/errors, and do not stress RAM/disk or change mounts/desktop settings.

Proposed standalone parent commands below have **not been run**. Each compiler
driver builds its sources sequentially; do not launch these builds concurrently.
These commands avoid pulling Tile/document/render into the standalone test.

```sh
mkdir -p build/spill-agent
g++ -std=c++20 -O1 -g -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror -pthread -I. core/memory_admission.cpp io/spill_store.cpp io/spill_backing.cpp tests/spill_backing_test.cpp -o build/spill-agent/spill_backing_test
build/spill-agent/spill_backing_test ./build/spill-agent
```

For sanitizer verification, the parent can repeat that single compiler command
with Clang, adding `-fsanitize=address,undefined -fno-omit-frame-pointer`, then
separately `-fsanitize=thread -fno-omit-frame-pointer`, using distinct output
names. Enable leak checking where the execution environment supports it; the
earlier SpillStore LeakSanitizer run needed execution outside ptrace tracing.
For CTest, use `add_test(NAME spill_backing COMMAND spill_backing_test
"${CMAKE_CURRENT_BINARY_DIR}")` only when that directory is disk-backed, and
set a timeout such as 30 seconds. The parent owns all CMake edits and execution.

The parent owns worker dispatch, aggregate limits, immutable tile/history
records, streaming save and recovery. Supply the shared MemoryAdmission and a
verified disk-backed SpillStore, retain backing/lease ownership across jobs, and
install reconstructed objects only after lease acquisition succeeds. There is
no automatic eviction, compression, crash recovery, pressure retry, or document
wiring here. This component alone does not establish complete low-resource
compatibility; LP01–LP05/P17–P21 remain separate application qualification.
