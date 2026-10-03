# Photo adjustments

Choose a raster layer, then open **Levels…**, **Curves…** or **Color Balance…**
from **Adjust** or the Adjustments panel. Confirm with **OK** to submit one
undoable pixel command. A selection clips its effect with the existing 8×8
coverage grid in document coordinates; alpha and layer placement are retained.
Folder selections and busy jobs disable these commands.

## Levels

Input black/white define the encoded tonal range; output black/white define the
resulting range. These fields display `0–255`, converted internally to `0–1`.
Input black must be below input white. Output black may equal output white, but
cannot exceed it. Gamma ranges from `0.1–10`: above `1` lifts midtones, below `1`
darkens them. Defaults are input/output black `0`, input/output white `255` and
gamma `1`.

For encoded component c, the mapping is:

`outputBlack + (outputWhite-outputBlack) × clamp((c-inputBlack)/(inputWhite-inputBlack),0,1)^(1/gamma)`

Gamma `2` with default endpoints maps encoded `.25`, `.5` and `.75` to their
square roots. All RGB channels use the same controls.

## Curves

The graph maps input horizontally to output vertically. It is one combined RGB
curve applied to each channel, with straight segments between control points;
it is not a spline or separate-channel editor. Black `(0,0)` and white `(1,1)`
endpoints remain fixed in the dialog. At most 16 points include those endpoints.

Click inside the graph to add a point, click an existing point to select it,
and drag an interior point to move it. Selected input/output fields display
`0–255`. Left/right arrows change input; up/down arrows change output. The normal
step is `1/255`; Shift changes it to `0.05` internally. Input points cannot cross
one another. Delete, Backspace or **Remove point** removes an interior point.

Presets are **Linear**, **Lift shadows** and **Gentle contrast**. Linear restores
the neutral diagonal. Manual changes mark the curve custom. The graph previews
the mapping only; it does not preview adjusted image pixels.

## Color Balance

Warmth and Tint display `-100–100`, starting at `0`. They are relative RGB gain
controls, not Kelvin temperature, sampled white balance or a RAW pipeline.
Positive Warmth increases red and decreases blue; negative Warmth does the
reverse. Positive Tint decreases green for a magenta shift; negative Tint
increases green.

With internal warmth w and tint t equal to the display values divided by 100,
premultiplied linear RGB is multiplied by `(2^w, 2^-t, 2^-w)`. Warmth `100` and
Tint `100` therefore multiply red by `2`, green by `0.5` and blue by `0.5`.
Alpha stays unchanged.

## Applying, undoing and saving

There is no live image preview, histogram or Reset button. Restore numeric
defaults manually, choose the Linear curve preset, or Cancel and reopen to start
with defaults. Cancel leaves the document unchanged. Neutral Levels, identity
Curves and zero Color Balance preserve canonical pixels exactly, create no
history entry and retain any redo entry.

Nonneutral Levels/Curves work in bounded straight encoded sRGB and clip extended
negative or above-white input before mapping. Color Balance works in linear RGB
and retains extended values until a result cannot fit finite RGBA16F storage.
Such overflow, resource refusal or cancellation fails the whole command without
publishing partially adjusted pixels. Existing Exposure, Brightness, Contrast
and Saturation definitions are unchanged.

Undo/redo restores the complete committed edit. Save `.cproj` to retain the
edited canonical raster, selection, profile and layer placement. Reopen permits
further pixel editing; it does not restore revisable adjustment parameters.
No new adjustment-layer record or format bump is introduced: `.cproj` remains
schema 3 with older project support. PNG/JPEG export flattens the visible result;
PNG retains transparency and JPEG composites over white.

The [command contract](adjustments-development-contract.md) defines processing
and resource boundaries. Formal M3 closure, native/runtime, large-document,
low-resource coexistence and professional color/output qualification remain
pending; this guide does not claim those checks passed.
