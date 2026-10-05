# Per-channel Curves development acceptance

Verified October 3, 2026 UTC on the Linux development laptop. This packet extends the ordered revisable adjustment stack; it does not complete D2 or establish professional photo/print qualification. Changes remain uncommitted.

Curves now offers RGB master, Red, Green and Blue controls. Each channel retains its own points and preset changes; switching channels preserves exact saved values. Reset clears all four curves. The master runs before the individual channels. Acceptance creates one undo entry; cancel leaves the committed document unchanged. Settings remain editable after save/reopen, and PNG/JPEG export uses the committed result.

## Verification

- Serial release build passed. All 53 default CTests passed in 23.47 seconds.
- Native Wayland adjustment controls passed 11 Qt cases, the stack dialog passed 6, and the channel workflow passed 3, including setup/cleanup.
- The workflow used actual stack controls to create independent Red and Blue curves, checked pixels against an independent calculation, exercised Undo/Redo, saved/reopened, revised Red while preserving Blue, canceled a Blue preview and checked decoded PNG/JPEG output. The final editable project was reopened and its exact parameters and pixels compared with the committed revision.
- Native widget captures and final `.cproj`, PNG and JPEG are privately retained in `build/d2-channel-native-20261003`. The ready dialog capture was inspected. These are widget captures, not physical desktop captures.
- Core tests cover processing order, alpha preservation, identity/HDR behavior, legacy master-only canonical pixel identity, invalid curves and cancellation. Persistence tests cover schema 4 compatibility, schema 5 settings, conflicting envelopes and aggregate metadata refusal before replacement. The writer and loader both enforce the 4 MiB aggregate parameter limit.
- The private install matches the release binary, and its desktop entry validates.

## 12-megapixel resource observation

The actual low-memory host observation passed in 64.341 seconds using normal admission and disk-backed spill. It generated a 4000×3000 procedural gradient, retained source/history, applied Exposure, Levels, independent channel Curves and Color Balance, revised, saved/reopened, verified every source/cache pixel, revised again and exported PNG/JPEG. A precanceled replacement preserved the existing file identity and hash.

At 50 ms sampling intervals, peak observed RSS was 715,935,744 bytes (about 683 MiB), peak admitted memory was 692,736,488 bytes, and peak spill was 96,784,384 bytes across 192 entries. Releasing payloads returned spill bytes/entries to zero with no cleanup failures. The log is `build/release/d2-channel-12mp.txt`. No desktop applications were closed. This procedural fixture is development evidence, not the actual portrait/product brief, full LP8 coexistence, a universal memory ceiling or a latency guarantee.

## Format and scope

New saves use [project format 5](project-format-v5.md). Schemas 1–4 remain readable; unchanged legacy stacks preserve their processing policy and appearance. Older builds cannot open new schema 5 saves. Nonneutral Curves retains the existing encoded RGB clamp behavior; this packet does not introduce extended-range Curves or a new precision mode.

Soft mask/selection editing, clone/healing, defined image resampling and the actual portrait/product revision brief remain pending. Recovery and broader resource/platform/print qualification remain separately tracked. See the [controls guide](per-channel-curves-guide.md).

## Reproduction

From the repository root, with an unlocked native Wayland session and verified disk-backed build directory:

```sh
cmake --build build/release --parallel 1
ctest --test-dir build/release --output-on-failure --parallel 1
QT_QPA_PLATFORM=wayland build/release/photo_adjustment_dialog_test
QT_QPA_PLATFORM=wayland build/release/revisable_adjustments_dialog_test
QT_QPA_PLATFORM=wayland build/release/revisable_channel_workflow_test "$PWD/build/release"
build/release/revisable_stack_resource_observation "$PWD/build/release"
```

Set `LUMAIRE_D2_CHANNEL_CAPTURE_DIR` to a fresh nonexistent absolute directory for retained native workflow artifacts. Resource observations are opt-in, outside the default CTest suite.
