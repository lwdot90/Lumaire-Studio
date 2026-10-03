# Streamed PNG and JPEG image export

> Export note: Recorded validation results are historical source-repository observations, not qualification of this standalone repository. Commands use the standalone layout.

`io::exportImage(path, document, options)` runs on an IO worker with an immutable
snapshot. The extension selects PNG (`.png`) or JPEG (`.jpg` / `.jpeg`), case
insensitively. It writes the full document resolution, flattening visible
layers through the same CPU layer sampler and admitted mip cache used by the
renderer. Export does not create a full-canvas QImage or encoded-file buffer.
Only one scanline and bounded codec/filter state are resident. Maximum side is
32,768 pixels; this is a dimension limit, not a measured throughput guarantee.

PNG stores straight encoded sRGB RGBA8, including alpha and canonical black
RGB for fully transparent samples. JPEG composites premultiplied linear RGB
over linear white, encodes sRGB RGB8, and uses baseline 4:4:4 compression with
quality 1–100. Extended RGB is clipped only at the final SDR code boundary.
No dither is applied. Document DPI is retained unless a positive resolution
option replaces it. PNG stores rounded pixels/meter; JPEG stores rounded
integer DPI. Resolution does not resize the image. The upstream
`Compositor/IO/ImageExporter.swift` similarly exports sRGB, preserves PNG
resolution, and flattens JPEG against a background. Linux rendering uses its
own explicit blend/color contract; no CoreGraphics or Photoshop output parity
is asserted.

The row and sampling plans are admitted before allocation. PNG installs custom
allocation callbacks, including its zlib state. JPEG installs custom small and
large allocation callbacks immediately after initialization; baseline mode
rejects virtual-array requests, disabling full-image/progressive/optimized
coefficient storage. Callback allocations have independently retained charges
for the requested backing, allocation header and a 64-byte allocator allowance.
At most 4,096 callback allocations can be live. A separately admitted 128 KiB
allowance covers initial JPEG memory-manager/control objects and codec stack
state. It is conservative policy, not an instrumented bound on libc/libjpeg
initialization internals or total process RSS. The injected application memory
admission and separately charged mip/source backing remain authoritative.

QSaveFile with direct-write fallback disabled keeps the previous destination
until encoding and final identity checks complete. An absent `expected`
identity requires a missing destination; replacing an existing file requires
its observed regular-file identity. Symlinks/nonregular destinations are
rejected by the common file-identity reader. Identity is checked before writing
and again before commit. A concurrent namespace replacement in the final
check-to-rename window is not prevented by a filesystem compare-and-swap.
Cancellation and injected failures before commit discard the temporary file;
no post-commit cancellation rollback is claimed. This export does not mark the
editable project saved, change its file path, or alter document history.

Cancellation is observed before each scanline, every 256 sampled pixels, in
source reads and around final codec completion/commit. A codec write or kernel
IO operation already in progress can delay cancellation. Export has no fixed
250 ms cancellation guarantee. Export checkpoint hooks support tests and are
empty in production.

The standalone test independently decodes tiny output through Qt's PNG/JPEG
readers and checks dimensions, alpha, white flattening, sRGB colors, DPI and an
analytical two-layer Multiply result. It also checks cancellation and failure
rollback for both codecs, destination changes during encoding and stale identity
conflicts, a codec allocation failure after first-row sampling exercising the C
error/longjmp path with zero retained charges, unsupported formats, quality bounds,
admission denial and released scratch charges. Fixtures are at most 2×2.
The implementation agent performs no builds; the parent must register libpng,
libjpeg and this test, then execute normal and sanitizer verification.
