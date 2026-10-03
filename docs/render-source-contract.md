# Immutable render-source and dependency interface

> Export note: Recorded validation results are historical source-repository observations, not qualification of this standalone repository. Commands use the standalone layout.

`core/render_source.h/.cpp` provides an in-memory render-source adapter and a
validated dependency graph. It adds no native project fields or schema version.
The existing legacy renderer and file samples retain their RGBA16F behavior.

## Supported source adapter

`RenderSource` owns an immutable `RasterSnapshot`, its source extent, ID and
revision, and a validated invertible source-to-document affine transform.
`documentToSource` maps continuous document coordinates back into the original
source resolution. It never resamples or rewrites stored pixels. Pixel-center
and sampling/filter selection remain the renderer's existing responsibility.

The only accepted processing descriptor is premultiplied binary16 RGBA with
`LegacyLinearSrgb` and policy version `legacy-schema-1`. The original source
profile is retained as immutable provenance. It does not turn that profile into
a new working space or implicitly assign/convert pixels. Unsupported precision
and color-policy descriptors throw before a source can become renderable.

`SourceReadContext` reads integer source-resolution pixels. It returns transparent
outside the extent and the existing canonical default inside missing tiles. A
context retains at most one `Tile::ReadLease`; it releases that lease before
admitting/rehydrating a different tile. Actual tile reads use the live immutable
tile backing's verified read API. A lease remains readable across cache eviction,
and a later context read can rehydrate it. No raw resident pointer escapes this
interface. A context retains its immutable source and is confined to one worker;
call `release()` when yielding that worker. Source metadata and the context's
small objects are subject to the caller's existing metadata/job bounds.

Every read checks the supplied stop token, including transparent/default reads.
The token must represent both worker shutdown and request supersession when the
caller needs both to cancel. Synchronous rehydration belongs on a worker, never
a GUI publication path. Kernel IO cannot provide a hard cancellation deadline.

## Dependencies, readiness and invalidation

`RenderDependencyGraph` accepts an immutable acyclic graph containing ready
legacy source nodes and explicitly described isolated-group, filter-stack,
effect-stack and alternate-color-policy nodes. Such operation nodes are
**unsupported semantics**, even when their opaque versioned parameter bytes are
present. A dependent node reports **dependency unavailable** when any upstream
node is unavailable. The graph does not flatten, skip, or render these nodes as
an identity operation. Callers must check readiness before attempting output.

Source nodes accept no effect parameters, dependencies or halos, and their
region must match the source extent. Unsupported operation nodes require an
explicit parameter contract and at least one dependency. Empty/duplicate IDs,
missing/repeated dependencies, cycles, invalid regions, unknown node kinds and
excessive depth/count/parameter sizes are rejected. Limits are 10,000 nodes,
100,000 edges, 64 dependency edges per longest path, 1 MiB parameters per node
and 16 MiB total parameter bytes. They bound graph validation independently of
caller admission; they are not permission to allocate those maxima on a GUI
thread. IDs and parameter contract identifiers are limited to 256 bytes.

`sameOutput` compares the complete reachable dependency closure. Node revision,
kind, ordered dependency IDs, exact parameter contract/bytes, region, halo,
readiness, source placement and immutable raster/profile identity all enter the
comparison. Unrelated graph changes do not invalidate an unchanged source.
Identical textual asset IDs or revisions do not alias different immutable raster
objects. The comparison uses no digest that could silently miss invalidation.
A conservative mismatch may redraw equal pixels; it never certifies equality
using only a reused ID or numerical revision. Graph invalidation is available to
renderer integration, but this adapter does not replace existing mip/GPU cache
keys or enable an unsupported node in the existing layer graph.

## Numerical and product boundaries

M0-R has not pinned integer16/FP32 preservation, alternate color/compositing
policies, CMYK, isolated/pass-through group behavior, Fill effects or new project
schema contracts. M11 supplies filter/effect/object semantics. This interface
makes those dependencies and unavailable states explicit; its enums are not
implementations of those capabilities. No procedural noise/grain seed interface
is invented without a pinned coordinate/seed/effect contract. Stable noise/grain,
effects during drafts, V08–V10 and Photoshop reference qualification therefore
remain separate unfinished work. This packet alone does not complete M3.7.

## Validation handoff

`tests/render_source_test.cpp` contains five small groups: frozen descriptor and
transform validation; exact negative-origin/partial-tile/default reads and
snapshot lifetimes; readiness and transitive invalidation; graph/cycle/limit
validation; and actual disk-backed tile reads across spill, lease retention,
release, verified rehydration and cancellation. Payload fixtures contain only
six pixels; the test requires a verified disk-backed build directory argument
and creates/removes only its own `mkdtemp` fixture.

Parent build wiring: compile `core/render_source.cpp` into `compositor_engine`;
link `render_source_test` with that target; register CTest command
`render_source_test ${CMAKE_CURRENT_BINARY_DIR}`. Compilation and execution are
intentionally left to the parent to keep build jobs serialized on the LP8 host.
No pass result is claimed until the parent records its combined verification.

## Production CPU source admission

Production layer sampling and mip base reads use `SourceReadContext` through
shared-source `RasterReader` construction. Pixel coordinates are already mapped
by the sampler; the source adapter uses identity placement and introduces no
additional coordinate transform. Borrowed raster readers preserve their explicit
caller-owned lifetime contract.

Before constructing sampling plans or their shared `RenderSource` adapters,
`CpuWorker` reserves `cpuRenderScratchBytes(document)`: the existing 4 MiB base
allowance plus `CpuLayerSampler::scratchBytes(visibleLayerCount)`. The derived
term includes twice the concrete `Plan` size per layer, `sizeof(RenderSource)`
plus 128 bytes for control/allocation metadata, and eight layer-index slots per
layer for overlapping candidate/result vectors. Plans reserve the visible-layer
count once. Checked multiplication/addition reject overflow. This accounts for
source-plan growth independently of raster dimensions and rejects admission
before adapter construction. Resident tile leases and mip regions retain their
separate admission charges. The one-source thumbnail path retains its existing
2 MiB scratch allowance.
