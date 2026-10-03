# Using the current development editor

> Export note: Recorded validation results are historical source-repository observations, not qualification of this standalone repository. Commands use the standalone layout.

This Linux development build supports ordinary raster editing and compositing with editable `.cproj` projects. Open an image, edit the active layer, save an editable project, and export the visible composite. Professional RAW, text/vector, print/color and full Photoshop replacement workflows remain in development. This guide describes the exposed controls; it does not assert a completed test run or delivered binary.

## Workspace

The layout follows the tool/options/panels organization in [Adobe’s Photoshop workspace reference](https://helpx.adobe.com/photoshop/desktop/get-started/learn-the-basics/workspace-overview.html), using Lumaire Studio’s implemented commands.

The workspace uses compact neutral-gray controls: canvas in the center, a narrow vertical icon tool rail on the left, and **Adjustments**, **Properties** and **Layers** on the right, with Adjustments/Properties sharing a tabbed panel. The Adjustments panel groups its seven adjustments as icon-and-label buttons. Hover a tool icon for its name/shortcut. One horizontal options bar at the top follows the selected tool: Move shows transform commands, Brush/Erase show painting settings, and selection tools show Deselect/Invert commands. Crop remains available from the Image menu and its tool action. File operations remain in the File menu. **Rendering diagnostics** is hidden initially and can be opened from **View**; restore hidden toolbars/docks there.

## Open and navigate

Choose **File → Open image or project…** (`Ctrl+O`), or **File → New canvas** (`Ctrl+N`). Choose **Layer → Import image as layer…** to combine images. Select the layer you want in **Layers**; use its visibility, opacity and blend controls. Sampling and parent-folder controls are in **Properties**. The development renderer has 24 upstream blend modes. Folder opacity attenuates its descendants under inherited pass-through behavior.

The wheel zooms around the pointer. Hold Space and drag with the left button, or use the middle button, to pan. **View → Fit canvas** (`Ctrl+0`) fits the canvas; **View → Actual pixels** (`Ctrl+1`) returns to actual display pixels. Each tab keeps its own view.

## Paint, erase, move and select

The left **Editing** tool rail contains **Brush** (`B`), **Erase** (`E`), **Move** (`V`), **Rectangle selection** (`M`) and **Ellipse selection**. For Brush or Erase, the contextual options bar at the top shows **Size** in document pixels, **Hardness** and **Opacity** in percent, and **Color**, with a swatch showing the current foreground color. Brush settings are hidden in Move and selection modes; restore the tool rail or options bar from **View** if the bar itself is hidden. Foreground/background swatches remain at the bottom of the tool rail; click a swatch to choose its color, use `X` to swap colors and `D` for the defaults. The Color panel provides a saturation/value square, a hue slider, RGB readout and a hex field: enter a color such as `#d75568` and press Enter to set the foreground. The brush/eraser outline follows the pointer over the canvas and shows the current document-space brush diameter. Use `[` and `]` to decrease or increase brush size. Paint a stroke, then release to commit one undoable edit. Erase removes alpha. The toolbar's Move tool translates the active raster layer while retaining source pixels.

Drag a rectangle or ellipse to select a canvas region. The selection outline appears after release; **Deselect** (`Ctrl+D`) removes it. Active selections constrain pixel edits. Check **Paint mask** to paint the active layer's existing mask instead of its color pixels. Initial masks and selections have hard edges; feathering, lasso/path selections and folder masks are not part of this development slice.

Press Escape to cancel an unfinished gesture. Changing tool/document or losing focus also cancels it. A wheel event during a gesture cancels that gesture and zooms. A stroke exceeding 8,192 distinct consecutive points is rejected; use shorter strokes. **Edit → Undo** (`Ctrl+Z`) and **Edit → Redo** restore complete committed changes.

## Geometry, masks and adjustments

**Image → Move, scale and rotate…** (`Ctrl+T`) accepts relative movement in pixels, scale percentages and clockwise rotation. **Image → Crop…** defaults to the active selection's bounds, or the whole canvas; source pixels beyond the cropped view are retained. **Image → Resize…** (`Ctrl+Alt+I`) changes canvas dimensions and layer placement while retaining source resolution, sampling and PPI.

**Select → Rectangle…** and **Ellipse…** accept numeric bounds. **All** (`Ctrl+A`), **Deselect** (`Ctrl+D`) and **Invert** (`Ctrl+Shift+I`) manage the selection. **Layer Mask → Reveal all** creates a white mask; **From selection** creates one from the current selection. **Remove mask** deletes it; **Enable mask** toggles it without deleting its contents.

The right **Adjustments** dock exposes **Exposure…**, **Brightness…**, **Contrast…**, **Saturation…**, **Levels…**, **Curves…** and **Color Balance…**. The same commands are available under **Adjust**. Select a raster layer, open the desired dialog and confirm it to commit one undoable pixel adjustment. Exposure uses stops: `+1` doubles linear RGB while preserving alpha. Brightness and contrast start at `0`; saturation starts at `1`, with `0` producing grayscale. An active selection limits the affected pixels using the existing 8×8 source-pixel coverage grid. Restore the dock from **View** if hidden.

**Levels** supplies input/output black and white points on a `0–255` scale plus gamma. Gamma above `1` lifts midtones; below `1` darkens them. **Curves** applies one combined RGB piecewise-linear curve: click to add an interior point, drag it or use the selected-point fields/arrow keys, and remove it with Delete, Backspace or **Remove point**. At most 16 points include fixed black/white endpoints. Choose **Linear**, **Lift shadows** or **Gentle contrast** from Preset. **Color Balance** supplies relative RGB Warmth and Tint controls: positive Warmth increases red and reduces blue; positive Tint reduces green for a magenta shift. It is not a Kelvin white-balance or RAW-development control. See [photo adjustments](photo-adjustments.md) for defaults, ranges and numerical limits.

These are destructive pixel commands, not editable adjustment layers. Save/reopen retains their edited canonical pixels, not revisable dialog parameters. Dialogs have no live image preview or histogram; the Curves graph only describes the mapping. There is no Reset button: restore the displayed defaults manually, select the Linear curve preset, or Cancel and reopen the dialog. Neutral defaults create no history entry and preserve redo. Cancel leaves the document unchanged. Folder selections and busy operations disable the Adjustments buttons; they become available again for an idle raster layer.

## Save and export

Use **File → Save project** (`Ctrl+S`) or **Save project as…** to retain editable layers, placements and masks in a `.cproj`. A `*` on the tab indicates unsaved edits; an ellipsis indicates an operation is busy. Wait for the operation to finish before another edit. Closing during a job requests cancellation and asks you to close again when it finishes.

While the current tab has an operation in progress, the status bar shows **Working…** and a **Cancel** button. Click Cancel to request that operation stop; the indicator changes to **Cancelling…** while its worker reaches a cancellation checkpoint and releases its resources. This is an asynchronous request, not a percentage-complete display or an immediate-stop guarantee: codec and kernel IO can delay completion. A failed or canceled edit preserves the previously committed document rather than publishing partially changed tiles. If an operation has already completed, cancellation cannot undo it; use Undo for a committed edit.

Choose **File → Export PNG or JPEG…** (`Ctrl+Shift+E`) for the flattened visible image. PNG retains transparency; JPEG places transparent areas over white. Export does not replace the editable project or mark a clean project dirty. Save the `.cproj` too if you need to revise the work. Selection outlines and temporary brush previews are not exported.

## Resource behavior

The shared runtime limits compute work and admits resident tiles, rendering buffers and IO scratch against memory availability. Verified disk-backed session storage can evict immutable tile/history payloads and reload exact bytes. Existing read leases may keep bytes resident until a job releases them; requesting eviction does not promise immediate memory release. A storage or memory refusal is reported rather than authorizing an incomplete document edit.

Unmasked browsing and the initial Move tool can use GPU presentation. Choosing Brush or Erase requests a CPU canvas before the gesture; enabled masks and selections also use CPU fallback. The CPU backend preserves the development workflow; it can be slower. Large image import still depends on the decoder's peak allocation, and codec/kernel IO may delay cancellation. Session spill is temporary and does not provide crash recovery. Full 8 GB coexistence, large-document and professional-output qualification remain separate pending measurements; ordinary small workflows do not establish those results.

For a repeatable development example and expected saved/exported files, see development delivery (historical source-repository record; not included). The detailed implementation limits are in the [brush](brush-development-contract.md), [geometry](geometry-development-contract.md), [selection/mask](selection-mask-development-contract.md), [adjustment](adjustments-development-contract.md), [export](image-export-contract.md) and [resource](resource-admission.md) contracts.
