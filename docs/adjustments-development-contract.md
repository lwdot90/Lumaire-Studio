# Destructive adjustment commands

> Export note: Recorded validation results are historical source-repository observations, not qualification of this standalone repository. Commands use the standalone layout.

`adjustLayer(document, target, store, parameters, stop)` returns an unpublished
`EditTransaction`. The caller commits the completed transaction as one named
history command. Failure, cancellation or unrepresentable output returns no
transaction and leaves the immutable input, duplicate layers and history intact.
Folders are rejected; the target raster's extent, identity/profile metadata,
layer placement/name/opacity/blend and document resolution remain intact.

Seven destructive commands use FP32 tile-local arithmetic and the existing
canonical binary16 pixel storage. They do not create adjustment layers or a new
native precision format. The original four command definitions remain unchanged;
Levels, Curves and Color Balance extend the same transaction surface.

| Command | Value range | Definition |
|---|---|---|
| Exposure | -8..8 stops | Multiply premultiplied linear RGB by 2^value |
| Brightness | -1..1 | Add value to bounded straight encoded sRGB components |
| Contrast | -0.95..4 | 0.5 + (encodedComponent-0.5)*(1+value), then bound to 0..1 |
| Saturation | 0..2 | L + (encodedComponent-L)*value, then bound to 0..1; L=0.2126R+0.7152G+0.0722B |
| Levels | Input/output points 0..1; gamma 0.1..10 | outputBlack + (outputWhite-outputBlack) × clamp((encodedComponent-inputBlack)/(inputWhite-inputBlack),0,1)^(1/gamma) |
| Curves | 2..16 points; strictly increasing input from 0 to 1; output 0..1 | Piecewise-linear interpolation, applied independently with the same curve to R/G/B |
| Color Balance | Warmth/tint -1..1 | Premultiplied linear R × 2^warmth, G × 2^(-tint), B × 2^(-warmth) |

Alpha is unchanged. Exposure and Color Balance retain extended/negative linear
RGB when finite binary16 can represent it; overflow rejects the entire command.
Brightness, contrast, saturation, Levels and Curves use bounded straight encoded
sRGB, clipping extended input when a nonneutral command runs. Neutral exposure/
brightness/contrast value 0, saturation value 1, default Levels, identity Curves
(all input/output points equal), and zero Warmth/Tint return the exact input
without clipping, a history entry or discarded redo. Alpha-zero pixels remain
canonical transparent zero. Levels requires inputBlack < inputWhite and
outputBlack <= outputWhite. Nonfinite/out-of-range parameters and malformed
curves fail before publication. See [photo adjustments](photo-adjustments.md)
for the desktop controls and their display-unit conversion.

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

The authored fixtures exercise analytic adjustment output, canonical native
persistence, alpha/profile/selection/placement preservation, immutable inputs,
neutral history behavior, cancellation, admission refusal and representability
failure. These commands save as edited raster pixels in existing `.cproj` v3
records, not serialized adjustment parameters; older project readers remain
supported without a format bump. Compilation and authored test coverage alone
do not establish a passed run or native/large-image qualification. Executed
checks are recorded separately in [local verification](../VALIDATION.md).

## Shared selection coverage

[selection_coverage.h](../core/selection_coverage.h) supplies the same source-pixel
footprint rasterization to adjustments, brushes and mask creation. Apply its
coverage once to the proposed command output; brush accumulation does not gate
each dab independently through selection. Fractional rectangle and ellipse
fixtures must pass across commands before claiming cross-command edge
consistency. Before adopting this helper, center gating produced 1/0 coverage
where half-translated adjustments produced 0.5/0.5; the shared helper removes
that discrepancy without redefining the frozen selection grid.
