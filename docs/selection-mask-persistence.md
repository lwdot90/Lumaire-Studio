# Selection and linked-mask persistence

> Export note: Recorded validation results are historical source-repository observations, not qualification of this standalone repository. Commands use the standalone layout.

Schema 3 stores a bounded rectangle/ellipse selection and linked layer-local
mask coverage without changing canonical color interpretation. The schema and
strict selection envelope are specified in
[project-format-v3.md](project-format-v3.md).

The upstream `DocumentSelection` supports paths, antialiasing, feathering, and
explicit empty coverage. This Linux packet enables only the immutable bounded
rectangle/ellipse/inversion model. Upstream `LayerMask` supports enable state,
linked movement, and independent placement; this packet preserves enable state
and linked same-grid masks, with all unsupported placement modes rejected.
No macOS format fields or Swift implementation are modified.

Writer validation precedes publication. Color and mask asset identities cannot
conflict. Coverage has zero RGB and canonical alpha [0,1], including defaults.
Mask extent and transform agree with their layer. The writer streams the same
one-tile canonical/encoding buffers and owning leases for color and mask assets;
existing spilled payloads return to disk-only cache residency after copying.

The reader validates authored version-specific DDL, selection parameters,
asset kinds, separate color/mask extent limits, mask/layer references and
placement, then verifies and validates every decoded mask pixel. Unsupported
or corrupt state aborts the complete staged document. Existing documents and
previous destination files remain intact on failed load or pre-rename save.
Disabling a mask preserves its coverage asset. Mask bytes and selection state
remain available to undo/redo through immutable document snapshots.

Implementation and regression fixtures are owned by the persistence agent;
parent integration owns build wiring and serial verification. This packet does
not establish full selection-tool, brush, mask-transform, or low-resource
qualification.
