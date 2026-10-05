# Lumaire Studio

Lumaire Studio is a Linux desktop image editor for photo editing, compositing
and graphic design.

The current development editor provides:

- A compact dark workspace with a vertical tool strip, contextual tool options,
  Color panel, Layers, Properties and adjustment controls.
- PNG/JPEG import, layered compositing, 24 blend modes, pass-through folders,
  opacity, visibility and immutable layer duplication.
- Brush and erase, source-preserving move/scale/rotate, crop and canvas sizing,
  pixel resampling with three filters, feathered rectangular/elliptical selections,
  linked paintable/featherable raster masks and seven pixel adjustments:
  Exposure, Brightness, Contrast, Saturation, Levels, Curves and Color Balance.
- Revisable stacks of all seven adjustments, retained source pixels, live
  preview and sampled histograms, with editable settings after reopening.
- Curves controls for a master RGB curve and independent Red, Green and Blue
  curves, with presets and Reset all curves. See the
  [packet acceptance](docs/per-channel-curves-acceptance.md) for executed checks.
- Clone and bounded texture-transfer healing with retained strokes, editable
  stroke strength after reopening, and retouch before revisable adjustments.
- Undo/redo, editable `.cproj` saving and reopening, and flattened PNG/JPEG export.
- Vulkan presentation with CPU fallback, bounded resident storage and verified
  session spill for immutable tiles and history.

Native pixels use the existing linear premultiplied RGBA16F working contract.
PNG export preserves transparency; JPEG composites over white. Adjustments are
available as pixel commands and retained-source stacks. Masks and interactive painting/retouch use
the CPU canvas. Import still depends on the decoder's allocation peaks.

This is a development editor. RAW development, advanced text/vector editing,
professional color/print output, recovery and
complete low-resource qualification remain unfinished. See [ROADMAP](ROADMAP.md).

## Build and use

See [BUILDING](BUILDING.md) for dependencies and configuration. From this
repository's root:

```sh
cmake --preset release
cmake --build --preset release --parallel 1
ctest --preset release --parallel 1
./build/release/lumaire-studio --backend cpu
```

Use `--backend auto` for Vulkan where supported. The CPU backend preserves the
available editing workflow. Open an image or project through File; save `.cproj`
for later editing and export PNG/JPEG for flattened output.

The [editing guide](docs/today-editor-guide.md) describes the controls. The
[native format](docs/project-format-v6.md) describes schema 6, written by new
saves, and read compatibility with schemas 1–5. Older application versions
reject schema 6; keep a separate original if you need older-version access. Save editable Lumaire projects as `.cproj`.

The included CI workflow checks the core engine with GCC and Clang. It does not
establish desktop, GPU, performance or professional-output qualification, and a
prepared workflow is not evidence of a completed GitHub run.

See the [per-channel Curves guide](docs/per-channel-curves-guide.md) for channel
controls and their processing limits.
The [photo tools guide](docs/d2-photo-tools-guide.md) covers retouching, soft masks,
resampling and their current limits. The integrated D2 build passed all 61 default tests and the native portrait/product
edit, save/reopen, revision and PNG/JPEG export workflows. Independent saved-file
and visual review passed the frozen photo brief, including corrected retouch
coverage and settled adjustment controls. D2 is complete for this development
scope; professional replacement and full low-resource release qualification
remain separate.
See the [D2 acceptance record](docs/d2-photo-acceptance.md).

See [local verification](VALIDATION.md) for the completed standalone build and
editing checks. The per-channel packet also passed a bounded procedural
12-megapixel edit/save/reopen/export observation; its
[acceptance record](docs/per-channel-curves-acceptance.md) reports the measured
memory and storage scope. The newer actual-photo record is separate from that
procedural fixture. D2 photo acceptance is complete; broader LP8 qualification remains pending.

## License

Distributed under [MIT](LICENSE). See [THIRD_PARTY](THIRD_PARTY.md) for required
notices and dependency attribution. No application packages or dependency
binaries are bundled.
