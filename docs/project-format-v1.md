# Native project format — schema 1

> Export note: Recorded validation results are historical source-repository observations, not qualification of this standalone repository. Commands use the standalone layout.

Normative M0 contract, 2026-09-20. Extension .cproj; SQLite application_id
1129337418 (0x4350524a, CPRJ), user_version=1, page size 4096. Normative table
definition: project-format-v1.sql. UUIDs are lower-case canonical 36-character
UUID text. Revisions are nonnegative signed 64-bit integers. All numbers finite.
SQL constraints do not replace semantic validation below.

## Records

Exactly one project row, id=1, format='compositor-linux', working space
'linear-srgb-extended-v1'. required_features_json=[] for baseline; reject unknown
required features/version. Canvas <=30,000 per side and <=100 MP. Resolution>0.

Layer sibling_order is contiguous zero-based bottom-to-top under each parent;
NULL parent denotes roots. Parent must be folder. Types: raster, paint, shape,
adjustment, folder. Raster/paint require a color asset; folders/adjustments do
not have one. Shapes use editable parameters, optional nonauthoritative caches.
Folder opacity=1, blend=normal. Blend identifiers are the 13 lower-case kebab-case
names in pixel-semantics.md. Visibility/linked/enabled are integer 0 or 1.
Names <=4096 UTF-8 bytes (reader additionally enforces bytes, not SQL characters).

Transforms contain 9 row-major little-endian binary64 numbers acting on column
[x,y,1]. Map local to document coordinates. Require finite invertible matrices;
normalize m22=1 when nonzero, reject condition number>1e12 and projective poles
within asset extent. Children store document transforms; do not multiply by the
parent again. Folder transform places its mask. Each mask has its own transform,
asset, enabled/linked bits and exterior coverage [0,1], default 1. Linked masks
follow layer edits; unlinked placement remains independent. Mask assets are r16f.
Sharing immutable mask assets is allowed; mask placement records are per layer.

Limits: 10,000 layers, hierarchy depth 64, clipping depth 256 edges. Clipping
endpoints must be raster/paint/shape; reject missing/self/cyclic dependencies.
Source visibility does not affect coverage. Aggregate distinct live color asset
extents <=100 MP and distinct mask extents <=100 MP, counting sparse extents
fully and shared assets once. Asset origins are signed 64-bit, but endpoint and
grid arithmetic must be checked for overflow before allocating or iterating.
All assets must be reachable from a layer/mask; reject orphan payloads in v1.

## Tiles and profiles

Asset format: rgba16f-le or r16f-le. default_value holds 8 or 2 canonical bytes.
Each tile is the intersection of its signed 256-grid cell with the asset extent;
width/height equal that intersection, stored top-left row-major without padding.
Reject duplicate coordinates, out-of-extent tiles and inconsistent dimensions.
Missing cells use default_value. Exterior mask coverage is separate from missing
interior tiles. Binary16 data obeys pixel-semantics.md.

Encoding constant stores one pixel; raw stores expanded bytes; zstd stores one
ordinary level-1 frame, no external dictionary, concatenated frames or trailing
bytes. Limit frame window to 1 MiB. decoded_size is expanded width*height*bpp,
including constant descriptors. Check before allocation/decompression. Payload
<=decoded_size+65536; constant length=bpp; raw length=decoded_size. Require exact
decoded length and SHA-256 over expanded canonical bytes, stored as 32 bytes.
Validate every canonical pixel before installing a document.

Profiles: <=16 records, <=4 MiB each, role source/display/output, SHA-256 of exact
ICC bytes. Asset source profiles are metadata; pixels always use working space.
Malformed ICC causes a useful error, never silent reinterpretation. Preview is
optional PNG <=4 MiB, <=1024 per side, <=1 MP; corrupt previews can be ignored.
Cap physical project size at 4 GiB. Canonical content round-trip is exact;
SQLite page arrangement and compressed bytes need not be identical.

## Parameters and compatibility

JSON is UTF-8, unique keys, finite numbers, maximum depth 32; aggregate layer
parameters plus required-feature text <=4 MiB. Envelope:
{"version":1,"kind":"raster","values":{}}. Raster/paint/folder use their type
as kind. Empty raster values retain the original bilinear default. M3 also
accepts raster values {"sampling":"nearest"} or {"sampling":"lanczos3"}; these
are pixel-affecting fields and older readers must reject rather than discard them.
Folder and reserved paint values remain empty. See the sampling decision record.
Shapes use kind rectangle/rounded-rectangle/ellipse,
values: bounds [x,y,w,h], fill straight encoded sRGB [r,g,b,a], radius>=0.
Adjustment kinds hue-saturation/levels/curves/exposure/gradient-map/grain reserve
version 1; M7 freezes their field validators before support is enabled. M2
explicitly rejects unsupported kinds. No loader may silently omit effects.
Unknown fields affecting pixels, required kinds and versions fail explicitly.
Selections, history, cache paths and framework/GPU handles are not serialized.

## Save, recovery and validation

Capture immutable revision and await tile futures on workers. Serialize saves
to each path. Create an exclusive sibling temporary file (0600 for new files;
preserve destination mode for replacement), journal_mode=DELETE,
synchronous=FULL, foreign_keys=ON. Write in one transaction, validate, commit,
close, fsync file, atomically rename, fsync directory. Only then mark that
captured revision saved. Newer revisions stay dirty. No in-place destination edits.

Compare opened device/inode/size/mtime and recorded identity before replacing;
changed/newly appearing destination prompts Replace/Save As/Cancel. Resolve
symlink targets explicitly with confirmation. Lock cooperating writers. POSIX
identity checks have a race with independent writers: do not promise global
cross-process serializability. Remote filesystem saves stage locally and do not
inherit a power-loss guarantee. Pre-rename failure preserves the old file;
post-rename directory-fsync failure reports uncertain durability (the new file
may be visible), retains dirty state and recovery data.

Recovery uses XDG_STATE_HOME/compositor-linux (default ~/.local/state), private
per-session stores and the same tile validation. Persist within 5 seconds after
last completed edit, maximum 30 seconds while continuously editing. Interrupted
strokes may be discarded; last durable completed revision must restore. Delete
recovery only after durable save or explicit discard. Cleanup uses owned session
records, never broad filename guesses.

Open untrusted projects read-only with query_only=ON, trusted_schema=OFF and
extension loading disabled. Accept ordinary declared tables/indexes only; reject
triggers, views and virtual tables. Never execute SQL supplied by a project.
Use bound queries, bounded SQLite resource limits and cancellable workers.
Validate schema, IDs, references, hierarchy, parameters and all size arithmetic
before publishing loaded content. Migrations create new in-memory snapshots;
opening never overwrites the source. Failure-injection implementation gates:
truncation, incorrect checksums/lengths, invalid zstd, allocation/disk/fsync/rename
failures, cancellation, process termination and pending GPU recovery copies.
