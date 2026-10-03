# Local development verification

Verified on October 2, 2026, on Fedora 44 with GCC 16.2.1, Qt 6.11.2 and
a native Wayland session. Application code checkpoint: `289d90c`.

## Completed checks

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
  destination. No remote repository was created or pushed.

The full-suite run found and led to a fix for a rounded layer-opacity field.
Fractional opacity, including 37.5%, now survives the UI control and undo/redo.

## Reproduce

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
professional-output or release qualification. GitHub CI is prepared but has
not been run remotely.

GCC 16 emitted existing warnings about an optional tile read lease
(`-Wmaybe-uninitialized`) and PNG error handling (`-Wclobbered`). The build is
not warning-free. No new sanitizer or device-dependent GPU run is claimed here.
