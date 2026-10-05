# Local development verification

Initial standalone verification was on October 2, 2026, on Fedora 44 with
GCC 16.2.1, Qt 6.11.2 and native Wayland, at initial source checkpoint
`289d90c`. Later checkpoint results and phase-1 closure are recorded below.

## Phase 1 closure

D1 is complete. Current-source native editor/photo, full raster UI and queued
cancellation workflows passed, alongside Intel GPU/CPU correctness with
synchronization validation, native Vulkan/fallback lifecycle and backend/tab
brush transitions. The complete default suite now passes **42/42 tests** in
**15.49 seconds**. The installed app completed its retained 15-step demo.

The [phase-1 acceptance record](docs/raster-delivery-acceptance.md) lists exact
checks, environments, scope and reproduction. It supersedes the earlier pending
native status for this development delivery; the earlier 35/41-test and locked
session records below remain historical evidence. Original expanded M3, broad
LP8 and professional replacement-release qualification remain outstanding.

## Initial standalone checks

- Configured a fresh standalone release build and built all default targets
  with one compiler job. All 108 compile entries used standalone source paths.
- Ran the complete default CTest suite: **35 passed, 0 failed**, in 19.26 seconds.
- Ran the editor workflow on native Wayland: **3 Qt test cases passed**,
  including setup and cleanup. The workflow exercises actual tool/color input,
  layer target switching, adjustments, undo, native save/reopen and PNG/JPEG
  output checks.
- Inspected the workspace capture. It combines Qt widgets with the accepted
  CPU-rendered image; it is not a physical desktop/compositor screenshot.
- Installed into a private build-directory prefix and verified the executable,
  desktop entry, icon and MIME definition. `desktop-file-validate` passed.
- Opened the application and completed its 12-step CPU demonstration: import,
  brush stroke, ellipse selection, mask, exposure, deselect, transform, crop,
  resize, editable project save, PNG export and JPEG export. The app remained
  open with the result. Outputs are generated under `build/lumaire-demo/` and
  are not committed source files.
- Checked exported source hashes, relative documentation links and unchanged
  MIT license bytes. Confirmed that the exporter refuses an existing
  destination. This initial check preceded publication of the standalone repository.

The full-suite run found and led to a fix for a rounded layer-opacity field.
Fractional opacity, including 37.5%, now survives the UI control and undo/redo.

## Photo editing and cancellation checkpoint

Verified October 2, 2026 (local time), in the same Fedora/Qt/GCC environment:

- Built all default release targets with one compiler job, then incorporated
  test corrections and the expanded demonstration in an incremental build.
- Complete default CTest suite: **41 passed, 0 failed**, in **13.08 seconds**.
  Added checks cover Levels/Curves/Color Balance pixel math, alpha and selection
  behavior, invalid controls, rollback, undo/redo, canonical save/reopen,
  actual dialog/tool input and decoded PNG/JPEG output.
- Progress-widget and actual cancellation tests passed. They cancel queued
  processing through the statusbar button, preserve pixels/dirty state/redo,
  verify independent busy-tab feedback and prove a subsequent edit succeeds.
- The offscreen photo workflow passed all 3 Qt cases (including setup/cleanup).
  Inspected its widget capture combined with an accepted CPU-rendered image;
  all seven adjustment tools are fully visible. This is not a desktop capture.
- The app's expanded **15-step CPU demo completed** and wrote an editable
  project plus PNG/JPEG under `build/photo-demo-20261002/`. The new app remains
  open. The desktop had locked before final checks, so this checkpoint does
  **not** claim a new native input/frame qualification.

A separate real-host, disk-backed **4000 × 3000** tiled observation passed in
**24.13 seconds**. It retained the original and three adjustment history
snapshots, performed undo/redo, saved/reopened a project, verified selected
canonical samples, and exported PNG/JPEG. Highest sampled process RSS/high-water
value was **420,737,024 bytes (about 401 MiB)**. Reopen held **95,727,616 bytes**
in **190 spill entries** at its sample. After releasing payloads, tile usage and
spill entries were zero; tracked CPU charges returned to baseline. Allocator RSS
remained above baseline. Actual host headroom probes stayed enabled, and bounded
refusal/pre-cancellation checks preserved snapshots. This is one development
observation, not full LP8 qualification or a universal memory ceiling.

Logs, captures, JUnit reports and demo artifacts are local ignored build outputs.
Reproduce the added checks after the normal build:

```sh
ctest --preset release --parallel 1
QT_QPA_PLATFORM=offscreen ./build/release/photo_workflow_test "$PWD/build/release"
./build/release/photo_resource_observation "$PWD/build/release"
```

The new features still lack live image adjustment preview and histograms.
Professional color/print output, RAW/PSD/text/vector work, recovery, broad
large-document coexistence and formal M3/release qualification remain pending.

## Reproduce initial checks

Run from the repository root on a suitable disk-backed filesystem:

