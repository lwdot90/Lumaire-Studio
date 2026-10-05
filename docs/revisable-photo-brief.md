# Revisable photo editing brief

**Status: D2 scoped development acceptance passed October 4, 2026.**
Both fixed portrait/product workflows, independent saved-project checks, visual
review and settled saved controls passed. The [acceptance record](d2-photo-acceptance.md)
preserves source identity, measured resources, artifacts and earlier failed runs.

This is the frozen D2 workflow brief: edit a portrait or product photograph,
save it, then satisfy a revision request without starting again from flattened
pixels. It defines the acceptance requirements for revisable photographic editing.
The initial implementation packet supplied **Revisable Exposure**. Subsequent
packets add ordered retained adjustments for all seven operations, including
an RGB master and independent Red, Green and Blue Curves. See the
[revisable-adjustments guide](revisable-adjustments-guide.md) and
[per-channel Curves guide](per-channel-curves-guide.md). Current native project
format 6 retains these settings, retouch strokes and source pixels; older formats
remain readable. Clone/healing, soft mask editing and explicit image resampling
are integrated. Their photograph-based acceptance is recorded separately in
[photo acceptance](d2-photo-acceptance.md).

## Inputs and machine

Use two fixed, redistribution-safe 8-bit RGB PNG/JPEG photographs: a portrait
with skin, hair and shadow detail, and a product with neutral background,
colored packaging and a small distracting blemish. Each is 4000 × 3000 pixels.
Record filenames, SHA-256 hashes and usage rights before execution; keep those
exact inputs for later comparisons. The exact photographs, hashes and rights are frozen in the
[D2 completion checklist](d2-completion-checklist.md). Do not replace photographs with tiny test images to pass this brief.

Run both on the user's approximately 8 GB i5-8265U/UHD 620 laptop, with normal
desktop applications retained. Forced CPU operation must complete the same
editing and output workflow. Keep actual memory admission and the 512 MiB
system-headroom floor enabled. Record process/resource peaks, elapsed times,
disk use and safe refusal/cancellation. This brief does not establish RAW,
high-depth, working-profile, proofing or print capability.

## First packet: retained-source exposure

1. Import a photograph and create an editable native project. Keep its original
   source, resolution, layer placement and profile metadata.
2. Open **Adjust → Revisable Exposure…**. Preview `+1 EV`, then change to
   `+0.5 EV`. The final result must evaluate the retained source at `+0.5 EV`,
   rather than compound the two exposures. Preview changes must not mark the
   document dirty or create undo entries.
3. Cancel a preview and verify that the previous committed image and history
   are restored. Accept a changed value and verify one Undo/Redo step.
4. Save, close and reopen the native project. Reopen the same adjustment and
   revise its saved exposure to `+0.25 EV`; the original source must remain
   available. Confirm that any layer mask and opacity still scope visibility.
5. Export PNG and JPEG. Compare decoded output with the accepted composition;
   PNG retains transparency and JPEG uses white behind transparent pixels.

The exposure dialog's histogram is a bounded sample of the selected raster,
not a full-image analysis or an ICC-managed composite histogram. Whole-layer
revisable exposure is distinct from the existing selection-limited destructive
Exposure command. Rasterization explicitly discards retained adjustment
parameters; it is not a required shortcut for passing a revisable workflow.

## Full D2 revision request

The complete photograph-based workflow requires the following steps on both
fixed photographs:

- Retouch the distracting blemish using clone/healing on a separate layer.
  Preserve the underlying photograph. After reopen, change retouch strength
  using that layer's opacity and continue retouching without flattening it.
- Grade with retained Levels, Curves and Color Balance parameters and live
  preview. After reopen, revise a black point, a curve control and warmth/tint;
  evaluate from retained sources and preserve parameter order. The current
  implementation supplies the retained ordered controls and per-channel
  Curves; their inclusion must be checked in the actual portrait/product
  delivery record. Verify changes to individual channels independently of the
  RGB master after reopening.
- Use a soft mask to restrict the grade. After reopen, revise its boundary and
  softness, enable/disable it and undo those changes without losing coverage.
- Apply the defined image-resampling workflow for a smaller delivery size.
  Preserve the original source and document the sampling/output contract.
  Use Image size for pixel resampling; Canvas size changes affine geometry
  and does not supply this requirement.

For both photographs, issue that revision request after the application has
closed and reopened the saved file. Existing source, mask, retouch layer and
adjustment parameters must remain editable; matching a flattened preview alone
does not pass.

The fixed scored recipe is in the [completion checklist](d2-completion-checklist.md).
It preserves original-size A and requested revision B, including B's reduced
retouch strength and layer opacity. Continue cleaning the same targets into a
separate original-size polished C before resampling; C must retain the earlier
editable records and the exact base photograph. Keep B intact so a polished
export cannot substitute for the requested revision evidence.

## Required delivery record

Retain the source hashes, application commit and machine/backend information;
the before/after revision `.cproj` files; PNG/JPEG exports; and screenshots of
the adjustment controls, mask/retouch layers and revised result. Label a
widget-plus-renderer capture accurately if it is not a desktop screenshot.
Record executed tests and measured resource/time traces separately from visual
acceptance. Keep failed, refused, canceled and unexecuted cases explicit.

Numerical checks must independently verify exposure/source preservation and
canonical reopen. Visual review checks skin/product color, edge halos, unwanted
clipping, blemish removal and visible revision effects against the fixed brief.
Future tools need their own fixed numerical fixtures before adoption.

Both fixed photographs now satisfy the full frozen D2 request: A remains editable,
B preserves the requested reduced-strength revision, C continues cleanup without
losing B's records, and the Lanczos delivery reopens with editable constructs.
The final primary run passed 4 cases in 204191 ms; independent saved-project
review passed 4 cases in 301410 ms. All 61 default tests passed in 31.90 seconds.
Logs, source fingerprint and measured 8 GB/CPU resources are in the acceptance
record; full-size renders, projects, attribution and captures are retained under
`build/d2-photo-native-20261004-final/`.

The literal +1/+0.5/+0.25 EV sequence above documents the earlier retained-Exposure
packet. The final full-photo stack recipe uses +0.2 then +0.1 EV, as frozen in the
checklist; it is not claimed to rerun those earlier exact values on both inputs.
The export oracle samples 4096 pixels and is supplemented by delivery-size and
100% visual assessment. Source JPEG softness and clipped bright lights remain;
Heal is bounded texture transfer, not content-aware removal of broad objects.
Broader LP8 qualification, recovery, professional color/print and Photoshop
interchange remain separate roadmap gates. This brief claims no Photoshop parity.
