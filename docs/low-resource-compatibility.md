# Required low-resource compatibility — LP8

> Export note: Recorded validation results are historical source-repository observations, not qualification of this standalone repository. Commands use the standalone layout.

User requirement confirmed 2026-10-02: the Photoshop alternative must work on low-resource computers, including the user's own laptop. This extends the confirmed photo editing/compositing/graphic design/print target; it does not remove capabilities or lower final image quality. See the development plan (historical source-repository record; not included) and finished-work acceptance (historical source-repository record; not included).

**LP01–LP05/P17–P21 are mandatory release gates and currently pending.** This document specifies future implementation/qualification. Read-only hardware inventory is not an application benchmark; no performance result is claimed here.

## Actual primary baseline

Verified locally with `lscpu`, `free -b`, `lspci -nn`, `df -B1`, `uname -r` and `/etc/os-release` on 2026-10-02:

| Item | Observed baseline |
| --- | --- |
| CPU | Intel Core i5-8265U, x86-64, 4 physical cores / 8 logical threads; AVX2/F16C available but optional for the product |
| Physical RAM | 7,948,062,720 bytes, about 7.4 GiB usable / 8 GB installed |
| Graphics | Intel UHD Graphics 620, PCI 8086:3ea0; integrated/shared system memory, no dedicated GPU listed |
| Storage | Toshiba XG6 NVMe controller; project filesystem is local disk |
| Temporary filesystem | `/tmp` is tmpfs; large spill/undo/decoder stores must use a disk-backed location |
| OS/kernel | Fedora Linux 44 Workstation; 7.2.5-200.fc44.x86_64 |
| Snapshot conditions | About 1.68 GiB MemAvailable and 1.72 GiB swap used during this inventory; these are transient desktop conditions, not minimum hardware or performance evidence |

The existing hardware record (historical source-repository record; not included) retains historical driver/desktop information. This audit did not requalify Vulkan features, display timings, power/thermal behavior or driver versions. Each measured run captures them again. The machine is now a separate required compatibility class, not a substitute for faster GPU-class throughput tests.

The application must run without Vulkan, FP64 GPU support, discrete VRAM or GPU inference. Native iGPU acceleration is used where qualified; CPU dispatch supplies the same required tools, files, parameters and final output. An 8 GB computer is the confirmed lower-resource reference; support claims for still smaller configurations require separate evidence.

## Resource contract

Numbers below are unmeasured initial engineering targets frozen with the workload/profile during M0-R.6. They are ceilings, not allocations made at launch.

| Resource | LP8 policy |
| --- | --- |
| Total memory | Idle application target ≤ 300 MiB; routine-job steady target ≤ 1.5 GiB; combined peak ≤ 2 GiB. Include main/child processes, toolkit/fonts, sources/history, codec/RAW/filter/inference transients and GPU allocations not already represented. Account for shared backing once when overlap is proven; conservatively count/report uncertain overlap. |
| Derived caches | Initial CPU ceiling 384 MiB and shared-GPU ceiling 128 MiB; both inside the total envelope, resized downward under pressure. Do not allocate each pool's maximum in advance. |
| Transfer/history | Staging/readback ceiling 64 MiB, resident history payload 128 MiB. Preserve the existing 100-command/retained-payload policy with disk-backed history rather than forcing all undo bytes into RAM. Sole unsaved/current/recovery copies cannot be discarded. |
| Admission | Before starting an operation, reserve a bounded peak against both the app envelope and current MemAvailable; preserve a 512 MiB availability floor. Include outstanding reservations. Evict/spill first; serialize or suspend new heavy allocations when headroom is absent. A setting is not evidence until actual peaks and pressure cases pass. |
| Compute | Start with two compute workers and one memory-heavy compute job, coordinating nested codec/RAW/ONNX/tile pools. A bounded IO writer handles recovery/save. Interactive jobs preempt/yield background batch, decode, thumbnail and mip work. Record sustained thermal behavior; tune from measurements. |
| Disk | Lazy/disk-backed originals and decoded tiles, compressed session/undo payloads and streaming save/export. Use verified disk-backed XDG cache/state storage, preflight estimated source/temp/recovery/output space plus reserve, and preserve the prior project on failure. tmpfs/swap are not spill-storage plans. |
| Preview | Viewport-sized progressive previews and lazy mips/tiles with bounded queues. Label provisional/proxy results; only the current request can replace them. Final rendering uses canonical sources/settings/depth/profile and cannot inherit proxy pixels. |
| Inference | On-demand local CPU model; initial inference-worker peak ≤ 1 GiB inside total ≤ 2 GiB. Release derived caches and serialize heavy work before loading. Model/precision changes require the same independent quality/category/license gates. |

Current decoder/filter adapters that materialize full images or hidden library pools must be measured and replaced/adapted where they cannot fit. "CPU fallback" alone does not establish low-memory support. A single-layer full-resolution buffer may fit, but multiple full copies of every layer cannot be an accepted default strategy.

For large files, latency and completion time can differ from workstation targets. Every feature still produces its required result through bounded/tiled/disk-backed processing. If an algorithm cannot run within the envelope, its LP gate stays open until a suitable implementation is supplied. Do not declare a low-resource edition complete by disabling RAW, masks, filters, print, source objects or inference.

## Required acceptance and workloads

