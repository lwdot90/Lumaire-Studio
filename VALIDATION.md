# Local development verification

Verified on October 2, 2026, on Fedora 44 with GCC 16.2.1, Qt 6.11.2 and
a native Wayland session. Application code checkpoint: `289d90c`.

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
