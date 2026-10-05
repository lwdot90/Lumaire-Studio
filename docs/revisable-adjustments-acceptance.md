# Ordered revisable adjustments acceptance

Verified October 3, 2026 UTC on the Linux development laptop. This is a D2 packet, not completion of D2 or professional photo/print qualification. Changes remain uncommitted.

The Adjustments panel and Adjust menu expose a retained-source stack editor for Exposure, Brightness, Contrast, Saturation, Levels, combined RGB Curves and Color Balance. Users can add, remove, reorder and reset operations, preview with a bounded sampled histogram, commit one history entry or cancel. Saved settings reopen for revision from the original source. Up to 16 ordered operations are supported; removing all restores the source and detaches the stack. The Exposure shortcut and explicit rasterization remain available.

Initialized controls preserve exact saved values until their respective fields are edited. Typed values must commit to a completed preview before acceptance. Embedded controls scroll, align at the top and the initial dialog size follows available screen dimensions. Curves remains combined RGB; this packet does not claim per-channel curves or independent adjustment masks.

## Verification

- Serial release build and all 51 default CTests passed.
- Native Wayland initialized-control tests passed 9 Qt cases; stack-dialog tests passed 5 cases, including setup/cleanup.
- Native stack workflow passed 3 Qt cases. Actual UI controls added Levels and Color Balance, revised Levels after save/reopen, reordered the operations, canceled a changed draft and exported PNG/JPEG. Canonical results matched independently sequenced destructive operations over the retained original; PNG pixels and bounded JPEG color error were checked. Undo/Redo restored exact document snapshots and each acceptance created one history entry.
- The native capture run additionally saved/reloaded the final reordered editable project and verified its exact parameters and canonical pixels. Ready-dialog/workspace widget captures, `.cproj`, PNG and JPEG remain privately under `build/d2-stack-fit-native-20261003`. Captures were inspected. They are widget images, not physical desktop/compositor captures.
- Core, preview, codec and persistence regression checks remain in the default suite, including source preservation, cancellation/refusal, preview supersession, history, malformed records and shared-cache identities.

## 12-megapixel resource observation

`revisable_stack_resource_observation` generated an opaque 4000×3000 procedural gradient tile by tile with admitted scratch. It retained the original plus history while applying Exposure, Levels and Color Balance; revised exposure, saved/reopened schema 4, compared all source and cache pixels, revised again after reopening and exported PNG/JPEG. A precanceled replacement preserved the existing file identity and streamed hash. The test used actual host memory samples, normal runtime admission and verified disk-backed spill, without closing desktop applications.

The run passed in **54.426 seconds**. At 50 ms sampling intervals the maximum observed RSS was **714,555,392 bytes (about 682 MiB)**; maximum admitted memory was **693,114,080 bytes**; peak spill was **96,784,384 bytes across 192 entries**. Releasing payloads returned spill entries/bytes to zero with no cleanup failures. Results are recorded in `build/release/d2-stack-12mp.txt`.

This is bounded development evidence for one procedural fixture, not portrait/product quality acceptance, full LP8 coexistence, an interactive latency guarantee or a universal memory ceiling.

## Reproduction

From the repository root, using a native unlocked Wayland session and verified disk-backed build directory:

```sh
cmake --build build/release --parallel 1
ctest --test-dir build/release --parallel 1 --output-on-failure
QT_QPA_PLATFORM=wayland build/release/photo_adjustment_dialog_test
QT_QPA_PLATFORM=wayland build/release/revisable_adjustments_dialog_test
QT_QPA_PLATFORM=wayland build/release/revisable_stack_workflow_test "$PWD/build/release"
build/release/revisable_stack_resource_observation "$PWD/build/release"
```

For retained native artifacts, supply `LUMAIRE_D2_STACK_CAPTURE_DIR` as a fresh nonexistent absolute directory when running the stack workflow test. Resource observations are opt-in executables, not default CTest entries.

## Remaining D2 work

Per-channel curves, soft selection/mask editing, clone/healing, defined image resampling and the full retained portrait/product revision brief remain pending. Recovery and broader resource/platform/print qualification remain separately tracked. The original destructive adjustments are still distinct pixel commands.
