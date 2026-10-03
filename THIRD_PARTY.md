# Third-party dependencies and attribution

The application source is distributed under [LICENSE](LICENSE). Preserve the
original notice: **Copyright (c) 2026 Wonder Assembly LLC**. The included MIT
notice applies to reused code and must remain with copies of that code.

Default builds link distribution-provided dependencies. They do not bundle
library binaries, models or dependency archives.
A binary package must include the notices and satisfy the terms of the exact
libraries/plugins it distributes; a development package list is not a complete
binary-distribution inventory.

| Dependency | Current use and attribution source |
|---|---|
| Qt Base | Widgets, Gui, Concurrent and Test; preserve applicable Qt module/plugin license notices from the selected Qt distribution |
| Qt Wayland | Native Wayland platform plugin; preserve that plugin's separate notices when distributed |
| Vulkan headers/loader | Device interfaces and loader; upstream Apache-2.0 notices |
| glslang and SPIR-V Tools | Shader build/validation tools; retain their own upstream notices if distributing tools |
| SQLite | Native project database; upstream public-domain dedication/blessing |
| zstd | Project tile compression; upstream BSD-3-Clause or GPL-2.0 licensing options |
| Little CMS | ICC import conversion; upstream MIT license |
| zlib | Compression dependency; upstream zlib license |
| libpng | PNG export; upstream libpng license and notices |
| libjpeg-turbo | JPEG export; upstream project includes distinct IJG, BSD and zlib license notices |
| Tracy, optional | Profiling client, BSD-3-Clause; exact commit `30997d5ca6bb632cc10807a1da8a6d3de0aeeb3c` |

Consult the selected dependency's actual source/license files rather than
assuming a single package label covers every bundled component. Qt licensing
also depends on the chosen distribution and modules. The supplied build links
system libraries; this repository does not select a binary redistribution
arrangement for Qt.

Tracy source is absent from the default export. If restored for the optional
profile build, preserve its LICENSE and bundled component notices. The pinned
source and bootstrap commands are in [BUILDING](BUILDING.md).

No RAW/TIFF/HEIF/PSD decoder, model weights, font collection or third-party photos
are claimed bundled or supported by this dependency list. Import currently
exposes PNG/JPEG. Historical development dependency inventories from the parent
project are intentionally not the standalone application's dependency contract.
