# Project streaming and tile residency

> Export note: Recorded validation results are historical source-repository observations, not qualification of this standalone repository. Commands use the standalone layout.

The Linux writer emits native format 3. It adds bounded selection metadata and linked layer masks while preserving version-specific appearance
constraints, little-endian RGBA16F canonical pixels, SHA-256 of the full expanded
canonical tile, and existing constant/zstd payload encoding. The reader accepts
formats 1, 2 and 3. Format 1 permits the original 13 blend identifiers and folder
opacity 1; format 2 also permits the appended modes and folder opacity. Format 3 stores selections and masks; see [format 3](project-format-v3.md). Unknown
versions fail before decoded tile allocation. The macOS `.comp` format is unchanged.

Save obtains one owning `Tile::read(stop)` lease per tile, copies canonical bytes
into one tile-sized buffer, and retires the lease before compression. A tile
that already has session spill backing is returned to disk-only cache residency
before encoding proceeds. Exceptions attempt the same cache retirement without
invalidating the immutable tile. Concurrent leases may independently retain their
resident bytes. Previously resident tiles remain available to pressure policy.
There is no whole-document canonical pixel copy.

`SaveOptions::memory` supplies the shared application admission. When supplied,
each tile reserves its expanded canonical size, twice its compression capacity
(including SQLite's `SQLITE_TRANSIENT` payload copy), the Zstd level-1 context
estimate, and 256 bytes for checksum/small encoding buffers. The token outlives
all these buffers and the prepared insertion statement. Legacy callers without
injected admission remain supported, but do not gain transient memory accounting.

Read uses `TileStore::memoryAdmission()`. It checks a SQL blob's size before
copying it, reserves its copied payload plus expanded canonical size, and covers
a compressed tile's decompression context and the enforced maximum 1 MiB window.
`TileStore` independently admits retained backing. With spill-enabled tiles, a
verified newly constructed tile is spilled before it enters the staged asset
map, so decoding many rows does not accumulate resident canonical payloads.
Failure discards the entire staged document and preserves the caller's document.

These reservations cover tile byte buffers and Zstd's published context estimates;
they are not an allocation-level accounting of Qt, SQLite, profiles, graph/map
nodes, allocator overhead, or filesystem page cache. Both SQLite connections use
a 4 MiB page-cache target and disable mapping. Existing graph/profile/count/file
limits remain in force. Session spill is not crash recovery.

Cancellation is checked at each row, during lease acquisition/canonical copying,
and before commit/replacement. Zstd and SQLite blob copies do not provide a
250 ms cancellation guarantee. `SaveStage::Tile` is a test checkpoint after an
insertion, enabling mid-stream rollback/cancellation fixtures. Atomic sibling
replacement, identity checks, fsync, and post-rename uncertainty retain their
previous behavior.

`project_store_test` now requires an injected absolute disk-backed directory
argument. Fixtures add memory-admission denial, scratch retirement, per-row
fault/cancellation, three small disk-backed tiles, exact save/reopen bytes,
format-1 migration, and format-2 appearance metadata. Maximum new disk payload
is 16 KiB. Compilation and execution are owned by the parent integration agent.
