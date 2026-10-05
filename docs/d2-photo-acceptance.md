# D2 photo workflow acceptance record

**Status: D2 development workflow acceptance passed.**
The integrated implementation, all 61 tests and corrected primary
portrait/product A/B/C and delivery workflows passed. Earlier v3 images failed
retouch quality, which prompted the preserved recipe corrections below.
Independent final saved-project checks, final 100%-size visual assessment of
the frozen scope and settled control-capture verification also passed. This
closes D2's defined development workflow, with the qualification limits below.

## Build and machine

The source is an uncommitted development worktree based on commit
`33f273d4b4ca4f5b0635edc1a84a6bdd42a137ef`. The final corrected-recipe snapshot
records 205 source files, fingerprint
`12269649394c77962d6d7fd6bf0d09378a42b06d64000ec14a21a90fa4e9ec5c`,
and application SHA-256
`ef6023b57a603df10724cd2d4d5a862c075a92d878eefcd59d858f0b497a1760`.
See `build/release/d2-source-state.json`. The historical v3 pressure-fix snapshot
records 205 source files, fingerprint
`9304e20ae67e2b8c8c494efde40346cc36830dc04be314f4f140416c3f95070e`,
and application SHA-256
`ef6023b57a603df10724cd2d4d5a862c075a92d878eefcd59d858f0b497a1760`.
See `build/release/d2-source-state.json`. These identify the measured
development snapshot, not a published release. Earlier native v1/v2 evidence
used source fingerprint
`163c46471218554647bc8a9b9525b55e33aab4eabdfcce1d3331d1c285826762`
and application SHA-256
`18d774b0dfdf2f99b38e21a5bf03b6841ad71129446f4a326a8933aa17442867`.

The target machine is the approximately 8 GB Intel i5-8265U/UHD 620 laptop.
The photograph workflow forces the CPU backend, uses actual host-memory
admission and retains the 512 MiB system-headroom floor. Normal desktop
applications remain running. Builds and tests run serially.

## Fixed photographs and rights

Both inputs are original, unmodified 4000 × 3000 RGB JPEG files. Their local
provenance and attribution records are under `build/d2-photo-fixtures/`.

