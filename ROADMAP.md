# Lumaire Studio roadmap

Revised October 2, 2026 (local time). This is the active delivery plan for the standalone
Linux project. It separates implementation progress from product qualification.
It replaces the former open-ended milestone scheduling, without turning any
failed or pending acceptance result into a pass.

## Product target

Deliver a Linux editor with comparable practical value for professional photo
editing, compositing, graphic design and print. Finished jobs must remain
revisable after native reopen and support their required handoff formats.
A familiar UI or a flattened export alone does not establish replacement value.

The user's 8 GB / Intel i5-8265U / UHD 620 laptop is a required target, including
CPU-only completion. Larger jobs may take longer, but must preserve source
content and editability, provide progress/cancellation and finish safely.
Complete low-resource compatibility is a release requirement, not a claim made
by the present development build.

## Current position

**The raster development checkpoint is delivered. Active feature development
can proceed beyond M3; its outstanding qualification is tracked separately.**

At source checkpoint `55937cf`, the application has brush/erase, affine layer
transforms, source-preserving crop/canvas resize, hard rectangle/ellipse
selections, basic linked raster masks, 24 blend modes, seven pixel adjustments,
undo/redo, native save/reopen and PNG/JPEG export. Progress and queued-job
cancellation are connected to the UI. Layered CPU rendering, Vulkan
presentation, admitted immutable backing, session spill and streamed native IO
are implemented.

Evidence: 41 default tests passed; GCC and Clang core CI passed; an offscreen
photo workflow and 15-step app demo passed. A retained-history 4000×3000
photo-adjust/save/reopen/export observation passed in about 24 seconds, with
about 401 MiB highest sampled process RSS. See [VALIDATION](VALIDATION.md) for
exact scope. Earlier native/GPU results belong to their recorded checkpoints.
The latest feature checkpoint still needs native input/frame verification.

Pixel adjustments are destructive commands, not revisable adjustment layers.
Canvas resize does not supply a complete image-resampling workflow. Current ICC
import handling is not professional working-profile/proof/output support.
Session spill is not crash recovery. The current RGBA16F contract does not prove
precision-preserving integer16/FP32 editing.

## M3 boundary and remaining acceptance

M3 previously mixed renderer implementation with tests that require later tools,
interchange and recovery. Waiting for all those features before leaving M3 makes
milestone sequencing stall. The new boundary is explicit:

- **Implementation handoff:** renderer/layer/storage mechanisms are delivered
  for the current raster contract, with the scoped evidence above. Future
  editing packets may use them now. Fix reproduced workflow defects as they
  arise; do not add unrelated features or unavailable hardware work to this gate.
- **Original expanded qualification:** remains incomplete. Its unmet checks are
  retained below and scheduled when their dependencies and environments exist.
  This split changes scheduling; it does not declare the old full M3 exit passed.

| Gate | Current status | Execution point / responsibility |
| --- | --- | --- |
| H1: latest native CPU UI, accepted frames, tool/dialog input, tab cancellation and edit/save/reopen/export | Pending on `55937cf`; offscreen equivalents passed | Next desktop verification; coordinator |
| H2: current-source native Intel/Vulkan comparison, validation and fallback lifecycle | Earlier checkpoint evidence only | Renderer qualification lane; renderer owner |
| Q1: exact P08 10,000-layer / 600-event protocol, handler p95 ≤4 ms | Recorded native aggregate failed; no replacement pass | Reproduce and isolate handler cost; UI/performance owner |
| Q2: full P03 4K viewport, eight full-cover 4096² layers, four masks and fixed pan/zoom trace | Pending; basic masks now exist, full fixture/trace and reference timings unexecuted; original discrete-GPU p95 ≤16.7 ms retained | Composite integration, then available hardware qualification |
| Q3: full P07 10,000² image, 100 genuine brush edits, save/reopen and exact canonical tiles | Pending; basic brush now exists, full scenario unexecuted | Paint/storage integration; original idle RSS ≤6 GiB on 32 GiB references remains separate |
| Q4: V08–V10 real draft/filter/effect and noise consistency | Pending actual feature implementations | Photo/composite integration |
| Q5: full LP01–LP05 / P17–P21 on the actual laptop, CPU and iGPU, normal desktop coexistence | Partial component/12 MP evidence only; richer tools, large PSB and recovery remain dependencies | Each feature packet measures its scope; final release runs complete scenarios |
| Q6: required AMD/NVIDIA/Intel reference-class and X11 evidence | Pending unavailable or unexecuted environments | Release qualification; missing environments remain explicit blockers |

No tiny fixture substitutes for a full workload, offscreen timing substitutes
for native timing, or configured memory limit substitutes for observed usage.
Keep the existing exact protocols and limits; a future change must be named and
reviewed as a requirement revision, rather than described as a passing test.

