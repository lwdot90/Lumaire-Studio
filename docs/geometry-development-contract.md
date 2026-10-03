# Geometry edit commands

> Export note: Recorded validation results are historical source-repository observations, not qualification of this standalone repository. Commands use the standalone layout.

`transformLayer`, `cropDocument` and `resizeDocument` return one `EditTransaction`
against the supplied immutable document. Publishing and undo naming remain the
caller's responsibility. No command reads, allocates, flattens or resamples tile
pixels. Sources, masks and hierarchy metadata remain shared.

Move, scale and rotation adapt the center and clockwise, y-down conventions in
`Compositor/Document/LayerTransform.swift`. Scale factors operate along the
current layer's source axes, around the currently placed source extent center;
rotation then acts in document space and translation moves that center. Existing
flips, affine shear and sampling are preserved. Folder transformation is rejected
because Linux folders are pass-through and child placements are document-space.

Cropping follows `Crop.swift` and `CanvasResizer.swift`: an integer rectangle
strictly inside the existing canvas changes canvas dimensions and translates all
placements by the negative crop origin. Source pixels beyond the new canvas are
retained and return on undo. Canvas resize follows the document-coordinate scale
in `ImageResizer.swift`, while Linux retains full affine shear and original source
resolution rather than destructively rasterizing each layer. Nonuniform resize
multiplies the affine output axes by the new/old canvas ratios. Sampling quality
is preserved; the user can select Lanczos independently. Resolution metadata is
unchanged.

Crop and resize clear selection to avoid stale document-space clipping. Attached
linked masks follow the color layer's affine; geometry commands do not change
mask contents. All numeric parameters must be finite, scales positive, placed
source edge lengths at most 300,000 pixels and mapped source corners within
one million document pixels. Existing affine conditioning and canvas extent
validation still apply. Invalid commands never mutate their immutable inputs.

`geometry_edits_test.cpp` covers placed centers, scale/rotation orientation,
negative source origins, nested layers, shared source identity, pixel-position
agreement after crop/resize, immutable undo/redo, no-op redo retention and rejected
geometry, linked mask identity/following and selection clearing. The commands use
`core/editor_commands.h`. Parent integration supplies transaction canvas and
selection fields, UI commands, persistence and build wiring.
