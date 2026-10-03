# Native project format — version 2

> Export note: Recorded validation results are historical source-repository observations, not qualification of this standalone repository. Commands use the standalone layout.

Version 2 is the Linux `.cproj` appearance extension for M3. SQLite
`application_id` remains 1129337418 (0x4350524a, CPRJ); `user_version` is 2,
with 4096-byte pages. The normative authored table definitions are
[project-format-v2.sql](project-format-v2.sql). The column layout is inherited
from [version 1](project-format-v1.md), with version-specific layer constraints
for the extended blend identifiers and folder opacity. The macOS `.comp` format
is unchanged.

## Appearance identifiers

Raster blend identifiers retain their original ordering and spelling. These
24 identifiers are stable; numeric positions below describe the runtime enum,
while the database stores the textual identifier.

| Position | Identifier | Introduced |
| --- | --- | --- |
| 0 | `normal` | 1 |
| 1 | `multiply` | 1 |
| 2 | `screen` | 1 |
| 3 | `overlay` | 1 |
| 4 | `darken` | 1 |
| 5 | `lighten` | 1 |
| 6 | `difference` | 1 |
| 7 | `color-dodge` | 1 |
| 8 | `color-burn` | 1 |
| 9 | `hue` | 1 |
| 10 | `saturation` | 1 |
| 11 | `color` | 1 |
| 12 | `luminosity` | 1 |
| 13 | `linear-burn` | 2 |
| 14 | `linear-dodge` | 2 |
| 15 | `soft-light` | 2 |
| 16 | `hard-light` | 2 |
| 17 | `vivid-light` | 2 |
| 18 | `linear-light` | 2 |
| 19 | `pin-light` | 2 |
| 20 | `hard-mix` | 2 |
| 21 | `exclusion` | 2 |
| 22 | `subtract` | 2 |
| 23 | `divide` | 2 |

The [blend contract](blend-24-contract.md) defines scalar equations and edge
behavior. The [GPU contract](gpu-blend-24-contract.md) records shader agreement.
Extended non-overlap colors remain FP32 premultiplied linear values; artistic
overlap uses the bounded encoded-sRGB policy. Adding identifiers does not change
working-space or canonical tile encoding.

Folders remain pass-through: no raster asset and blend `normal`. Version 2
allows opacity in [0,1]. Each descendant raster's effective opacity is its own
opacity multiplied by every ancestor folder's opacity. Effective visibility is
the conjunction of its own visibility and all ancestor folders' visibility.
Multiply all four premultiplied raster channels by effective opacity before
blending in bottom-to-top order. A folder is not rendered as an isolated
intermediate image. Its children keep their own document-space transforms;
parent transforms are not multiplied into them. See the
[folder opacity contract](folder-opacity-contract.md).

## Version 1 compatibility

The reader accepts authored versions 1 and 2 and rejects other versions. Each
version must match its corresponding authored table/index definitions; version
checking is separate from schema-object comparison. Views, triggers, virtual
tables, unknown DDL, and unknown required features remain rejected. A project
cannot bypass version-1 appearance restrictions by changing its version number.

Version 1 permits only positions 0–12 and folder opacity exactly 1. Its pixels,
identities, revisions, transforms, profiles, sampling parameters, layer order,
and shared asset relationships load unchanged. Opening creates an in-memory
snapshot and does not overwrite the source. A successful subsequent save emits
version 2 through sibling-file atomic replacement. An older version-1-only
reader must reject version 2 rather than discard appearance information.

## Pixels, bounds, and enabled support

Canonical working color remains `rgba16f-le`: four little-endian binary16
premultiplied linear-sRGB channels, eight bytes per pixel, retaining the
[existing pixel validation](pixel-semantics.md). Tile coordinates remain signed
256-grid coordinates; edge dimensions equal the intersection with the asset
extent. Missing tiles use the canonical asset default. Constant/raw/zstd payload
formats, expanded decoded lengths, SHA-256 of the expanded canonical tile,
1 MiB Zstd window limit, and exact canonical round-trips are unchanged.

Existing limits remain: 30,000 pixels per canvas side, 100 MP canvas and
aggregate distinct color asset extents, 10,000 layers, hierarchy depth 64,
4 GiB physical project size, 16 source profiles of at most 4 MiB each, and
4 MiB aggregate layer parameter text. Source profiles are metadata, with
validated ICC bytes and exact checksums; pixels use the recorded working space.
Finite transforms, UUIDs, references, canonical pixels, dimensions, checksum,
encoding, and size arithmetic are validated before publication.

The current reader enables raster layers, folders, source ICC metadata, and
nearest/bilinear/Lanczos sampling parameters. Paint, shapes, adjustments, masks,
clipping, other profile roles, and pixel-affecting unknown parameters still fail
explicitly. Their reserved table fields do not imply implemented support.
Version 2 does not introduce typed mask, shape, effect, or new color formats;
[typed rendering interfaces](render-source-contract.md) are runtime contracts.

[Project streaming](project-streaming-contract.md) admits one tile's temporary
canonical/encoding bytes at a time and uses owning read leases. Session spill
paths, caches, leases, GPU handles, history, and recovery indexes are not saved.
[Spill storage](spill-store-contract.md) and
[pressure coordination](spill-coordinator-contract.md) do not change this format
or provide project crash recovery. Save identity checks, transaction rollback,
file/directory fsync, atomic replacement, and post-rename uncertain-durability
reporting retain the previous contract.
