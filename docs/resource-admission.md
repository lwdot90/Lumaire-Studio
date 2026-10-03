# Runtime resource admission

> Export note: Recorded validation results are historical source-repository observations, not qualification of this standalone repository. Commands use the standalone layout.

This is the integration contract for the current low-resource foundation, not a completed LP8 qualification. The [release specification](low-resource-compatibility.md) retains its complete tool/quality, memory and workflow gates.

## Ownership and scheduling

`defaultRuntimeResources()` supplies one process-wide context. Default windows share its canonical `TileStore`, memory ledger and compute scheduler; tabs and retained history consume that same tile allowance. An explicitly injected context is independently owned, principally for tests. Selecting a profile allocates no cache or pixel pool in advance.

Machines with no more than 8 GiB detected physical RAM select two compute slots (or one on a single/unknown-thread machine), one memory-heavy job, a canonical tile ceiling no larger than 768 MiB or RAM/8, CPU mip ceiling 8 MiB per canvas, thumbnail mip ceiling 4 MiB, GPU tile pool 8 MiB, mip pool 16 MiB and linear frame ceiling 64 MiB. The application envelope is at most 2 GiB or RAM/2 with 512 MiB system headroom. Unknown inventory uses a conservative 4 GiB profile. Smaller machines still require separate qualification.

Compute permits are move-only RAII objects. Queued interactive CPU frames precede eligible processing jobs and background thumbnails; an ineligible memory-heavy waiter cannot waste the spare slot. Acquisition happens off the UI thread, supports stop tokens and checks supersession within a bounded polling interval. Already-running jobs retain cooperative cancellation. Vulkan queue recording, hidden codec pools and later inference library threading need separate qualification.

## Memory reservations

`MemoryAdmission::reserve(bytes, kind, failure)` returns a move-only reservation or a named refusal. `require()` throws the same refusal as a length error. A serialized injected/default probe supplies current `MemAvailable` and process RSS; missing/malformed data denies admission. Pending reservations compete across all consumers rather than giving each job an independent envelope.

Reserve before allocating. Commit after backing has been materialized, and keep the charge until backing is freed. Declare reservations before their backing or explicitly destroy backing first. Failed construction, cancellation and stale requests unwind charges automatically. A reservation can outlive its admission owner because it retains the ledger state. Probes must not reenter their admission object.

Application admission checks `RSS + committed CPU + committed GPU + pending + requested <= application envelope`. System admission checks `committed CPU + committed GPU + pending + requested + headroom <= MemAvailable`. Arithmetic is checked for overflow. Procfs cannot prove that a tracked allocation is resident: unrelated toolkit RSS must not hide swapped tiles. These equations intentionally count resident tracked CPU bytes twice and reserve headroom for all tracked backing becoming resident. They can refuse work earlier than an allocation-level residency model.

The ledger alone is not an exact measurement of all physical memory. RSS includes uninstrumented toolkit/allocator memory, and another process can change headroom immediately after a sample. Qualification must record actual process-tree/GPU/system peaks and overlap rather than interpreting a configured cap as a passed benchmark.

## Integrated allocation boundaries

