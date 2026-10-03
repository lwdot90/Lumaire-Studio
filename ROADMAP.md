# Roadmap

Lumaire Studio targets professional photo editing, compositing, graphic design
and print on Linux, including low-resource and
CPU-only operation. The current development editor is a useful raster editing
slice, not completion of that target.

## Present development scope

The application exposes layered PNG/JPEG workflows, brush/erase, transforms,
source-preserving crop/resize, hard rectangular/elliptical selections, linked
raster masks, seven pixel adjustments (Exposure, Brightness, Contrast, Saturation,
Levels, Curves and Color Balance), undo/redo, editable native projects and
PNG/JPEG export. It has 24 legacy blend modes, pass-through folder opacity,
Vulkan presentation and CPU fallback. Its compact contextual workspace includes
a Color panel, tool strip, tool options and Layers/Properties controls.

Resource infrastructure includes admitted immutable resident backing, temporary
verified disk spill, bounded worker scheduling and streamed project tile IO.
These components do not prove all imported decoder peaks, desktop coexistence,
large-image latency or recovery behavior. Adding the photo-adjustment commands
does not close formal M3; native/runtime qualification remains pending.

## Pending capability work

- Precision-preserving integer16/FP32 processing, explicit versioned color and
  blending policies, ICC working/proof/output workflows, CMYK and print output.
- Professional RAW development and qualified import/interchange, including
  editable PSD/PSB workflows.
- Rich text, vector/path tools, graphic-design layout and editable source objects.
- Advanced selections, masks, brushes, adjustments, filters and layer effects
  with live drafts and complete native persistence.
- Crash recovery, robust large-document operation and release packaging.

Numerical, color, source-object and schema extensions need explicit contracts
and fixtures before they are enabled. Old RGBA16F projects must retain their
existing appearance; enum names or unimplemented descriptors are not completed
rendering capabilities.

## Qualification still required

Keep correctness tests, native desktop usability, GPU compatibility,
performance and professional-output acceptance separate. Freeze representative
fixtures and timing/memory boundaries before claiming results. Low-resource
acceptance includes system headroom, retained/transient/GPU memory, realistic
background applications, CPU-only quality, failure preservation and large-job
progress/cancellation. It must be demonstrated on the supported machine class,
not inferred from configured byte limits.

Each new editing slice should be reachable through the UI, undoable, saved and
reopened without losing editability, and verified by observable output. The
[editing guide](docs/today-editor-guide.md) describes current controls; the
[native format](docs/project-format-v3.md) defines persisted development data.
