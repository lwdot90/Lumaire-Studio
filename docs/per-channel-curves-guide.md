# Revisable per-channel Curves

Curves can change contrast and color while retaining the original layer pixels and editable settings.

1. Select a raster layer and choose **Adjust → Revisable adjustments…**, or use the primary button in the **Adjustments** dock.
2. Choose **Curves** in the adjustment selector and click **Add**. Remove an unwanted initial Exposure operation. Operations run in list order; use the move buttons to change that order.
3. Choose **RGB**, **Red**, **Green**, or **Blue** in **Channel**. RGB controls the master curve; each color channel has its own curve. Switching channels preserves every other curve.
4. Choose **Linear**, **Lift shadows**, or **Gentle contrast** to replace the selected channel's curve. For finer control, click the graph to add a point, then drag it or use the arrow keys. The **Selected input** and **Selected output** fields use a 0–255 display scale. Delete or **Remove point** removes an interior point; endpoints stay fixed. Each curve supports up to 16 points.
5. Wait for the preview, then choose **OK** to create one undoable edit. **Cancel** restores the committed image. Save the project, reopen it, and use the same dialog to revise the saved curves from the retained original pixels.

**Reset all curves** restores the RGB master and all three color curves to identity. It does not remove the Curves operation or reset other operations in the stack. A preset affects only the selected curve.

The histogram describes the adjusted selected-layer raster before its layer mask or other layers are composited. Transparent samples are excluded. Large rasters use bounded sampling and display **Sampled histogram**; this is not a full-image or final-composite histogram.

For an active Curves operation, the editor unpremultiplies RGB, converts to encoded sRGB, applies the master curve followed by the individual channel curves, and converts back to the existing linear premultiplied RGBA16F storage. Curve inputs and outputs are bounded to 0–1, so active curves use the legacy clamping behavior rather than preserving an unrestricted HDR range. Alpha remains unchanged. When all four curves are identity, the neutral operation preserves the original HDR pixel values.

Project format 5 stores all four curves and the retained source. This editor reads older project formats, but older versions of the application reject format 5. Keep a separate original project if you need to open it in an older version.

This workflow is one part of the D2 photo-editing phase. It does not establish complete D2 delivery, RAW development, or print and color-management support.
