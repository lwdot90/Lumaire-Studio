# Legacy typed spill tile payload

> Export note: Recorded validation results are historical source-repository observations, not qualification of this standalone repository. Commands use the standalone layout.

`core/spill_tile_payload.*` adds a headless typed adapter above
[opaque SpillBacking](spill-backing-contract.md). This prerequisite preserves the
existing legacy `rgba16f-le` working-pixel contract. It does not reconstruct or
change Tile, TileStore, RasterSnapshot, history, rendering or native projects,
and it does not enable application eviction or establish LP8 qualification.

## Typed bytes and API

`engine::SpillTilePayload` has immutable width/height (each 1–256) and an explicit
`SpillTileKind::Dense` or `Constant` tag. Dense stores exactly width × height × 8
bytes, in row-major RGBA binary16 little-endian order. Constant stores exactly
one eight-byte PackedPixel for its entire extent. Classification never depends
on whether the resident cache is empty, nor on whether dense samples are equal.
Both representations use the existing extended linear-sRGB, premultiplied-alpha
working semantics. They are not native RGB16/FP32/CMYK source payloads.

Every stored PackedPixel is checked with the existing `validateCanonical` rules:
finite samples, valid alpha, canonical zero and transparent RGB. Factories first
check tag/dimensions/exact byte count, admit metadata and opaque storage, then
validate the private immutable resident bytes before returning a typed object.
`fromSpill()` eagerly performs an admitted, integrity-verified read and canonical
validation. Later `resident()` calls retain those validated immutable bytes;
cache hits do not rescan canonical samples. Rehydration verifies the original
opaque CRC-64 and file identity before publishing a resident block, restoring
that validated byte sequence under the private, immutable session-storage
contract. CRC-64 detects accidental corruption; it is not authentication against
an attacker who can replace payload and checksum. Runtime storage must remain
private, and callers must never mutate resident spans through casts. This avoids
repeating a full-tile scan for every pixel or layer read. Factory validation
reads one eight-byte scalar at a time and allocates no full decoded pixel array.
No byte buffer is reinterpreted as an aligned native-endian PackedPixel array.

| API | Contract |
| --- | --- |
| `create(width, height, kind, bytes, memory, store, stop, fault)` | Exact immutable caller bytes, shared admission and a writable session store are required. Caller input remains unchanged/alive until return and is not retained afterward. |
| `fromSpill(width, height, kind, handle, memory, stop, fault)` | Retain an exact-sized live handle, read and validate before returning. Caller handle is not consumed. No store wrapper is required. |
| `width()` / `height()` / `kind()` / `size()` | Immutable descriptor and compact stored byte count. Empty width/height/size are zero; empty kind throws. |
| `sameIdentity(other)` | True only for copies sharing one nonempty typed state. Independent factories have separate identities even with equal bytes. Spill/rehydration does not change identity. |
| `resident(stop)` | A lease of the factory-validated immutable payload; cache misses first verify opaque integrity. Shares resident allocation and charge without repeating a canonical scan. |
| `spill(stop)` | Delegate caller-worker spill-before-evict. Failures preserve existing published backing; no automatic eviction policy is supplied. |
| `spillHandle()` / `status()` | Existing opaque handle/cache diagnostics. Cache charge excludes older blocks retained only by leases. |
| `metadataCharge()` | Typed metadata/control-block admission formula, separate from opaque state/resident charges. |

The spill file remains an opaque compact payload: it carries no tile dimensions,
placement or type header. The caller must retain and correctly pair the trusted
width/height/kind descriptor with its handle. Validation cannot distinguish
different descriptors whose expected byte lengths and canonical samples both
match. Raw handles must not replace trusted immutable tile descriptors. The
fixed encoding identifier is `rgba16f-le`; other precision/color representations
need separate typed contracts.

## Reader lease

`SpillTileReadLease` wraps an opaque `ResidentLease` and copies the small immutable
descriptor into the lease. Copies share one resident allocation/charge. A lease
remains valid after spill, typed/backing owner destruction or destruction of the
caller store/admission owner. Keep it alive for every use of `storedBytes()`.

- `pixel(x, y)` returns an exact PackedPixel at local coordinates; invalid/local
  out-of-bounds coordinates throw. `linearPixel(x, y)` applies existing unpack.
- `storedBytes()` exposes only a const span: width × height × 8 for Dense, eight
  bytes for Constant. It is the compact stored representation.
- `canonicalByteCount()` is width × height × 8 for either representation.
  `copyCanonicalBytes(destination, stop)` requires that exact size and expands
  Constant into full row-major canonical bytes. It allocates nothing. The caller
  must admit/own the destination and must not alias immutable resident storage.
  Discard partially written destination bytes on cancellation. This full
  representation matches existing Tile canonical-byte serialization.
