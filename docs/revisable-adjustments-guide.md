# Revisable photo adjustments

Select a raster layer and choose **Adjust → Revisable adjustments…**, also
available in the Adjustments panel. The editor retains that layer's original
source and an ordered list of controls. Reopen the saved `.cproj` and open the
same command to revise them without compounding already adjusted pixels.

## Edit and preview

Add an operation, select it and change its controls. Available operations are
Exposure, Brightness, Contrast, Saturation, Levels, Curves and Color Balance.
The list accepts at most 16 operations. Evaluation follows list order from the
retained source; reordering can change the result.

Changes request a cancellable worker preview. **OK** becomes available when the
current preview is ready. Accepting changed controls creates one undoable edit;
accepting the same saved controls leaves history unchanged. **Cancel** restores
the last committed document. Preview changes alone do not mark the file dirty.
Large images or long lists can take time; a ready preview is not instantaneous
processing of every slider movement.

Removing all operations and accepting restores the exact retained source and
detaches the list. A neutral operation can still be retained as editable
metadata even when it leaves the displayed pixels unchanged.

## Controls and neutral defaults

| Operation | Controls | Neutral setting |
| --- | --- | --- |
| Exposure | Linear-light stops, −8 to +8 | 0 stops |
| Brightness | Encoded RGB offset, −1 to +1 | 0 |
| Contrast | Encoded RGB contrast, −0.95 to +4 | 0 |
| Saturation | Encoded RGB saturation, 0 to 2 | 1; 0 makes grayscale |
| Levels | Input/output black and white; gamma | Full 0–255 range, gamma 1 |
| Curves | RGB master plus independent Red, Green and Blue piecewise-linear mappings | All four curves at identity |
| Color Balance | Relative warmth and tint | 0 warmth, 0 tint |

Levels gamma above 1 lifts midtones; below 1 darkens them. Curves uses at most
16 points per curve. Choose RGB, Red, Green or Blue to edit that curve; the RGB
master runs before the individual color curves. Presets affect the selected
curve, while **Reset all curves** restores all four to identity. See the
[per-channel Curves guide](per-channel-curves-guide.md) for graph controls and
the bounded color-processing contract. Positive
warmth raises red and lowers blue; positive tint lowers green toward magenta.
Color Balance is not Kelvin white balance or RAW development.

These operations affect the whole retained raster. Active selections do not
limit them; the layer's mask and opacity still control visible composition.
The histogram samples up to 65,536 positions from the adjusted raster, excludes
transparent pixels and uses display-encoded RGB bins. It is not a full-image
analysis, a masked-composite histogram or professional ICC color management.

## Save, export and paint

Save the native `.cproj` to retain source pixels and parameters. Current project
format 5 stores all four curves and reads older project formats; older editor
versions reject format 5. Keep an original copy when older-editor compatibility
is needed. PNG/JPEG export
flattens the accepted composition; it does not retain editable controls. PNG
preserves transparency; JPEG places transparent pixels over white.

Direct color-pixel painting and destructive adjustments are disabled for a layer
with retained adjustments. Use **Adjust → Rasterize adjustments** when you
explicitly want to keep the displayed pixels and discard their retained
source/controls. Rasterization is undoable; after accepting it, subsequent edits
use those rasterized pixels as their starting point.

The original **Exposure…**, **Levels…** and other pixel commands remain
destructive, selection-limited operations on ordinary raster layers. **Revisable
Exposure…** is a shortcut for a single retained Exposure operation; use the
general dialog for ordered lists.

This development workflow does not establish full D2 completion, RAW/high-depth
editing, professional color/print output, crash recovery or Photoshop parity.
See the [photo revision brief](revisable-photo-brief.md) for the wider D2 target.
