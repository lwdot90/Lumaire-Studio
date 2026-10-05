# Phase 1 — raster development delivery acceptance

**Status: complete**, October 2, 2026 (local time). This closes D1 in the
[roadmap](../ROADMAP.md), including H1 and H2. It does not close the original
expanded M3 qualification or qualify a professional replacement release.

Application source is unchanged from `55937cf`. This acceptance checkpoint adds
the integrated `raster_delivery` UI regression and its build registration.
Tests used Fedora 44, kernel 7.2.5-200.fc44.x86_64, GCC 16.2.1, Qt 6.11.2,
native Wayland, Intel UHD Graphics 620 and the user's approximately 8 GB laptop.
Builds and tests ran serially; no normal desktop application was closed or swap
configuration changed to obtain these results.

## Completed gates

| Check | Executed result |
| --- | --- |
| Default release build and regressions | 42/42 CTest tests passed, 0 failed, in 15.49 seconds |
| Complete raster UI workflow on native Wayland | `raster_delivery_test`: 3 Qt cases passed, including setup/cleanup, 2.978 seconds |
| Existing editor input/color/save/export workflow on native Wayland | `editor_workflow_test`: 3 cases passed, 1.638 seconds |
| Levels/Curves/Color Balance dialogs and output on native Wayland | `photo_workflow_test`: 3 cases passed, 0.957 seconds |
| Queued cancellation and independent busy-tab state on native Wayland | `job_cancellation_test`: 4 cases passed, 0.482 seconds |
| Intel GPU/CPU numerical and storage comparison | Required device, validation and synchronization validation enabled; full harness exited 0, all 24 modes passed; no skipped suites observed |
| Native Intel Vulkan lifecycle, layered/high-quality presentation and injected backend failure | `lifecycle_test`, Vulkan mode: 3 cases passed, 5.815 seconds; zero validation errors asserted |
| First brush stroke after GPU-to-CPU and active-tool/new-tab transitions | `editing_transition_test`: 3 cases passed, 0.338 seconds; zero validation errors asserted |
| Native forced-CPU surface lifecycle | `lifecycle_test`, CPU mode: 3 cases passed, 3.812 seconds |
| Private development installation | Installed executable matches release binary; desktop entry validation passed; desktop/icon/MIME files installed |
| Retained installed-app demonstration | 15 steps completed; editable `.cproj`, PNG and JPEG produced under `build/d1-demo-20261002/`; app left open |

The complete UI regression enters through File actions, brush/erase tools and
canvas gestures, a selection drag, the From selection mask action, transform
controls, crop/resize controls, Exposure, Undo/Redo and File save/reopen/export.
It checks requested scale/rotation/translation independently, preserves source
pixels through geometry changes, compares exact canonical raster/mask bytes and
placement after reopen, and compares decoded PNG output before/after reopen.
JPEG dimensions and opacity are checked; the existing export/photo tests retain
independent pixel/color checks.

For reproducible automation the file chooser is Qt's dialog on the native
Wayland surface, with system-native/portal dialogs disabled in this test only.
The fixture directory is selected as test setup; filenames and Open/Save buttons
use actual UI input. This is not a qualification of every system file-picker
adapter. Native focus/exposure and committed numeric values are awaited before
submission. The earlier synthetic-input failures led to these harness fixes,
not changes to application pixel or geometry semantics.

The GPU log identifies Intel UHD 620, validation=1 and synchronization=1. Its
executed suites include 740,120 affine nearest/bilinear/Lanczos samples, mip/cache
and bounded-frame cases, 98,280 layered pixels, 9,240 high-quality layered pixels,
538,900 display pixels and 1,577,496 blend pairs. The maximum affine sample error
was 1.93566e-05; all suite-specific tolerances passed. These are correctness and
lifetime checks, not hardware performance qualification.

Inspected native editor/photo captures combine Qt widgets with the renderer's
accepted current CPU image. They verify visible brush controls and all seven
adjustment tools; they are not physical desktop/compositor screenshots.

## Reproduction

From the repository root, using an unlocked native Wayland session and a
verified disk-backed build directory:

```sh
cmake --preset release
cmake --build --preset release --parallel 1
ctest --preset release --parallel 1
QT_QPA_PLATFORM=wayland ./build/release/raster_delivery_test "$PWD/build/release"
QT_QPA_PLATFORM=wayland ./build/release/editor_workflow_test "$PWD/build/release"
QT_QPA_PLATFORM=wayland ./build/release/photo_workflow_test "$PWD/build/release"
QT_QPA_PLATFORM=wayland ./build/release/job_cancellation_test "$PWD/build/release"
QT_QPA_PLATFORM=wayland COMPOSITOR_TEST_BACKEND=cpu ./build/release/lifecycle_test
```

GPU tests require an actual validation layer. This run privately extracted the
previously pinned Fedora `vulkan-validation-layers-1.4.341.0-2.fc44.x86_64.rpm`,
SHA-256 `d4978ebe2a247db8d3f4fb6e4e2d5984609879eb58eff904c1174cdbee94da1d`,
under `build/d1-validation/`, without a host system installation. The package
was downloaded from its signed Fedora Koji package archive and its hash verified
before extraction. An absent device/layer must fail these checks rather than
be counted as a successful skip.

```sh
d1_vulkan() {
  env VK_LAYER_PATH="$PWD/build/d1-validation/usr/share/vulkan/explicit_layer.d" \
      LD_LIBRARY_PATH="$PWD/build/d1-validation/usr/lib64" \
      VK_DRIVER_FILES=/usr/share/vulkan/icd.d/intel_icd.x86_64.json "$@"
}
d1_vulkan ./build/release/gpu_blend_test --require-device --require-validation --device Intel --storage-dir "$PWD/build/release"
d1_vulkan env QT_QPA_PLATFORM=wayland COMPOSITOR_TEST_BACKEND=vulkan ./build/release/lifecycle_test
d1_vulkan env QT_QPA_PLATFORM=wayland ./build/release/editing_transition_test
```

Do not infer complete GPU coverage from an exit code alone: check the executed
suite lines. Hardware without shaderFloat64 may skip sampling-dependent suites.

## Remaining qualification

The Q1–Q6 ledger in the roadmap stays unchanged: the recorded native P08 timing
failure, full P03/P07 and draft/effect scenarios, broad LP8 coexistence/large
workloads/recovery, X11 and wider reference hardware remain outstanding. The
previous 12-megapixel, approximately 401 MiB observation remains bounded evidence,
not a universal memory limit or full low-resource acceptance. Queued cancellation
does not prove cancellation latency during active large edits, codecs or kernel IO.

There are no remaining D1 blockers in its frozen development-delivery scope.
Proceed to D2's persisted adjustment/draft contract and one revisable adjustment
packet. Do not add new photo tools, recovery or professional print requirements
to D1 retroactively.

## Current-worktree completion audit

Rechecked October 3, 2026 UTC after the first D2 packet, without committing its
changes. All 49 default release CTests passed (17.82 seconds). Native Wayland
raster, editor, photo, cancellation, CPU/Vulkan lifecycle and editing-transition
regressions passed again. Intel GPU comparison executed all suites and 24 blend
modes with validation and synchronization validation enabled; no suites skipped.
Current private installation under `build/d2-install` matches the release binary
and its desktop entry validates. Detailed native results remain under
`build/release/phase1-current-*.txt`. This confirms D1 completion against the
current source while retaining the Q1–Q6 qualification exclusions above.
