# Photo retouching, soft masks and image sizing

These development tools extend the [revisable adjustment workflow](revisable-adjustments-guide.md).
They do not by themselves complete the portrait/product acceptance brief or
establish professional Photoshop equivalence.

## Clone and Heal

Select a raster layer, then choose **Clone** (`S`) or **Heal** (`J`) in the tool
rail. Set **Size**, **Hardness** and **Opacity** in the tool options. Hold **Alt**
and click inside the image to choose a source. Draw over the unwanted detail;
the source follows the offset from the beginning of each stroke. Alt-click
again to replace the source. Changing the selected layer or image clears the
source and cancels an unfinished gesture.

Clone copies sampled color and alpha through the soft brush coverage. Heal
transfers source texture while matching the destination's local low-frequency
color; **Heal radius** controls that neighborhood from 1 to 32 document pixels
at stroke time. Size and radius keep that captured coordinate frame during
retained replay, including after layer transforms or image resampling. Healing
preserves destination alpha. It is a bounded texture-transfer method, not
content-aware fill or a guarantee of seamless removal across edges. Choose a
nearby source with similar texture and inspect the result at actual pixels.

Retouching creates retained, ordered stroke settings on the selected layer,
including its source, brush controls, placement and selection at stroke time.
It can coexist with revisable adjustments; retouch is evaluated before the
adjustment stack. Changing the current selection later does not change the
selection recorded in an earlier stroke. Undo restores the previous accepted
state; previews or unfinished gestures do not constitute accepted edits.

Use **Image → Retouch strength…** to select a recorded stroke and revise its
strength after saving and reopening. Zero strength suppresses that stroke
without removing its saved settings. This implementation retains strokes on
the selected layer; it does not create a separate retouch layer automatically.
The source remains available until explicit rasterization or image resampling.

## Paint and feather a layer mask

Use **Layer Mask → Reveal all** to add a fully visible mask, or **From selection**
to create coverage from the current selection. Enable the mask, choose Brush
(`B`), then select **Paint mask** in the tool options. **Reveal (white)** adds
coverage and **Conceal (black)** removes it. Erase (`E`) always conceals. Size,
Hardness and Opacity control the mask stroke, and the current selection limits
its coverage.

Masks remain paintable on layers with retained adjustments or retouch even
though direct color-pixel painting is protected. When only mask painting is
available, the target is selected automatically. A disabled mask must be
enabled before painting. Toggle **Layer Mask → Enable mask** to compare its
effect; removing a mask is a separate undoable change.

**Layer Mask → Feather mask…** applies Gaussian smoothing to the entire existing
mask, independently of the current selection. Its radius is measured in the
layer's source pixels, from 0 to 32; zero leaves coverage unchanged. Repeating
the command smooths the already edited coverage again. The feather radius is
not a retained, revisable mask parameter: Undo restores the earlier coverage.
Saved mask coverage can still be repainted after reopening.

**Select → Feather…** instead softens the active rectangle or ellipse selection
in document pixels, up to 256. That selection softness affects subsequent
selection-limited brush, mask and retouch strokes. It does not directly blur
an existing layer mask.

## Image size and Canvas size

**Image → Image size…** changes the source sample grids, linked masks and canvas
dimensions. Choose **Nearest neighbor**, **Bilinear** or **Lanczos** resampling
and enter the output width and height. Dimensions are independent: maintain
their ratio yourself to avoid stretching. Retained adjustments and retouch
settings remain present and are reevaluated against the resized source.

Resampling replaces the retained source with resized pixels. Reducing its size
discards detail in the accepted document; enlarging later cannot recreate it.
Undo can restore the previous source while that history entry remains
available. Save a separate full-resolution project before making delivery-size
copies; saving and reopening does not preserve the previous session's undo
history.

**Image → Canvas size…** changes canvas dimensions and affine layer sizing
without resampling the retained source grids. It is distinct from the
pixel-resampling command above. Inspect layer placement and output size after
either operation.

## Save, export and rasterize

Current `.cproj` format **6** saves retained retouch strokes, adjustment
settings, sources, selections and mask coverage. The editor reads older native
formats; older application versions reject format 6. Keep a separate original
project when compatibility with an older editor matters.

**Adjust → Rasterize retained edits** keeps the displayed layer pixels and
discards retained adjustment and retouch settings. The layer mask remains
separate. Rasterization is undoable during the current session; saving the
rasterized project does not preserve the discarded settings as editable data.

PNG and JPEG export the accepted composition. PNG supports transparency;
JPEG composites transparency over white. Exported images do not retain native
adjustment settings, masks or retouch strokes. Save the `.cproj` as well when
future revisions are required.

See the [photo acceptance record](d2-photo-acceptance.md) for actual portrait and
product revisions, visual assessment and measured laptop/CPU delivery evidence.
Broader output and low-resource release qualification remain separate roadmap
gates.