```sh
cmake --preset release
cmake --build --preset release --parallel 1
ctest --preset release --parallel 1
QT_QPA_PLATFORM=wayland ./build/release/editor_workflow_test "$PWD/build/release"
cmake --install build/release --prefix "$PWD/build/install-check"
desktop-file-validate build/install-check/share/applications/lumaire-studio.desktop
```

For the visible demonstration, choose a fresh output directory:

```sh
./build/release/lumaire-studio --backend cpu --demo-dir "$PWD/build/new-demo"
```

`SOURCE_MANIFEST.json` records the initial source import. Later changes are
recorded by Git; its import hashes are not a rolling checksum of later commits.

## Scope

These checks establish a working development build and bounded editing
workflows. They do not establish full low-resource, large-document, GPU,
professional-output or release qualification. The published initial checkpoint (`e5ac862`) subsequently passed the GitHub
core correctness workflow with both GCC and Clang.

GCC 16 emitted existing warnings about an optional tile read lease
(`-Wmaybe-uninitialized`) and PNG error handling (`-Wclobbered`). The build is
not warning-free. No new sanitizer or device-dependent GPU run is claimed here.

## D2 ordered adjustment stack

The uncommitted seven-kind stack packet passed all 51 default CTests in 19.98
seconds. Native Wayland initialized controls, stack dialog and the complete
Levels/Color Balance revision/reorder/save/reopen/PNG/JPEG workflow passed.
A real-host 4000×3000 retained-source, three-operation observation passed in
54.426 seconds with maximum sampled RSS 714,555,392 bytes (about 682 MiB),
maximum sampled admitted memory 693,114,080 bytes, and 96,784,384 spill bytes
in 192 entries. All source/cache pixels matched after reopen; canceled writes
preserved the existing file. Spill entries returned to zero after payload
release. This procedural fixture is bounded resource evidence, not full LP8
or portrait/product quality acceptance. See
[stack acceptance](docs/revisable-adjustments-acceptance.md).

## D2 per-channel Curves

The subsequent uncommitted packet passed the serial release build and all 53
default CTests in 23.47 seconds. Native Wayland adjustment controls, stack dialog
and channel workflow passed 11, 6 and 3 Qt cases respectively. Actual controls
exercised independent Red/Blue editing, Undo/Redo, schema 5 save/reopen,
revision, canceled preview and checked PNG/JPEG output. Schema 4 compatibility
and writer/loader aggregate parameter bounds are covered in the default suite.

The updated real-host 4000×3000 four-operation observation passed in 64.341
seconds. Maximum sampled RSS was 715,935,744 bytes (about 683 MiB), admitted
memory 692,736,488 bytes and spill 96,784,384 bytes across 192 entries. Source
and cache pixels round-tripped exactly; canceled replacement preserved the
destination. Released spill returned to zero without cleanup failures. The
private installed binary matches the release build and its desktop entry
validates. These checks remain bounded development evidence; full D2 and
portrait/product/LP8 qualification remain pending. See
[per-channel acceptance](docs/per-channel-curves-acceptance.md).


## D2 final photo acceptance — October 4, 2026

D2 is complete at its frozen development scope. The final serial release build
passed; all 61 default CTests passed in 31.90 seconds. Native Wayland CPU photo
workflows passed four Qt cases in 204.191 seconds, and independent saved-file
review passed four cases in 301.410 seconds. Both original 4000×3000 photographs
retain full-resolution original, revised and polished projects, plus 2000×1500
Lanczos delivery projects and PNG/JPEG exports. Independent visual review passed
the selected distraction cleanup, deliberate partial-strength revision, subject
and product detail, grade and mask boundaries. Settled curve controls passed
layout and saved-value checks; Cancel preserved the committed document.

Portrait/product maximum sampled process RSS was 1,004,785,664 / 1,517,277,184
bytes. Actual host admission, the 2 GiB application envelope, 512 MiB system
floor and verified disk-backed spill remained enabled. Both workflows retired
all spill bytes and entries with no cleanup failures. This measures these jobs;
it does not qualify full LP8, crash recovery, RAW, professional color/print,
Photoshop interchange or all hardware.

Evidence is preserved in ignored `build/release/d2-final-build.txt`,
`d2-final-ctest.txt`, `d2-native-photos-final.txt`,
`d2-native-saved-review-final.txt`, and `build/d2-control-capture/run.log`.
Artifacts and attribution are in `build/d2-photo-native-20261004-final/`.
Earlier failed pressure/retouch attempts remain preserved and described in the
[D2 acceptance record](docs/d2-photo-acceptance.md).

Base commit: `33f273d4b4ca4f5b0635edc1a84a6bdd42a137ef`.
Final source digest: `12269649394c77962d6d7fd6bf0d09378a42b06d64000ec14a21a90fa4e9ec5c`.
Release binary SHA-256: `ef6023b57a603df10724cd2d4d5a862c075a92d878eefcd59d858f0b497a1760`.
The source manifest defines the digest's file scope. Changes remain uncommitted
at the owner's request. These final results supersede earlier pending D2
statements for the current development snapshot, without changing their
historical test scopes.
