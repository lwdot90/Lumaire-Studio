# Lumaire Studio

Lumaire Studio is a Linux desktop image editor for photo editing, compositing
and graphic design.

The current development editor provides:

- A compact dark workspace with a vertical tool strip, contextual tool options,
  Color panel, Layers, Properties and adjustment controls.
- PNG/JPEG import, layered compositing, 24 blend modes, pass-through folders,
  opacity, visibility and immutable layer duplication.
- Brush and erase, source-preserving move/scale/rotate, crop and resize,
  rectangular/elliptical selections, linked raster masks and basic adjustments.
- Undo/redo, editable `.cproj` saving and reopening, and flattened PNG/JPEG export.
- Vulkan presentation with CPU fallback, bounded resident storage and verified
  session spill for immutable tiles and history.

Native pixels use the existing linear premultiplied RGBA16F working contract.
PNG export preserves transparency; JPEG composites over white. Adjustments are
pixel commands, not editable adjustment layers. Masks and painting previews use
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
[native format](docs/project-format-v3.md) describes schema 3 and compatibility
with older native projects. Save editable Lumaire projects as `.cproj`.

The included CI workflow checks the core engine with GCC and Clang. It does not
establish desktop, GPU, performance or professional-output qualification, and a
prepared workflow is not evidence of a completed GitHub run.

See [local verification](VALIDATION.md) for the completed standalone build and
editing checks.

## License

Distributed under [MIT](LICENSE). See [THIRD_PARTY](THIRD_PARTY.md) for required
notices and dependency attribution. No application packages or dependency
binaries are bundled.
