# Spill coordinator contract

> Export note: Recorded validation results are historical source-repository observations, not qualification of this standalone repository. Commands use the standalone layout.

`core/spill_coordinator.h/.cpp` provides an injected `MemoryAdmission`-backed,
single dedicated IO thread for immutable Tile cache eviction and final Tile
retirement. It does not acquire heavy-compute permits, read environment policy,
perform crash recovery, or claim physical disk guarantees.

## Integration

Create `SpillCoordinator(memory, limits)` before constructing spill-enabled
TileStore content. Register each successfully constructed dense tile using
`registerTile`. Connect `TileStore::setTileDeleterFactory` to
`coordinator.makeTileDeleter()`: the factory must run before allocating the raw
Tile. This reserves one fixed retirement slot. Exhaustion fails construction;
there is no caller-thread deletion fallback. An abandoned deleter returns its
slot. The final custom deleter stores the raw Tile into its preallocated slot
without allocating; the IO worker deletes it outside the coordinator mutex.

`reclaim(bytes, stop)` is a synchronous worker-only API. `requestReclaim(bytes)`
is asynchronous and returns false when closed/full. Both use a bounded request
queue. A separate issued-request bound includes active requests and completed
synchronous requests until their callers collect the result; waking an
unscheduled caller cannot permit unbounded completed allocations. The registry holds weak ownership, deduplicates owner identity, reuses
expired slots and rejects overflow. The separate explicit cleanup queue exposed
by `retire` is also bounded; rejected cleanup remains the caller's responsibility.
Callback captures must themselves be bounded; arbitrary callable heap ownership
is not measured by the coordinator's fixed bookkeeping allowance.

Reclamation calls atomically nonwaiting `Tile::trySpill`: a busy backing is
skipped, preventing a rehydrating worker's admission retry from deadlocking on
the IO worker. No coordinator mutex is held during tile IO or destruction.
Failure leaves the original resident payload published. Failure counts and the first exception are
reported, while canceled requests report cancellation. Successful eviction
keeps read-only resident leases usable and charged until their final release.

`droppedCachedBytes` describes removed cache ownership, including bytes still
held by leases. `releasedTrackedBytes` is the observed CPU ledger reduction;
concurrent unrelated allocations/releases can affect that observation. Admission
retry must call MemoryAdmission again, rather than trusting either diagnostic.
A single scan can skip busy or newly registered tiles; lack of progress must
return an ordinary admission failure rather than spin indefinitely.

## Lifetime and shutdown

`waitIdle()` drains queued/active work, not live Tile ownership. `shutdown()`
cancels queued reclamation and drains accepted cleanup and ready Tile retirement.
It is idempotent; concurrent shutdown calls are outside the API contract. API
methods must not be called from the IO worker (cleanup callbacks cannot reenter
synchronous methods). Kernel IO can delay cancellation and shutdown.

Issued deleters keep the worker endpoint alive independently of the coordinator
wrapper. If tickets remain at shutdown, the thread is detached and exits after
all tickets are abandoned or Tiles retired; it owns its state throughout that
interval. Otherwise shutdown joins it. Obtain `retirementDrain()` before wrapper
teardown when a caller needs a fence for escaped Tiles. Release all their Tile
owners/deleter copies, then call the fence. It waits for deletion and closed
worker exit. The fence itself retains the bookkeeping charge. Resident leases
retain only their resident bytes and may outlive this endpoint.

All vector capacities, per-ticket/request allowance and bookkeeping are admitted
before allocation. Ticket counts, registry size and pending queue sizes are
explicitly bounded by injected limits. Pending requests plus the active request
are included in the reservation. Caller-owned std::function captures and caller
thread stacks are not included. This is not whole-application resource
qualification.

## Tests and build

Add `core/spill_coordinator.cpp` to the engine target and link the existing typed
payload, SpillBacking, SpillStore, MemoryAdmission and `Threads::Threads`
dependencies. Register `tests/spill_coordinator_test.cpp` with its sole argument
an existing disk-backed build directory. Five small groups cover held leases, continued reclamation past a pinned first tile,
verified reload, cancellation, failure preservation, bounded queues and weak
registration, abandoned/exhausted retirement slots, idempotent shutdown, and
worker file cleanup after wrapper destruction. Fixtures contain two pixels;
no full-memory or swap-pressure test is performed here.

Tests are authored and manually reviewed. Compilation, execution and sanitizer
checks are reserved for the parent agent's serial combined verification.