| Input | SHA-256 | Author and rights |
| --- | --- | --- |
| `portrait.jpg` | `84496329b251241f0ed8238c27d1b077ab9b3cf34b85f12893c208b70f1ad0da` | Doormaind, [Woman seated in office chair in saree.jpg](https://commons.wikimedia.org/wiki/File:Woman_seated_in_office_chair_in_saree.jpg), [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/) |
| `product.jpg` | `6c9cfc43bfaf2086bfbb085c4a03fc71eba38caabe6fe403a7bc191bd5d0d029` | Candeadly, [Orbitz bottle.jpg](https://commons.wikimedia.org/wiki/File:Orbitz_bottle.jpg), [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) |

Edited portrait derivatives require CC BY-SA 4.0 attribution and change notices;
product derivatives require CC BY 4.0 attribution and change notices. These
licenses do not change the application source license. No endorsement is
implied. Acquisition and provenance were recorded on October 3, 2026.

## Finite acceptance checklist

The [frozen photo brief](revisable-photo-brief.md) requires both photographs to
complete the following sequence through the integrated editor, with numerical
and visual evidence:

1. Import the exact photograph, retain its original source and duplicate a
   separate retouch layer. Apply Clone and Heal strokes without changing the
   underlying photograph.
2. Create a separate grade layer with retained, ordered Exposure, Levels,
   Curves and Color Balance settings, a soft elliptical mask, and live preview.
   Undo/Redo must restore the accepted state.
3. Save, close and reopen the full-resolution native project. Verify exact
   canonical original pixels, retained settings, layer identities and masks.
4. Revise recorded retouch strength and layer opacity; add another Heal stroke.
   Revise mask boundary and softness, toggle its enabled state and undo it.
   Revise Levels, an individual channel curve and warmth/tint. Cancel another
   preview without changing accepted document identity or history.
5. Save a separate full-resolution revision. Resample a delivery copy to
   2000 × 1500 with Lanczos, verify Undo/Redo, save/reopen and verify retained
   sources, operations, masks, transforms and layer opacity.
6. Export PNG and quality-95 JPEG. Verify decoded output, retain the original,
   revision and delivery `.cproj` files, and inspect both revised photographs
   for color, halos, clipping, blemish removal and visible revision effects.
7. Record elapsed time, sampled process RAM, admitted RAM, spill usage and
   post-release cleanup. Complete the supplemental verifier and visual review.

The native recipe starts with Exposure `+0.2 EV`, Levels black point `5`, RGB
curve midpoint output `0.53`, Red midpoint `0.515`, warmth `3`, and tint `-2`.
After reopen it changes Exposure to `+0.1 EV`, black point to `3`, Red midpoint
to `0.505`, warmth to `2`, and tint to `-1`. The first retouch stroke changes
to 40% strength; the retouch layer changes to 65% opacity and receives a further
30%-opacity Heal stroke. The soft selection starts at radius 32 source-scale
document pixels, changes to 48 with a shifted/narrowed boundary, then the mask
is feathered at radius 8. The exact image-specific coordinates and independent
checks are in `tests/d2_photo_workflow_test.cpp`.

## Executed checks

The coordinator's serial release build passed. All **61 default CTests passed
in 33.40 seconds**. Native Wayland checks independently passed:

| Check | Result | Evidence |
| --- | --- | --- |
| Mask editing and image resampling | 4 cases passed, 2005 ms | `build/release/d2-native-mask-resize.txt` |
| Retouch tools, reopen, strength revision and export | 3 cases passed, 1807 ms | `build/release/d2-native-retouch.txt` |

These are development integration checks, not the full photograph acceptance.
After a test-harness retirement-fence correction, the actual portrait rerun
**passed** in 100926 ms. Its sampled RSS peak was 955269120 bytes (about
911 MiB), admitted peak 766536237 bytes, and spill peak 173178880 bytes across
341 entries. JPEG mean sampled channel error was 0.9423828125. After the worker
retirement fence, cleanup reported zero bytes, entries and failures; the test
also asserted zero active IO. This v2 portrait pass did not qualify its failed
product row or visual acceptance. See `build/release/d2-native-photos-v2.txt`
and the artifacts under `build/d2-photo-native-20261004-v2/`.

The retained first-attempt log, `build/release/d2-native-photos.txt`, records a
failed portrait post-release assertion with 152485888 charged spill bytes.
The harness now waits for worker retirement before garbage collection; no
application-code change was needed for this correction. The earlier failure
does not establish an application memory leak. The product v2 attempt failed
while preparing the draft that was to be canceled: it reported **Application
memory envelope exhausted**. Immediately before that step, elapsed time was
69643 ms and sampled RSS was 1509433344 bytes. This is a failed acceptance
attempt, not a successful cancellation or complete product delivery.

A subsequent pressure fix in `core/resources.cpp` releases allocator pages
after spill reclamation. That change built successfully, and all **61 default
CTest checks passed in 31.29 seconds** afterward; see
`build/release/d2-pressure-ctest.txt`. On this snapshot the fresh v3 portrait
**passed** in 87931 ms, with sampled RSS 908017664 bytes, admitted peak
766536237 bytes, spill peak 173178880 bytes across 341 entries, and JPEG mean
sampled channel error 0.9423828125. Cleanup bytes, entries, active IO and
failures were all zero. The v3 product **passed** in 107854 ms, with sampled
RSS 1413111808 bytes, admitted peak 793114333 bytes, spill peak 181276672 bytes
across 358 entries, and JPEG mean sampled channel error
0.9567057291666666. Cleanup bytes, entries, active IO and failures were all
zero. Both primary photograph rows, setup and teardown passed: **4 Qt cases,
195961 ms**, in `build/release/d2-native-photos-v3.txt`. The earlier
`before_cancel` product observation (63231 ms, RSS 1370333184 bytes) was an
intermediate measurement, not the final resource peak.

Supplemental v3 saved-project verification passed **4 Qt cases in 245397 ms**;
see `build/release/d2-native-saved-review.txt` and the detailed coverage below.
It does not supersede the failed visual assessment or verify the corrected
recipe. The changed photograph test's synthetic smoke run previously passed
again in 7.47 seconds.

## Scope and limitations

Heal is bounded local texture transfer with a 1–32 pixel neighborhood, not
content-aware fill. Retained retouch is capped at 16 strokes per layer.
Strength and layer opacity remain editable after reopening. Saved mask coverage
can be repainted and feathered; feathering is destructive coverage processing,
not a retained radius parameter.

Resampling replaces the accepted retained source with resized pixels and loses
detail on reduction. The separate full-resolution revision project preserves
the pre-resize source; reopening the smaller delivery project does not retain
session Undo history. Adjustment/retouch records remain editable at the new
sample grid. See the [tool guide](d2-photo-tools-guide.md).

Canonical raster/source/mask digests cover their complete stored pixels, but
the decoded export oracle samples a 64 × 64 grid (4096 image positions), not
every export pixel. It allows one 8-bit level for PNG channels/alpha and checks
JPEG mean channel error below 8 against linear white compositing.

Workspace images combine a Qt widget capture with the CPU renderer presentation;
they are not physical-desktop screenshots. They require separate visual review.
No new GPU qualification, sanitizer run, broader LP8 qualification, recovery,
RAW/high-depth or professional ICC/print acceptance is claimed by this record.
Changes remain uncommitted.

## V3 visual rejection and next recipe

The independent native saved-project verifier passed all four Qt cases in
245397 ms; see `build/release/d2-native-saved-review.txt`. It checked exact A/B
source and parameter preservation, unaffected curve channels and retouch
records, full-size PNG output, saved controls, delivery reopen and exact
soft-mask history restoration. Both photographs released all spill charges.

Visual inspection rejected retouch quality despite those numerical passes.
The portrait bottle clone used an unsuitable chair-texture donor, leaving a
dark patch, and the keys/wires healing smeared the distraction. The product
scratch remained; the yellow wall mark was only softened. Color/detail and
soft grade transitions were acceptable at delivery size. Comparison crops are
retained under `build/d2-photo-native-20261004-v3/review-agent/`.

The [corrected recipe](d2-completion-checklist.md), frozen October 4 before
another scored run, preserves the same targets and uses matching clean
partition/table/wall donors with coverage following the actual distractions.
Initial A stores three strokes: main Clone, secondary Heal and secondary Clone
cleanup, each at full strength and 25% hardness; Heal radius is 32 pixels.
The portrait bottle uses the final zigzag path to y1253 and recorded feather-8
selection `(3125,1055,125,212)`; the product main scratch uses clean donor
`(3100,675)` over the complete path. Exact coordinates and diameters are frozen
in the checklist. Layer names are `Original photograph`, `Retouch` and `Grade`.
Requested B changes the first stroke to 40% strength, the retouch layer to 65%
opacity and appends a 30%-strength Heal continuation, yielding four retained
strokes. It intentionally reveals part of the distraction. Polished C restores
layer opacity to 100% and appends two full-strength Clone cleanup strokes,
yielding six retained strokes without replacing B's records. Separate original
size A, B and polished C projects must be saved; only C is resampled for
delivery. The product's separate right-side scratch network is outside the
selected target and is not claimed removed.

The corrected test build passed and all **61 default CTests passed in 34.03
seconds** (`build/release/d2-recipe-ctest.txt`). Later final-run evidence follows
below; the v3 hashes and rejection records remain historical. At that stage D2
remained incomplete until both corrected photograph outputs passed numerical,
saved-project and visual acceptance.

### Fast corrected-recipe probe: further visual failure

A full-size, actual-host native probe of the first corrected recipe completed
under ignored `build/d2-recipe-probe/`. Its A/B/C images still showed conspicuous
hard pale capsules at portrait bottle/keys. Thus the 85%-hardness donor recipe
was rejected before spending another full acceptance run on it. This is a
visual recipe failure, not evidence that retouch records or native IO failed.

Before the next probe, freeze a softer recipe: hardness 25%, dense bounded
paths and a clean donor offset following the tabletop's perspective gradient.
Portrait bottle uses diameter 130, zigzag paths at x3180/3200/3220 from y1110
to1240, source (2880,1110), and a recorded feather-8 rectangular selection
(3125,1055,125,200) protecting the tabletop and adjacent chair. Keys use
diameter100, points (600,1435),(800,1430),(800,1450),(600,1455),(600,1475),
(800,1470), source(440,1465): offset(-160,+30) replaces the overly bright
(-160,+65) donor. Product main keeps the full scratch path, diameter150; its
yellow mark uses diameter160 and the same nearby clean wall donor. A3/B4/C6
record counts and preserved revision B remain unchanged. At that point this
softer candidate had not passed and was not yet the scored test recipe; later
probe outcomes follow below.

The soft v2 probe removed keys and the product wall mark without hard capsules,
but left a blue bottle-base strip and the ends of the selected main scratch.
The next v3 probe was frozen before execution: extend the bottle zigzags to
y1253 and its selection to height212 (bottom1267); retain feather8. Start the
product main path at (3700,675), source(2900,675), keep the prior path and append
(3200,1280),(3130,1310),(3070,1335). All source offsets, softness and other
coverage remain unchanged. Verify the tabletop edge separately; this extension
must not remove it merely to hide the bottle. Earlier rejected outputs remain
available.

The v3 quick probe passed portrait A/C target removal and preserved the desk
edge. The product main scratch was removed, but extending its path with an
800-pixel donor offset brought bottle texture into the lower wall. Reject that
new artifact. Before a product-only v4 probe, freeze source(3100,675), offset
(-600,0), so the same lower endpoint samples clean wall at(2470,1335) rather
than glass at(2270,1335). Keep all target points and softness unchanged. This
retains the entire selected scratch instead of shortening it to avoid failure.

### Final recipe probe results

The portrait v3 A/C probes and product v4 A/C probes passed the coordinator's
100%-size target review. Bottle/keys and the selected main scratch/yellow mark
were removed without the rejected capsules, bottle-foot strip or contaminated
walking donor. Both probes exited zero with clean retirement. These are
full-size recipe-quality probes, not the scored grade/reopen/export acceptance
workflow. The earlier failed outputs remain retained.

The final frozen recipe above keeps soft 25% hardness and A3/B4/C6 retained
stroke counts, preserves requested revision B separately, and saves polished C
before resampling. It is applied to the checked workflow tests: the final build
passed and all **61 default CTests passed in 31.90 seconds**
(`build/release/d2-final-ctest.txt`). The corrected actual-photograph workflow
passed both rows, setup and teardown: **4 Qt cases in 204191 ms**. Evidence is
`build/release/d2-native-photos-final.txt`, and artifacts are under
`build/d2-photo-native-20261004-final/`. That directory preserves the fixture
provenance and attribution, original-size A, requested revision B, polished C,
resampled delivery projects, PNG/JPEG exports and Qt workspace captures.

| Final corrected photograph | Elapsed | Sampled RSS peak | Admitted peak | Spill peak / entries | JPEG mean sampled channel error |
| --- | --- | --- | --- | --- | --- |
| Portrait | 99949 ms | 1004785664 bytes | 769044853 bytes | 173178880 bytes / 341 | 0.9409993489583334 |
| Product | 104066 ms | 1517277184 bytes | 810701429 bytes | 181276672 bytes / 358 | 0.9536946614583334 |

Both final rows released all spill bytes and entries with zero cleanup failures
and zero active IO. These are sampled RSS/resource observations on the actual
laptop, not broader LP8 qualification.

Independent final saved-project review passed both photograph rows, setup and
teardown: **4 Qt cases in 301410 ms**, with zero remaining spill charges and
cleanup failures. See `build/release/d2-native-saved-review-final.txt`.
Full-size A/B/C renders and workspace/control captures are retained in the final
artifact directory. Independent final 100%-size graded-image visual QA passed
the frozen target scope: corrected A/C cleanup and B's deliberately reduced
strength, color/detail, retained boundaries and grade transitions. This does
not claim removal of the product's separate right-side scratch network.

The settled control capture also passed: `build/d2-control-capture/run.log`
records curve graph bottom 336 and numeric-field top 344, with saved input
127.500 and output 128.775, unchanged document identity and clean retirement.
The final artifact is `portrait-saved-grade-controls-settled.png` in the final
photograph directory. It verifies settled widget layout and saved controls;
it remains a Qt capture, not a physical-desktop screenshot.

Together the final primary run, independent saved-project run, scoped visual
QA and settled controls close the defined D2 acceptance. Failed earlier runs
remain recorded above. Broader release/LP8, GPU, recovery, professional color,
RAW/high-depth and print qualification remain outside this development gate.
