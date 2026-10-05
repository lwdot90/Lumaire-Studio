# D2 completion checklist

**Status: D2 scoped development acceptance completed October 4, 2026.**
Both fixed photographs passed the corrected native workflow, independent saved-project
review and visual assessment. See the [acceptance record](d2-photo-acceptance.md).

D2 is revisable photographic editing. The finite exit is the portrait **and**
product revision brief in [revisable-photo-brief](revisable-photo-brief.md), not a
count of adjustment commands. D1 remains closed; this does not reopen renderer
milestones or silently add RAW, print, PSD or broader LP8 qualification to D2.

## Independent status audit

| Gate | Status | Verified evidence |
| --- | --- | --- |
| Retained grading | Passed | Both photographs retain ordered Exposure, Levels, master/Red Curves and Color Balance; reopening and revisions preserve sources, unrelated channels and settings. Preview cancel leaves history unchanged. |
| Separate-layer retouch | Passed | Named Retouch layers retain Clone/Heal records; B changes the first stroke to 40% and layer opacity to 65%, continues retouching, and preserves other records. C retains B and appends cleanup. The Original photograph layer stays exact. |
| Soft boundaries | Passed | Both photographs reopen, move the grade-mask boundary, change softness, disable/re-enable and Undo/Redo. Supplemental boundary/feather edits restore exact snapshots and coverage hashes. |
| Defined delivery resampling | Passed | C becomes 2000×1500 with Lanczos; Undo preserves original-size C. Native delivery reopen preserves canonical caches, masks, geometry and editable records. A/B/C remain separate original-size projects. |
| Actual finished photograph | Passed | Both fixed 12 MP photographs have editable A/B/C/delivery projects, full-size A/B/C PNGs, delivery PNG/JPEG and workspace/saved-control captures. No required construct needs flattening. |
| Real-host safety | Passed for this workflow | Forced CPU, actual host admission and 512 MiB headroom ran with desktop apps retained. Primary and supplemental checks report zero post-release spill bytes/entries/failures; measured peaks and prior refusals are in the acceptance record. |
| Quality | Passed for frozen targets | Independent delivery-size and 100% review accepts A/C target removal, texture/color/detail and mask boundaries; B deliberately restores partial distractions. Source softness/clipped lights and the product's unrelated right-side scratch network remain explicit limits. |

The original photo must remain exact in the base layer and pre-resample native
project. Explicit pixel resampling is lossy geometry; its retained source is
resampled too. The original-size saved project and Undo preserve original detail.
This is an explicit delivery contract, not a claim that a reduced-size raster
contains all original samples.

## Fixed actual photographs

Acquired and visually inspected October 3, 2026. Both are unmodified original
4000×3000 RGB JPEGs, not generated or resized stand-ins. Sources and license
statements were checked on their authoritative Commons file pages.

