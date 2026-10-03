# Selection and mask editing development contract

> Export note: Recorded validation results are historical source-repository observations, not qualification of this standalone repository. Commands use the standalone layout.

`core/selection_edits.cpp` implements rectangle/ellipse selection metadata
commands. `core/mask_edits.cpp` implements linked raster-layer mask creation,
creation from an active selection, deletion, and enabled state changes. These
return worker-owned EditTransactions; the parent application commits a successful
transaction through its single DocumentHistory command boundary. History assigns
the committed revision. No-op transactions preserve input identity.

A selection is a document-space integer Extent, rectangle or ellipse, and an
inversion flag. Bounds are validated against the canvas; unknown shapes are rejected. Empty active bounds remain distinct from deselection and have zero coverage
before inversion. A selection
change shares existing color/mask raster backing. The commands do not resample
color pixels or independently transform a mask.

Masks have exactly the linked color raster's source extent. Their canonical
RGBA16F encoding has zero RGB and alpha coverage. A full white mask is an empty
TileMap with default alpha one. Selection masks use the shared
`selectionCoverage` helper: an 8-by-8 source-pixel footprint is transformed into
document space and its covered subcenters averaged. Coverage is stored exactly
as a representable multiple of 1/64 in alpha. Rectangle edges are half open;
ellipse membership uses its bounding rectangle and normalized squared radius at
most one. This same coverage contract is consumed by brush and adjustment
commands. There is no additional feathering or path/lasso selection.

All selection masks default to transparent and publish only tiles with covered
pixels. Inversion applies only within the document canvas; inverted selection never
covers off-canvas source pixels. Normal masks
visit the conservatively inverse-transformed selection bounds; inverted masks
visit the inverse-transformed canvas footprint. Both are clipped to source
extent. All pixels within candidate tiles receive the document-space membership
predicate, and tiles equal to the transparent sparse default are omitted. An
empty normal selection has no tiles; an empty inverted selection covers only the
canvas footprint. Mask extent, layer placement, and unchanged clone color backing
remain shared/linked.

Folder masks are explicitly rejected. Existing masks are not silently replaced.
A mask-from-selection request without an active selection is rejected. Deleting
an absent mask or applying an unchanged enabled state preserves the original
document. Disabling a mask retains its immutable raster for redo/re-enablement.

The command uses one at-most-256-square Pixel scratch vector, admitted before
allocation and charged until its storage is destroyed. The existing 100-megapixel raster model limit bounds the source footprint.
Source ROI tile-count arithmetic is checked. A conservative per-node/immutable-raster bookkeeping
reservation is admitted before map construction and retained by the resulting
raster's shared deleter. TileStore separately admits canonical tiles and their
backing. Cancellation is checked before allocation, per tile, per scanline, and
before candidate publication; failure publishes no changed document. Kernel IO
from underlying admitted TileStore backing can delay cancellation. These bounds
are not whole-application low-resource qualification.

Parent integration owns Selection fields/validation in DocumentSnapshot,
EditTransaction metadata comparison and history preservation, LayerStack CPU/GPU
mask sampling, UI controls/worker execution, and native-v3 persistence. Saved
format changes require the parent native-format version update. This component
does not claim that those consumers are implemented or verified by its tests.

`tests/selection_edits_test.cpp` has two groups for selection identity, sharing,
undo/redo/inversion/deselection, and invalid bounds. `tests/mask_edits_test.cpp`
has seven groups for sparse full-white defaults, one-command history, toggling and
deletion, rotated/scaled rectangle/ellipse masks against an independent scalar footprint
oracle, inverted sparse holes, translated/rotated/nonuniform canvas clipping, fractional rectangle/ellipse
footprints and clipped edge tiles, clone sharing, explicit
folder rejection, cancellation and scratch-admission rollback.

Tests are authored; compilation and execution remain with the parent agent's
combined serial checks. No commands in this packet modify native persistence or
rendering implementation.