All rows run with native iGPU and forced CPU where applicable, real Wayland and X11, release builds, relevant RGB8/16/FP32/CMYK cases and the same native/history/color/output semantics. Benchmark IDs map to the main plan. Original CPU/Vulkan numerical fixtures remain independent checks.

| ID | Required cases and gates | Benchmark / owner | Status |
| --- | --- | --- | --- |
| LP01 Routine editing | 4000×3000, eight full-cover raster layers, two masks, two adjustments and text, viewport no larger than 1920×1080. Import, paint 40/256 px, mask/transform, grade, undo, save/reopen and required export. Input handlers p95 ≤ 8 ms; input-to-visible brush and warm pan/zoom p95 ≤ 100 ms; steady memory ≤ 1.5 GiB, peak ≤ 2 GiB. Freeze storage/profile/assets before scoring; old 100 MP legacy limits do not qualify the richer fixture. | P17; M3.8/M4/M5/M10.7 | pending |
| LP02 Photo/design/print | W01–W07 representative deliverables at their declared routine size, including 24 MP RAW/RGB16/CMYK, rich text/paths and PSD handoff. Current reduced preview p95 ≤ 750 ms; specified full-resolution develop/grade/export cases ≤ 30 s; bounded peak ≤ 2 GiB. Retain originals, editable parameters/text/paths, required precision/profile and print/channel intent. Model operations have LP04's separate timing. | P18; M7.9/M12/M10.7 | pending |
| LP03 Large work and pressure | Full W06/W08 58 MP/29-layer banner plus separate sparse 300,000-axis PSB extent case; low caches, disk-backed sources/history, filter/save/reopen/export and resource failure. No mandatory crop/flatten, OOM or unsaved-data loss. Input handlers p95 ≤ 8 ms, progress within 250 ms, cancellation acknowledgement ≤ 250 ms; cancellation stops scheduling new work while in-flight bounded work retires safely. Record large-job times and disk footprint without applying workstation frame targets. | P19; M3.8/M9.9/M12.4/M10.7 | pending |
| LP04 Local intelligent tools | Background extraction and click-target Object/Subject at the frozen 1024-long-side quality evaluation; test categories, cancellation, stale tab and worker teardown. CPU p95 ≤ 15 s, inference-worker peak ≤ 1 GiB and combined app peak ≤ 2 GiB. Original IoU/boundary/category and editable-mask checks remain unchanged. No dedicated GPU or cloud-service requirement. | P20; M8.9/M10.7 | pending |
| LP05 Desktop coexistence/recovery | Cold launch ≤ 3 s and idle app memory ≤ 300 MiB; routine editing while normal desktop applications stay open, with ≥1.5 GiB starting MemAvailable. Adapt budgets to preserve 512 MiB availability floor; reproduce lower-headroom/cache/disk cases without killing the user's other apps or altering swap/desktop settings. GPU absence/loss, close during work, save A while editing B, completed-edit recovery and safe cache cleanup all pass. No unbounded app-attributable swap growth or OOM. | P21; M0-R.6/M9.9/M10.7 | pending |

Maximum brush/huge-filter cases remain correctness requirements; P17's practical routine latency target uses the stated 40/256 px brushes. Increasing brush/document size cannot silently reduce precision or skip completion. Progress/cancel remains responsive while heavier work completes.

## Qualification protocol

1. Freeze fixture hashes, logical/physical viewport size, profiles/fonts, filter settings, thermal/power condition and exact memory/timing boundaries. Record the actual CPU/RAM/driver/kernel/protocol, available memory, existing swap and disk condition before/after each run. Include sustained plugged-in and battery sessions; clearly separate cold/warm and app-only/coexistence results.
2. Measure process-tree RSS/PSS, tracked allocator/GPU/live/derived/transient/child bytes and conservative combined charge; report overlap and anything not directly measurable. Cache counters alone cannot pass total-memory targets. Record allocator high-water marks, reserved peaks, spill/read bytes, major faults/swap growth and system availability.
3. A process-memory-limited/headless run adds failure coverage but cannot replace actual iGPU/shared-memory, GUI, tablet, thermal or coexistence evidence. Existing swap is recorded; app-induced sustained thrashing cannot be disguised as successful low-memory support. Do not run destructive/full-memory stress during ordinary use without a separate execution task.
4. Quality and source/editability assertions are shared with C01–C12/W01–W08. Where task-time comparison is available, compare similarly constrained hardware and the same brief; a faster Photoshop workstation is not the low-resource timing oracle. Missing comparison evidence remains pending.
5. Retain pending/fail/pass per LP/P subcase and environment, output hashes, editability report, time/memory traces and failures. Larger jobs can settle more slowly, but required save/reopen/export/cancel paths must execute safely on this baseline. No faster-machine, source-review or memory-cap configuration result counts as the final LP8 pass.

## Implementation order and release rule

M0-R.6 freezes the contract before new precision/tool dependencies are selected. M3.8 supplies resource admission/storage/render foundation; M4/M5 verify interactive editing; M7.9 and M8.9 qualify processing/model peaks; M9.9 supplies coexistence, disk and lifecycle behavior; M10.7 executes the complete product tests. Early renderer/resource component results do not claim that tool-dependent LP01/LP02 scenarios already pass.

The replacement release requires LP01–LP05/P17–P21 and the complete confirmed feature/output profile. This computer is a first-class target. Published requirements and known limits must accurately state its tested workload sizes, resource envelope and heavier-job behavior; they cannot suggest that merely installing the application establishes useful compatibility.