| Fixture | Author and source | Rights | Downloaded bytes / SHA-256 |
| --- | --- | --- | --- |
| `portrait.jpg` | Doormaind, [Woman seated in office chair in saree](https://commons.wikimedia.org/wiki/File:Woman_seated_in_office_chair_in_saree.jpg) | [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/) | 2,271,268; `84496329b251241f0ed8238c27d1b077ab9b3cf34b85f12893c208b70f1ad0da` |
| `product.jpg` | Candeadly, [Orbitz bottle](https://commons.wikimedia.org/wiki/File:Orbitz_bottle.jpg) | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) | 4,734,297; `6c9cfc43bfaf2086bfbb085c4a03fc71eba38caabe6fe403a7bc191bd5d0d029` |

Private originals, `provenance.json` and `ATTRIBUTION.md` are under ignored
`build/d2-photo-fixtures/`; total original payload is 7,005,565 bytes (about
6.68 MiB). They are not application source assets. Direct originals:

- [Portrait JPEG](https://upload.wikimedia.org/wikipedia/commons/b/ba/Woman_seated_in_office_chair_in_saree.jpg?download=1).
- [Product JPEG](https://upload.wikimedia.org/wikipedia/commons/9/9d/Orbitz_bottle.jpg).

Credits accompany derivative artifacts. If shared, edited portrait imagery
retains CC BY-SA 4.0 and edited product imagery retains CC BY 4.0 attribution;
include source/license links and change notices. These photograph licenses do
not change the application's MIT source license or imply subject/product
endorsement.

## Frozen edit and revision request

Coordinates below are original document pixels. The portrait contains skin,
hair, shadow and embroidered texture; the product supplies colored labeling,
transparent glass, reflections and a textured neutral wall with real small marks.

1. Keep the imported photograph as an unchanged base. Duplicate it for a named
   retouch layer. Apply the frozen recipe below, with Clone and Heal stored as
   separate retained strokes. Do not erase identifying features, glass highlights,
   label characters or texture merely to make numeric comparisons easier.
2. Use a separate grade layer above retouch. Retain Exposure, Levels, Curves
   and Color Balance with a soft linked mask. Initial grade coverage centers
   on the portrait's head/upper torso, ellipse `(1450,160,1100,1400)`, or the
   product, ellipse `(1330,180,1170,2640)`; initial feather radius is 32 pixels.
3. Save the original-size editable project A. Close its tab and reopen from disk
   before revisions; retaining an already open document does not satisfy reopen.
4. Revise Exposure/Levels, one master or individual curve and warmth/tint from
   retained sources. Revise the first retouch stroke to 40% and its layer opacity
   to 65%; verify that base pixels and unrelated retained records are unchanged.
5. Move the grade-mask boundary and change softness to 48 pixels. Verify
   enable/disable and Undo/Redo. Continue retouching with another bounded Heal
   stroke. Save the requested revision B at original size. Its lower retouch
   strength deliberately exposes part of the original distraction.
6. Continue from B to a polished version C: restore retouch-layer opacity to
   100% and add full-strength Clone cleanup strokes on the same two real targets.
   Preserve B separately with its requested 40%/65% settings; retain all stroke
   records in C. Save C at original size before any delivery resampling.
7. Resample C to 2000×1500 with Lanczos, then save/reopen the delivery project.
   A, B and C stay available. Export PNG/JPEG; verify native records and decoded
   dimensions/pixels, and assess A/C removal and B's intended reduced-strength
   revision visually. Neither flattening nor loss of the base is allowed.

### Corrected retouch recipe, frozen October 4 before the next scored run

The v3 numerical run passed but its visual retouch failed: a chair-texture donor
made a dark patch on the portrait bottle, the Heal stroke smeared keys, and the
product brush missed most of the scratch. This corrected recipe keeps those
same real distractions. It uses matching clean wall/table donors, follows the
actual scratch, and bounds portrait strokes away from the tabletop/panel edge.
Broad objects are removed with Clone; Heal remains a separate editable texture
operation and is not claimed to erase broad low-frequency objects on its own.

| Photograph / target | Clone path in original pixels | Source anchor | Diameter |
| --- | --- | --- | --- |
| Portrait bottle | `(3180,1110)` → `(3180,1253)` → `(3200,1253)` → `(3200,1110)` → `(3220,1110)` → `(3220,1253)` | `(2880,1110)` on clean partition | 130 |
| Portrait keys/wires | `(600,1435)` → `(800,1430)` → `(800,1450)` → `(600,1455)` → `(600,1475)` → `(800,1470)` | `(440,1465)` on clean table | 100 |
| Product main scratch | `(3700,675)` → `(3650,720)` → `(3590,800)` → `(3500,900)` → `(3400,1000)` → `(3350,1100)` → `(3300,1190)` → `(3250,1240)` → `(3200,1280)` → `(3130,1310)` → `(3070,1335)` | `(3100,675)` on clean wall | 150 |
| Product yellow wall mark | `(2960,2150)` | `(2760,2150)` on adjacent clean wall | 160 |

Initial A stores main Clone, secondary Heal, then secondary Clone cleanup, all
at strength 100% and hardness 25%. Heal radius is 32 captured document pixels (also source pixels for these identity-placed original photographs). The portrait
main Clone records a feather-8 rectangular selection `(3125,1055,125,212)` to
protect the tabletop and neighboring chair; clear the current selection before
other strokes. B changes only the first stroke's strength, the retouch-layer
opacity, and appends the 30%-strength Heal continuation (radius16), besides the
declared grade/mask revision. C appends two full-strength Clone cleanup strokes
and restores layer opacity; it keeps B's earlier records intact. A/B/C have
3/4/6 retained strokes on the separate retouch layer. Name the layers
`Original photograph`, `Retouch`, and `Grade` using the existing Layers controls.

The product's separate right-side scratch network is outside the selected main
scratch target and is not claimed removed. The final matching donor avoids the
bottle silhouette over the complete path. Harder/shorter recipes were rejected
for visible capsules, missed ends or contaminated donors; see the acceptance
record for preserved failures. Before the scored run, full-size probes accepted
portrait A/C bottle/keys and product A/C main-scratch/wall-mark removal. Scored
reopen/export and final graded-image review subsequently passed independently.

Freeze any revised donor/target coordinates before scoring a new run, and
record why they changed. A failed retouch on a difficult texture is a failure,
not permission to substitute a blank synthetic patch.

## Qualification record

The final source snapshot is
`12269649394c77962d6d7fd6bf0d09378a42b06d64000ec14a21a90fa4e9ec5c`,
based on uncommitted work after `33f273d4b4ca4f5b0635edc1a84a6bdd42a137ef`.
The application SHA-256 is
`ef6023b57a603df10724cd2d4d5a862c075a92d878eefcd59d858f0b497a1760`.
All 61 default tests passed in 31.90 seconds. Final native primary checks passed
4 cases in 204191 ms, and independent saved-project review passed 4 cases in
301410 ms. Evidence logs are `build/release/d2-final-ctest.txt`,
`build/release/d2-native-photos-final.txt` and
`build/release/d2-native-saved-review-final.txt`.

Artifacts and attribution are under `build/d2-photo-native-20261004-final/`.
Settled saved-control layout passed separately in `build/d2-control-capture/run.log`;
these are Qt widget/renderer captures, not physical-desktop screenshots. PNG/JPEG
verification samples 4096 output pixels; it is accompanied by visual review,
not represented as an exhaustive pixel comparison. Measured primary RSS peaks
were 1004785664 bytes for portrait and 1517277184 bytes for product. Both rows
released all spill backing with zero cleanup failures and active IO.

The [acceptance record](d2-photo-acceptance.md) preserves the failed first cleanup
assertion, product memory refusal, rejected retouch recipes and subsequent fixes.
Those failures are not relabeled successful runs. Every frozen D2 gate above now
passes for both photographs. Durable recovery, professional color/print,
RAW/high-depth, interchange and complete LP8 remain separate requirements;
this closes the defined development workflow, not Photoshop parity or broad
release qualification.

## Reproduce the actual-photo run

With the registered `d2_photo_workflow_test` built, supply the
fixed private originals and a fresh output directory:

```sh
LUMAIRE_D2_PHOTO_FIXTURES="$PWD/build/d2-photo-fixtures" \
LUMAIRE_D2_PHOTO_CAPTURE_DIR="$PWD/build/new-d2-photo-delivery" \
QT_QPA_PLATFORM=wayland \
./build/release/d2_photo_workflow_test "$PWD/build/release"
```

Without the fixture environment variable, the same binary uses tiny synthetic
smoke inputs and explicitly labels them as such. A passing smoke run cannot
close photograph acceptance. The actual run checks source hashes and dimensions,
uses actual host admission and disk spill, processes photographs sequentially,
and retains attribution beside original/revised/delivery projects and exports.
Sampling of process/admitted/spill peaks is reported separately from visual
review; the mask/retouch/grade revision and output oracles must pass as well.
