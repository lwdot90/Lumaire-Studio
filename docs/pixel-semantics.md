# Pixel semantics — version 1

> Export note: Recorded validation results are historical source-repository observations, not qualification of this standalone repository. Commands use the standalone layout.

Normative M0 contract, 2026-09-20. Changes require a decision record and updated
fixture/workload versions. macOS is an interaction reference, not a pixel oracle.

## Representation

Working space: sRGB/Rec.709 primaries, D65 white, linear light. Color tiles:
interleaved little-endian binary16 premultiplied RGBA. Coverage tiles: binary16,
no transfer function or ICC conversion. Tile grid: 256×256 in asset coordinates.
Pixels occupy [x,x+1)×[y,y+1), centers (x+0.5,y+0.5), x right/y down. Signed tile
coordinates use floor division. Edge tiles store their intersection with the
asset extent, row-major without padding. Missing tiles use explicit defaults.

Alpha/coverage are finite [0,1]. RGB may be negative or exceed alpha/one to retain
intermediate headroom and out-of-gamut colors. No global RGB<=alpha clamp.
Alpha zero implies RGB positive zero; canonicalize all negative zero to positive
zero. Reject persisted NaN/infinity and computed nonfinite or binary16 overflow
(outside [-65504,65504]); fail the transaction without changing its input.
Extended storage does not promise HDR display or lossless 16-bit integer TIFF.

## Arithmetic

FP32 arithmetic, FP64 geometry/matrices. Scalar oracle: IEEE ties-to-even,
gradual underflow, no fast-math/reassociation. Half conversion is ties-to-even
with representable subnormals preserved. SIMD/GPU output meets tolerances below.
Unpremultiply iff alpha>0; no generic epsilon that discards tiny alpha.
Quantize only immutable output tiles once per operation, never per preview event.
Stroke coverage accumulates FP32 against frozen input/settings/selection;
predicted tails replace transient contributions. No repeated half rounding per
dab. Save/reopen preserves canonical bytes; undo restores prior tile versions.

## Color conversion and output

For nonnegative encoded sRGB c, decode(c)=c/12.92 for c<=0.04045, otherwise
((c+0.055)/1.055)^2.4. For nonnegative linear c, encode(c)=12.92*c for
c<=0.0031308, otherwise 1.055*c^(1/2.4)-0.055. Extend to negatives by odd
symmetry. Apply transfer functions to straight RGB, never alpha/premultiplied RGB.
Import orients once, converts embedded ICC with Little CMS floating-point
relative-colorimetric intent and black-point compensation to linear working RGB,
then premultiplies. Untagged RGB assumes sRGB and reports it; unsupported CMYK
is rejected. ICC LUT transforms may clip: extended storage alone cannot promise
lossless arbitrary-profile conversion. Preserve source profile metadata.

Display/export share the committed composite. Output ICC conversion does not
mutate tiles. SDR output clips to the output range after conversion; no implicit
tone map. Default output sRGB. JPEG matte composites in linear light before
conversion. PNG stores straight alpha. 8-bit RGB quantizes encoded values with
ties-to-even; alpha is undithered. Default RGB dither is ordered 8×8 Bayer:
B1=[0]; B2n=[4Bn+0,4Bn+2;4Bn+3,4Bn+1]. Add (B8+0.5)/64-0.5 to 255*c
before rounding/clipping; index absolute document x/y, same offset per channel.
Exact code-value tests disable dither. Native tiles are never dithered.

## Compositing

s,b are straight linear source/backdrop RGB, as,ab effective alpha; ps=as*s,
pb=ab*b. Normal: po=ps+pb*(1-as), ao=as+ab*(1-as). Opacity/masks multiply
RGB and alpha once. Invisible layers do not composite. Clipping source coverage
includes alpha, opacity, enabled raster mask and upstream clipping; not RGB or
visibility. Reject cycles before rendering. Folders pass through with opacity 1
and Normal; folder masks multiply descendant coverage in document placement.

For artistic modes use S=clamp(encode(s),0,1), D=clamp(encode(b),0,1).
po=as*(1-ab)*s + as*ab*decode(B(D,S)) + (1-as)*ab*b; ao unchanged.
Only artistic overlap clips extended colors.

