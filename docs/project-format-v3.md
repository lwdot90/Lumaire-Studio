# Native project format — version 3

> Export note: Recorded validation results are historical source-repository observations, not qualification of this standalone repository. Commands use the standalone layout.

Linux `.cproj` version 3 retains application ID 1129337418 and the version-2
blend/folder policies. Its normative schema is
[project-format-v3.sql](project-format-v3.sql). New fields represent a bounded
marquee selection and linked raster-layer coverage masks. The macOS `.comp`
format is unchanged. This is a practical subset of upstream selection/mask
behavior, not arbitrary paths, feathering, independent mask placement, or
folder masks.

## Selection

`project.selection_json` is non-null text, at most 512 UTF-8 bytes. `null` means
no selection. Otherwise the version-1 selection envelope has exactly these keys
and canonical order/spelling:

```
{"version":1,"shape":"rectangle","bounds":[0,0,64,32],"inverted":false}
```

`shape` is `rectangle` or `ellipse`. Bounds are four signed decimal integers
`[x,y,width,height]` in document pixels. Bounds must remain within the canvas;
model validation rejects coordinates outside it. Zero width or height
represents an explicit empty selection, distinct from `null`; negative dimensions
or arithmetic outside the canvas are rejected.
`inverted` is a JSON Boolean, never a number. No duplicate/unknown keys,
whitespace variants, nonintegral bounds, nonfinite numbers, trailing input,
unknown shapes/versions, or alternative value types are accepted. The writer
uses this canonical representation; opening never rewrites the source.

## Linked raster masks

A raster layer's optional `mask_uuid` references a masks record. `masks.uuid` identifies one layer's placement/enable record; the writer uses
that layer's UUID. `masks.asset_uuid` identifies its immutable coverage raster.
Different layers can share a coverage asset while retaining distinct placement
records, transforms, and enabled states.
`enabled` is 0 or 1; disabled masks remain saved and editable. `linked` must be
1, the decoded mask transform must equal the layer's document transform, and
`exterior_coverage` must be 1. Masks have exactly the same local extent and grid
as their color raster. A folder mask, independent placement, unlinked mask,
clipping dependency, mismatched extent/transform, or orphan record is rejected.
Sharing immutable mask assets is permitted; placement records are per layer
and cannot be shared. Conflicting versions of one coverage asset ID fail explicitly.

The asset format is `mask-rgba16f-le`: eight little-endian canonical bytes per
pixel, RGB exactly zero and alpha in [0,1] holding coverage. The RGB channels
are storage compatibility fields, not color. Mask default values obey the same
rule. Masks have no source ICC profile. Canonical negative zero, nonfinite
values, invalid alpha, or nonzero mask RGB fail before document publication.
Missing tiles use mask default coverage. Color and mask extents have separate
100 MP aggregate limits, counting each distinct asset once.

Existing constant/raw/zstd encodings, full expanded SHA-256 checksums, decoded
length checks, dimensions, frame-window cap, tile grid, and spill/read lease
admission apply equally to mask tiles. This representation uses existing
RGBA16F Tile infrastructure; it does not claim a compact R16F runtime mask or a
new color space. The reserved `r16f-le` format remains unsupported.

## Migration and persistence

The reader accepts exact authored schema versions 1, 2, and 3. Version 1 retains
its original 13 blends and opacity-1 folders. Version 2 retains all 24 blend IDs
and inherited folder opacity. Both older versions load with no document
selection or layer mask. Their pixel/metadata interpretation is unchanged;
a subsequent successful save emits version 3. Unknown versions or DDL variants
fail rather than lose data.

Selection and mask edits are immutable document history commands. Undo/redo and
save/reopen preserve their selection parameters, enabled state, mask identity,
default coverage, and every canonical mask pixel. Selections and masks are
pixel-affecting state; old readers must reject version 3. Profile, graph, count,
size, UUID, transform, and parameter bounds otherwise retain
[version-2 restrictions](project-format-v2.md). Unsupported reserved features
continue to fail explicitly.

See [selection and mask persistence](selection-mask-persistence.md),
[project streaming](project-streaming-contract.md), and
[resource admission](resource-admission.md) for implementation boundaries.
