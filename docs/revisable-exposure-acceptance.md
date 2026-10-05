# Revisable exposure packet acceptance

Verified October 3, 2026 UTC on the Linux development laptop. D1 remains complete; this is the first D2 packet, not full D2 or professional photo/print qualification.

Revisable Exposure retains an immutable original raster and editable exposure setting. Debounced worker previews are bounded to one active and one latest pending request, use shared compute/memory admission, and include a sampled histogram. OK commits one history transaction; Cancel restores the committed canvas. Schema 4 persists source tiles and parameters, recomputes derived pixels on reopen, and continues to read schemas 1–3. Explicit rasterization preserves displayed pixels and re-enables direct pixel editing.

Validation:

- Serial release build completed. All 49 default CTests passed in 17.82 seconds.
- Native Wayland `revisable_photo_workflow_test` passed all three Qt cases in 1.658 seconds. Actual menu/dialog controls verified preview without history mutation, +1 EV, Undo/Redo, save/reopen, revision to −1 EV from retained original, canceled preview, PNG decoded pixels, tool gating, explicit rasterization and its Undo/Redo.
- Controller tests exercised rapid supersession, active/pending cancellation and destruction with active or queued work. Core/codec/persistence tests exercised source preservation, neutral caches, duplicate layers with independent sampling, masks/placement/profile preservation, malformed records and cancellation/refusal.
- Captured native dialog/workspace widget images were retained privately under `build/d2-native-20261003`, alongside the saved +1 EV project and −1 EV PNG. The dialog/histogram capture was visually inspected. These are widget captures, not physical desktop/compositor capture evidence.

Commands:

```sh
cmake --build build/release --parallel 1
ctest --test-dir build/release --parallel 1 --output-on-failure
env QT_QPA_PLATFORM=wayland LUMAIRE_D2_CAPTURE_DIR=/home/luis/Lumaire-Studio/build/d2-native-20261003 build/release/revisable_photo_workflow_test /home/luis/Lumaire-Studio/build/release
```

Small analytic fixtures validate this packet; they do not complete the full photograph/resource brief. UI revision currently supports a single Exposure operation. Broader saved adjustment controls, per-channel curves, soft-mask editing, clone/healing and the measured portrait/product workflow remain D2 work. Existing low-resource/release qualification gates remain separate. No D2 commit or push was made.
