# Basic destructive adjustment commands

> Export note: Recorded validation results are historical source-repository observations, not qualification of this standalone repository. Commands use the standalone layout.

`adjustLayer(document, target, store, parameters, stop)` returns an unpublished
`EditTransaction`. The caller commits the completed transaction as one named
history command. Failure, cancellation or unrepresentable output returns no
transaction and leaves the immutable input, duplicate layers and history intact.
Folders are rejected; the target raster's extent, identity/profile metadata,
layer placement/name/opacity/blend and document resolution remain intact.

This is the first usable destructive adjustment surface, derived from upstream
`Document/ImageAdjustments.swift`, `Document/PixelAdjust.swift` and
`Rendering/AdjustPixels.c`. It uses FP32 tile-local arithmetic and existing
canonical binary16 storage rather than the macOS RGBA8 whole-image buffer.
These four commands are explicit Linux numerical definitions. They do not
implement all upstream adjustment settings, non-destructive adjustment layers,
Photoshop fixture parity or preservation of a new native RGB16/FP32 format.

| Command | Value range | Definition |
|---|---|---|
| Exposure | -8..8 stops | Multiply premultiplied linear RGB by 2^value |
| Brightness | -1..1 | Add value to bounded straight encoded sRGB components |
| Contrast | -0.95..4 | 0.5 + (encodedComponent-0.5)*(1+value), then bound to 0..1 |
| Saturation | 0..2 | L + (encodedComponent-L)*value, then bound to 0..1; L=0.2126R+0.7152G+0.0722B |

Alpha is unchanged. Exposure retains extended/negative linear RGB when finite
binary16 can represent it; overflow rejects the entire command. Brightness,
contrast and saturation use the bounded straight encoded sRGB domain, explicitly
clipping extended input for these operations. Neutral exposure/brightness/
contrast value 0 and saturation value 1 preserve the exact input without a new
history entry or clipping. Alpha-zero pixels remain canonical transparent zero.

Selection lives in document coordinates. Target layer placements already map
to document coordinates; folders supply hierarchy rather than an additional
placement transform. Transform an 8x8 fixed subpixel grid on each source
pixel into the document. Sample rectangle/ellipse selection and inversion only
inside the finite canvas; average the 64 binary samples. Apply that coverage
once as a premultiplied RGB lerp between original and adjusted pixels. This
preserves alpha and soft selection boundaries without resampling source pixels.
Without a selection the complete finite source raster is adjusted, including
parts outside the canvas and implicit default pixels. Missing transparent tiles
are skipped; selected nontransparent defaults materialize only tile-sized output.

Working pixel storage is one vector of at most 256x256 FP32 pixels (1 MiB).
Reserve those bytes through the injected admission owner before allocation,
retain the reservation through vector destruction, and keep only one source
read lease. Release the source lease before admitting the output tile. Check
cancellation at tile and row boundaries, before source reads, after each write,
and before returning. The enclosing application remains responsible for
admitting document/transaction metadata and scheduling the command on a worker;
the command itself does not allocate a full-raster pixel buffer. Existing tile
and spill admission cover each published output tile.

Tests cover independent exposure/encoded-brightness/contrast/desaturation
fixtures, alpha and transparent zeros, immutable duplicate sharing, exactly one
undo/redo step, neutral no-ops, document-space placement inside a folder, fractional rectangle
coverage, inverted selection, an independent 52/64 ellipse-grid fixture, sparse
negative-origin defaults at tile boundaries, admission refusal, invalid values
and cancellation after the first output tile. A late exposure-overflow fixture
changes the first tile before rejecting the final tile, proving that failed
canonical representability publishes no partial output. The output uses existing canonical
raster records; project IO needs no new adjustment record or schema to preserve
its pixels. Parent compilation and execution remain pending for this packet.

## Shared selection coverage

[selection_coverage.h](../core/selection_coverage.h) supplies the same source-pixel
footprint rasterization to adjustments, brushes and mask creation. Apply its
coverage once to the proposed command output; brush accumulation does not gate
each dab independently through selection. Fractional rectangle and ellipse
fixtures must pass across commands before claiming cross-command edge
consistency. Before adopting this helper, center gating produced 1/0 coverage
where half-translated adjustments produced 0.5/0.5; the shared helper removes
that discrepancy without redefining the frozen selection grid.