- `chargedResidentBytes()` reports the one opaque block retained by that lease.

Tile placement is separate caller metadata, including negative raster origins;
this adapter has no TileCoord/global-position API. It never clips or transforms
local samples, narrows precision or substitutes transparent pixels on failure.

## Memory, failure and concurrency

Before any typed metadata allocation, reserve `sizeof(TypedState) + 64` CPU bytes.
The reservation commits after construction and lives until the last typed owner
or operation releases that state. Backing members retire before this metadata
reservation. The 64-byte control-block allowance is an initial estimate, not an
allocator/platform measurement. Opaque state/resident allocations already own
their separate admission reservations and are not charged again here. Validation
uses constant-size stack scratch; caller buffers, input and containers remain
caller-owned accounting. Metadata copied into a stack/caller lease is not a new
heap state allocation.

Opaque single-flight transition, stop-aware waiting, admitted rehydration,
spill-before-evict and rollback guarantees remain authoritative. Resident blocks
stay charged while caches or leases retain them. If an older lease survives
spill, rehydration can allocate another independently admitted block; both charges
remain live. The typed adapter adds no lock, worker queue, pressure retry or IO
thread. Factories, cache-miss resident calls and spill run on the caller's worker.
Different copies are safe for concurrent operations. Mutating/destroying the
same wrapper concurrently requires caller synchronization.

Invalid descriptor/length/admission arguments use `std::invalid_argument`,
invalid canonical samples use existing `std::domain_error`, empty payload
operations use `std::logic_error` and invalid lease coordinates use
`std::out_of_range`. Opaque SpillError, admission `std::length_error` and allocation
failures propagate. Cancellation uses SpillError/Cancelled; validation checks it
before/after the loop and every 256 stored samples. Canonical copying also checks
between bounded chunks. No hard cancellation deadline is promised.

Failures release unpublished typed/opaque allocations and charges. A failed
factory consumes no caller handle and returns no typed object. A failed read
returns no typed lease; prior leases remain immutable and valid. Final owners may
release a spill handle and perform cleanup IO on the releasing thread: runtime
wiring must arrange appropriate worker retirement.

## Build handoff and next wiring

Owned files are the new header/implementation, `tests/spill_tile_payload_test.cpp`
and this contract only. No builds or tests were run by this agent; the root agent
serializes all compilation on the 8 GB machine. No runtime or sanitizer pass is
claimed. The six test groups cover local/partial/edge dimensions, extended and
subnormal exact samples, constants/full expansion, explicit tags, every canonical
rejection at the final dense sample, eager disk validation, identity/copies,
leases after owner destruction, denied metadata/resident allocations, corrupt
reads, allocation/write/publication failure, cancellation, charge cleanup and
four bounded concurrent callers. Compact payloads are at most 2048 bytes; one
constant canonical expansion uses an admitted 512 KiB caller buffer. No RAM/disk
stress or desktop/mount change is performed.

Root CMake can add a separate typed-payload library linking PUBLIC
`compositor_pixels` and `compositor_spill_backing`. This avoids an engine cycle;
the adapter has no Tile/document/render dependency. Register its test with an
absolute existing disk-backed fixture directory and a finite timeout, e.g. 30 s.
Proposed standalone commands have not been run:

```sh
g++ -std=c++20 -O1 -g -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror -pthread -I. core/pixels.cpp core/memory_admission.cpp io/spill_store.cpp io/spill_backing.cpp core/spill_tile_payload.cpp tests/spill_tile_payload_test.cpp -o build/spill-agent/spill_tile_payload_test
build/spill-agent/spill_tile_payload_test ./build/spill-agent
```

Future integration still needs stable Tile payload ownership and resident leases,
separate metadata/resident TileBudget charges, bounded worker read contexts for
CPU/thumbnail/mips and GPU upload, exact transaction comparison outside UI IO,
worker cleanup and disk/eviction policy. Do not reconstruct a new Tile merely to
rehydrate this payload: preserve the Tile object/version used by existing cache
identity. Save can later lease/copy canonical bytes into its existing encoding;
no native schema changes or disk references are introduced by this component.
Crash recovery, source precision migration and LP01–LP05 qualification are
separate work.

## Parent integration validation — 2026-10-02

Registered in CMake and run by the coordinator with one compiler/test job. The
combined GCC application suite passed 21/21, independent core suite 13/13,
Clang ASan/UBSan with leak detection 21/21, and targeted Clang TSan suite 10/10.
This component passed in each configuration. Agent authorship/manual review
above is distinct from these parent execution results. Commands and source
snapshot are in combined qualification (historical source-repository record; not included).
Application eviction and low-resource release qualification remain pending.
