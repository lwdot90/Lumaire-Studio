# Pass-through folder opacity

> Export note: Recorded validation results are historical source-repository observations, not qualification of this standalone repository. Commands use the standalone layout.

M3 folder opacity uses inherited pass-through semantics. Each raster's prepared
opacity is its own opacity multiplied by every ancestor folder opacity, in
root-to-leaf order using FP32. CPU and Vulkan consumers apply that prepared value
once before blending the raster. Children remain in bottom-to-top traversal
order and blend directly with preceding document content.

This differs from rendering a folder into an isolated intermediate and applying
opacity to the finished group. Overlapping translucent children receive the
inherited multiplier individually. Isolated group blending remains a separate
feature. Folder blend mode must be Normal, folders cannot own raster assets,
and folder transforms do not multiply child document-space transforms.

Opacity must be finite and in [0, 1] for every node. Hidden ancestors and zero
inherited opacity prune raster preparation. A folder opacity change changes the
prepared opacity for covered descendants, so region equivalence rejects stale
output within their sampling coverage while preserving unrelated regions.

The layer panel enables opacity for raster and folder selections, while folder
blend and sampling controls remain disabled. Busy and missing selections disable
all appearance controls. Layer transactions preserve immutable graph identity
through undo and redo. All 24 blend identifiers are available for raster layers.

Tests in `layer_stack_test.cpp` cover nested opacity, overlapping translucent
children, the distinction from isolation, zero opacity, inherited visibility,
coverage invalidation and immutable undo/redo. `document_workflow_test.cpp`
exercises the folder opacity control, disabled raster-only controls, undo/redo,
and save/reopen of a fractional folder opacity. Build and execution are delegated
to the parent agent. Project format version policy is owned by the parent and
project persistence task.
