# Building Lumaire Studio

Run every command below from the repository root. Configure does not download
source dependencies.

## Requirements

The full application requires:

| Dependency | Required scope |
|---|---|
| GCC or Clang | C++20 compiler and standard library |
| CMake | 3.28 or newer |
| Ninja | Generator used by the supplied presets |
| Qt | 6.8 or newer; Widgets, Gui, Test and Concurrent |
| Vulkan | 1.2 or newer headers and loader |
| glslangValidator, spirv-val | Compile and validate GLSL build inputs |
| pkg-config | Locate SQLite, zstd, Little CMS, zlib, libpng and libjpeg |
| POSIX threads/Linux system interfaces | Engine, workers and storage |

Qt Test is currently requested by the application configuration even when tests
are disabled. Qt's platform/image plugins must be available at runtime; native
Wayland needs the Qt Wayland plugin. The Vulkan validation layer is optional
unless `--require-validation` is used. A usable Vulkan device is optional at
runtime, but shader tools and Vulkan development files are still full-build
requirements.

Fedora 44 is the development baseline. A development package installation is:

```sh
sudo dnf install gcc-c++ clang cmake ninja-build pkgconf-pkg-config \
  qt6-qtbase-devel qt6-qtwayland vulkan-headers vulkan-loader-devel \
  glslang spirv-tools sqlite-devel libzstd-devel lcms2-devel \
  zlib-devel libpng-devel libjpeg-turbo-devel
```

This installs distribution-selected versions; it is not an archive-pinned,
reproducible dependency closure. Check the CMake configure output against the
minimum versions. Ubuntu 24.04's default Qt is below the application's minimum;
its stock compiler/build tools can still build the core-only configuration.

## Configure, build and test

```sh
cmake --preset debug
cmake --build --preset debug --parallel 1
ctest --preset debug --parallel 1
./build/debug/lumaire-studio --backend cpu
```

The build presets use one compiler job for an 8 GB machine. Keep builds and test
suites serial rather than running sanitizer builds together. Resource admission in
the editor does not limit the compiler's memory use.

Other presets are `release` (optimized with symbols), `clang`, `asan`
(address/undefined sanitizers), `tsan` (separate thread sanitizer), `core-only`
and `profile`. Sanitizers need a compatible compiler/runtime and host address
space. Their configuration alone does not establish a passing run.

Core-only needs C++20, Threads, CMake and Ninja, without Qt or Vulkan:

```sh
cmake --preset core-only
cmake --build --preset core-only --parallel 1
ctest --preset core-only --parallel 1
```

GPU tests are not registered by default. Enable `COMPOSITOR_GPU_TESTS` explicitly
on a host with a suitable Vulkan device. Desktop/input and timing checks need
native sessions; offscreen Qt tests cover only their recorded controller/IO
scope. The prepared GitHub workflow runs core-only GCC/Clang tests on Ubuntu
hosts. Desktop/Fedora and GPU CI are not provided by that workflow.

## Optional local installation

After a release build, install the application and desktop/icon/MIME assets into
an explicitly chosen user prefix:

```sh
cmake --install build/release --prefix "$HOME/.local"
```

This is optional; running directly from `build/release` also works. The installed
executable is `~/.local/bin/lumaire-studio`; ensure that directory is on PATH
when launching through the desktop entry. Desktop asset discovery/cache refresh
is platform-dependent. The repository does not install development dependencies
through this command. Its application icon does not require Qt Svg.

## Storage requirements

Storage tests create private fixtures inside the build directory. Place the
checkout and build directory on an ordinary disk-backed filesystem, not tmpfs.
Directory-policy tests additionally require ext4, Btrfs, XFS or F2FS, trusted
ancestors owned by root or the build user, and no group/other writable ancestor.
A path below `/tmp`, an overlay container filesystem, or a checkout owned by a
different user can fail this policy even if ordinary files can be written there.
Do not weaken the application policy to make a container test appear to pass.

Runtime session storage is injected through the existing XDG state/home policy.
Failed storage initialization is reported; it does not establish low-resource
compatibility. Session spill is temporary and is not crash recovery.

## Optional profiling

Tracy is excluded from the default source export. To enable profiling, restore
its exact upstream commit, including its license notices:

```sh
mkdir -p third_party
git clone https://github.com/wolfpld/tracy.git third_party/tracy
git -C third_party/tracy checkout --detach 30997d5ca6bb632cc10807a1da8a6d3de0aeeb3c
cmake --preset profile
cmake --build --preset profile --parallel 1
```

The `profile` preset intentionally fails if that source checkout is absent.
Default builds do not fetch Tracy. See [THIRD_PARTY](THIRD_PARTY.md).

CI pins `actions/checkout` to the upstream
[v4.3.1 commit](https://github.com/actions/checkout/tree/34e114876b0b11c390a56381ad16ebd13914f8d5)
and disables persisted checkout credentials. Dependency installation in CI uses
Ubuntu's repositories; it is not a fully reproducible package snapshot.