## Outcome-based phases

Phases overlap only where their interfaces are independent. Every packet must
connect its UI, undo, native persistence and required export behavior. A failed
or canceled operation must preserve the previous committed document.

| Phase | Deliverable | Finite finished-work gate |
| --- | --- | --- |
| D1 — Raster development delivery | The delivered editing tool set; bounded failure/cancellation; remaining H1/H2 checks | Import → paint/erase → transform → selection/mask → adjustment → undo → native save/reopen → PNG/JPEG through real UI. Record native/fallback results separately. No professional replacement claim. |
| D2 — Revisable photo editing | Persisted adjustment parameters/layers, live drafts, histogram, per-channel curves, soft masks, clone/healing and defined image resampling | Retouch and grade a portrait/product; reopen, revise mask boundaries and adjustment/retouch strength, and export without reconstructing flattened edits. |
| D3 — Professional compositing | Selection algebra/feathering/lasso, richer masks/clipping, isolated groups, editable source objects, effects and source-preserving transforms | Build a ten-source advertising composite; reopen, replace a source, revise placement/grade, retain required fine-edge/translucent detail and export. |
| D4 — Editable graphic design | Point/paragraph text and shaping, vectors/paths, guides, alignment/distribution, layout variants and persisted styles | Create poster/social variants; reopen and change wording/font/layout. Text remains text, vectors remain vectors and style parameters remain editable. |
| D5 — Managed photo, interchange and print | Qualified RAW source/development, integer16/FP32 and color policies, ICC working/proof/output support, named PSD/PSB profiles, CMYK and print TIFF/PDF | Develop a retained RAW source to qualified high-depth output; satisfy a named print-provider brief; separately execute the declared Linux → Photoshop → Linux editable handoff in actual Photoshop. |
| D6 — Replacement release qualification | Complete declared workflow profiles, durable recovery, batch/actions, install/update and platform/hardware acceptance | All frozen photo/composite/design/print briefs accept revision requests after reopen and meet quality, editability, resource and latency gates. Publish tested compatibility profiles and exclusions. |

Define precision/color and persisted parameter contracts **before** new kernels
or file representations depend on them. Preserve old RGBA16F appearance and old
project compatibility. Unresolved new-format decisions block the affected
packet, rather than all unrelated work. Bump the native schema when saved
representations change; enum declarations alone do not implement capabilities.

Implement recovery early alongside revisable editing, before promising durable
work. D6 qualifies recovery and automation; it is not permission to postpone
all their implementation until the final phase. RAW, text and print introduce
separate dependency/fixture decisions and cannot inherit raster-only evidence.

## Next bounded work queue

1. Execute H1 when the native desktop is available, and record H2's precise
   current-source gap. These checks do not prevent independent D2 implementation.
2. Freeze D2's persisted adjustment/draft contract and its portrait/product
   fixture, expected deliverables and revision request before coding.
3. Deliver the first D2 packet: one revisable adjustment with live preview,
   commit/cancel, undo and native reopen. Integrate immediately when it passes.
4. Deliver a bounded histogram and extend the same contract to Levels/Curves/
   Color Balance, with UI controls and saved parameter revisions.
5. Add soft selection/mask editing and clone/healing as independent packets;
   integrate them into the frozen photo brief. Start durable recovery in parallel.
6. Run the actual photo brief and record its measured 8 GB limits; then begin D3.

Do not reopen M3 for each new tool. Renderer optimizations enter the active queue
when a reproduced defect or measured workflow bottleneck makes them necessary.

## Resource and coordination rules

- The 8 GB baseline applies throughout: actual host headroom enabled, normal
  desktop apps retained, CPU-only completion, admitted transient/resident bytes,
  observed process/GPU/disk usage and safe refusal/cancellation.
- Preserve the 512 MiB system availability floor and 2 GiB application envelope
  in the published low-resource contract. Full latency/coexistence gates remain
  as specified in [low-resource acceptance](docs/low-resource-compatibility.md).
- Parallel agents receive separate file ownership and explicit shared interfaces.
  One coordinator owns document/schema contracts, integration and serial local
  builds/tests. Integrate completed packets continuously into usable checkpoints.
- Each handoff records source checkpoint, fixture, executed checks, pending
  checks and concrete blocker. A feature packet finishes when its defined gate
  passes; new requests enter another packet instead of extending it silently.
- Estimate remaining work from measured packet throughput. Do not promise a
  Photoshop-level completion date before the major dependencies are resolved.

The [editing guide](docs/today-editor-guide.md) describes today's controls.
The [native format](docs/project-format-v3.md) defines current schema semantics.