| Mode | B(D,S), componentwise except last four |
|---|---|
| Multiply | D*S |
| Screen | D+S-D*S |
| Overlay | 2*D*S if D<=0.5, else 1-2*(1-D)*(1-S) |
| Darken | min(D,S) |
| Lighten | max(D,S) |
| Difference | abs(D-S) |
| Color Dodge | 0 if D=0; else 1 if S=1; else min(1,D/(1-S)) |
| Color Burn | 1 if D=1; else 0 if S=0; else 1-min(1,(1-D)/S) |
| Hue | SetLum(SetSat(S,Sat(D)),Lum(D)) |
| Saturation | SetLum(SetSat(D,Sat(S)),Lum(D)) |
| Color | SetLum(S,Lum(D)) |
| Luminosity | SetLum(D,Lum(S)) |

Normative nonseparable helper definitions: sections 10.2–10.2.4 of
[W3C 2024-03-21](https://www.w3.org/TR/2024/CRD-compositing-1-20240321/).
SetSat equal-channel ordering is stable R,G,B; zero range produces zero before
SetLum. Clamp final B to [0,1]. Color-space application is this product's choice.

## Sampling and masks

Inverse-map destination centers. Nearest selects floor(source coordinate),
ties at boundaries select right/bottom. Bilinear uses pixel centers and triangle
weights. Filter premultiplied linear RGB and alpha together. Raster exterior is
transparent; masks have persisted exterior coverage (default 1). Missing tiles
within extents use asset defaults. Selections have zero exterior at all times.

High-quality reduction: separable Lanczos-3, sinc(t)*sinc(t/3) for |t|<3,
zero otherwise, sinc(0)=1. For reduction q<1 support=3/q, weights=q*K(q*t),
normalize the full tap set including exterior. Accumulate x left-to-right then
y top-to-bottom. Clamp filtered alpha; if old alpha>0 rescale RGB by new/old
alpha, otherwise transparent zero. Preserve extended straight RGB.
Mip k uses stable 2^k grid, exact successive 2× Lanczos halvings, dimensions
rounded up, transparent raster padding. Nearest bypasses mips. Isotropic q uses
max(0,floor(log2(1/q))), then residual filtering. For affine/projective transforms
use direct source-grid footprint evaluation when the footprint is anisotropic:
separable source-axis support from the inverse Jacobian's row lengths, each
clamped to at least 1; do not substitute a coarser isotropic mip. This v1 filter
is explicitly separable, not an EWA filter. Halos include full support.

Selection shapes rasterize on an 8×8 uniform subpixel grid, averaging inside
samples; top/left edges inclusive, bottom/right exclusive. Coverage algebra:
replace=b, add=max(a,b), subtract=max(0,a-b), invert=1-a inside finite canvas.
Expand/contract are max/min over integer pixel centers in a Euclidean disk;
exterior zero. Selection-limited edits lerp old/proposed premultiplied pixels
once. Mask invert=1-m. Erase multiplies RGB/alpha by (1-coverage*opacity).

Gaussian radius r specifies support ceil(r), sigma=r/3, normalized sampled
Gaussian; identity at r=0. Two separable FP32 passes with FP32 intermediate,
half output. P05 radius 20 means sigma=20/3, 41 taps per axis. Expand raster
bounds by support; selections limit application, not neighboring input reads.

## Operation spaces and tests

Exposure/spatial filters use linear light. Levels/Curves/HSV operate on straight
encoded RGB, clipping bounded controls' inputs to [0,1]. Gradient Map uses
linear luminance 0.2126R+0.7152G+0.0722B and bounded stops. Default gradients
interpolate premultiplied linear endpoints. Seeded effects key randomness by
stored algorithm version, uint32 seed, absolute coordinates/channel/sample;
never by thread/tile order. M4/M7/M8 define their specific deposition/adjustment/
solver equations and fixtures before implementation, as the plan requires.

Bounded pointwise error <=0.002 per premultiplied channel; extended RGB error
<=max(0.002,0.002*abs(reference)). Alpha uses absolute tolerance. Undithered
8-bit pointwise error <=1 code step. Geometric/Gaussian/Lanczos bounded error
<=0.003; tile-versus-whole error <=0.002, with no added boundary discontinuity.
Exact: native bytes, IDs, undo restoration, zero-alpha canonicalization, fixture
hashes. These limits are specifications, not claims of passing engine tests.
