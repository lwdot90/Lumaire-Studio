# M3 GPU blend expansion

> Export note: Recorded validation results are historical source-repository observations, not qualification of this standalone repository. Commands use the standalone layout.

The Vulkan blend kernel supports the 24 modes declared by `core/blend.h`.
Existing IDs 0–12 and their equations remain unchanged. Append-only IDs 13–23
are Linear Burn, Linear Dodge, Soft Light, Hard Light, Vivid Light, Linear
Light, Pin Light, Hard Mix, Exclusion, Subtract and Divide. Host compile-time
assertions bind the appended enum values to shader dispatch IDs. Saved
projects continue to use named identifiers, rather than numeric shader IDs.

Inputs and output are FP32 premultiplied linear pixels. Only the artistic
alpha-overlap contribution is converted to bounded encoded sRGB and blended;
extended non-overlap RGB remains intact. Transparent source/backdrop bypasses
are unchanged. Burn and Dodge retain their existing endpoint precedence.
Vivid Light delegates to those guarded equations. Hard Mix uses a deterministic 12-bit nearest encoded overlap code. Frozen
float32 decode thresholds compare against premultiplied input without division
or platform-dependent `pow`; ties round upward. The integer backdrop and source
codes sum to at least 4096 for white. Raw backdrop-white/backdrop-black guards
precede source-black/source-white guards, preserving Burn/Dodge endpoint
precedence. The lookup reads a shared readonly storage buffer uploaded once per kernel
from the generated CPU header. Its actual Vulkan allocation is admitted and
retained until kernel retirement; no per-invocation writable 16 KiB array is
created. This new-mode boundary differs from continuous Vivid Light at
quantization boundaries; each encoded component changes by at most 1/8192.
The shared generation script records exact threshold bits and their hash.
Divide returns white when source code is zero, including 0/0. These equations implement the explicit Linux
contract; they do not establish measured Photoshop fixture parity.

`gpu_blend_test` enumerates `blendModeCount` for direct dispatch, layered
bilinear sampling, and high-quality mip sampling. Direct dispatch checks
counts 1, 63, 64, 65 and 65,536, padding guards, invalid mode rejection,
canonical transparent output, extended RGB, soft alpha and two dependent GPU
passes without intermediate readback. Explicit encoded endpoint and half-way
neighborhoods supplement deterministic random canonical-half inputs. Eight frozen
nearest-code thresholds additionally require analytical classification at the
immediate float32 neighbor below, equality and the neighbor above; CPU and
GPU pixels must exactly match those independently derived outputs. The
layered fixture includes nested visible pass-through folders with 0.5 and
0.75 opacity, alongside a hidden folder. Folder opacity enters the kernel
through the layer plan's effective sampling opacity.

CPU/GPU FP32 comparisons require absolute error at most 0.002 or relative
error at most 0.002 for extended magnitudes. Display comparisons require at
most one SDR code step. Hard Mix's discontinuity is not exempted from those
checks. The harness reports observed maxima and validates all Vulkan and
synchronization messages when `--require-validation` is supplied.

Compilation and device execution belong to the parent integration pass; no
build or device run was performed by the GPU blend implementation agent.
The parent should run the complete harness on Intel UHD 620 and llvmpipe,
with explicit Vulkan and synchronization validation, then record the actual
results in the M3 qualification evidence.