| Boundary | Current behavior |
| --- | --- |
| Canonical tiles/history | Reserve immutable object/payload before construction; commit after materialization and retain through the last document/history/worker reference. Local tile limits remain an additional gate. Failed transactions cannot install partial output. |
| CPU frames | Reserve viewport RGB32 bytes separately from an initial 4 MiB planning/filter-scratch allowance. Shared QImage pixel backing retains its committed image charge through queued callbacks and display copies until final release. Old source mips are pruned; hidden canvases cancel work and clear derived cache/display references. |
| CPU mips | Reserve each piece and bounded metadata; evict this worker's disposable derived cache and retry on denial. Parent snapshots and canonical pixels remain unchanged. |
| Thumbnails/jobs | Shared compute admission plus initial transient allowances (2 MiB per thumbnail, 16 MiB for graph/native-IO/fill jobs). Thumbnail QImage pixel backing retains a separate charge through cache/result/shared-image copies. Document-free suspension cancels queued work and clears caches on the worker; final-tab closure clears row icons. These allowances require measured qualification. |
| Image import | Reserve frozen encoded size before its bounded read and worst-case decoded backing before decoding. Retire encoded/decoder state before canonical tile accumulation; convert at tile size. Initial 64 MiB metadata/ICC/codec/conversion allowance remains unqualified. Qt still decodes a full image. |
| Vulkan buffers | Reserve actual driver-reported allocation size before `vkAllocateMemory`; retain all source/staging/frame/mip/scratch/table charges until queue-safe `vkFreeMemory`. Host/device buffer overlap with RSS is conservatively additional. |

## Remaining integration

Shared QImage copies keep one pixel-backing charge. Consumers that create a detached/deep copy or converted image need a separate charge; the CPU canvas consumes shared images, while thumbnail QPixmap/icon conversion remains a bounded visible-row toolkit allocation requiring separate instrumentation. The wrapper is not a general hook into every Qt allocation.

Canonical tiles and retained undo snapshots now share immutable spill backing. RuntimeResources supplies verified XDG disk storage, a bounded one-worker spill coordinator, admitted metadata and read leases, pressure reclamation, and deferred final tile retirement. Cache eviction releases only unpinned resident charges; rehydration verifies CRC before use. Native save/load admits and processes one tile at a time. Disk mode charges TileBudget for tile metadata while MemoryAdmission bounds resident payloads inside the total envelope; the canonical pool is not an additional resident-payload ceiling. Total GPU image/swapchain/driver allocation, all codec/ICC/SQLite/font/toolkit transients, child inference processes, sustained swap/thermal behavior and real desktop coexistence remain to be measured/instrumented. Resource refusal preserves input and reports an error; general pressure-driven pause/retry/progress remains later work; allocation admission can synchronously request coordinated bounded eviction and then retry once. Broader native precision/schema extensions remain pending; schema 2 currently adds only the new blend modes and inherited folder opacity.

The [immutable backing adapter](spill-backing-contract.md), [legacy `rgba16f-le` payload bridge](spill-tile-payload-contract.md) and [injected directory policy](spill-directory-contract.md) are implemented as independent components. They provide admitted read leases, spill-before-evict publication, verified rehydration, immutable dense/constant metadata and exact canonical validation. Existing Tile now records dense/constant identity explicitly. These components are now connected to live Tile/history storage through RuntimeResources and SpillCoordinator; current qualification uses small fixtures rather than full LP8 workloads.

CPU cached readers, mip level-zero reads, reference sampling and GPU staging now hold resident leases throughout their reads. Exact no-op comparisons remain in transaction replacement; unchanged replacements normalize to the original Tile pointer. Transaction finish now compares coordinates and immutable pointer identity, avoiding a second pixel scan during GUI publication. Lease-based replacement runs on workers so comparisons do not initiate disk IO on the UI thread. Final Tile/backing destruction uses IO-worker retirement tickets, including history trimming and tab closure. Preserve current logical history-depth semantics separately from resident-byte accounting. A worker holding the sole heavy-job permit must not wait for reclamation that requires another heavy permit.

Current headless integration checks cover stable identity, partial/negative tile grids, exact canonical samples, snapshot/history sharing, active leases through eviction, concurrent loads, denied/corrupt rehydration, canceled/failed writes and final charge release. Application pressure reclamation is connected; real workload peak measurements remain pending. Session handles do not change the native file schema. Native save/load now streams individual admitted tiles; recovery, arbitrary image decoding/export streaming and later typed-source precision remain independent dependencies.

Do not reduce sample precision, discard a sole unsaved source, trim required editability or silently flatten a document to satisfy admission. A component correctness pass does not close any full LP/P/C/W release gate.
