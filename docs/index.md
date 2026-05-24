# libchromadec documentation

A shared library for decoding 4×fsc-sampled composite analog video (CVBS / TBC)
into component Y'CbCr output.

## Contents

- [Initial design thoughts](design-notes.md) — the planning document that
  kicked the project off
- [Architecture](architecture.md) — current design and module layout
- [ABI stability](abi-stability.md) — versioning and compatibility rules
- [Porting from ld-chroma-decoder](porting-from-ld-chroma.md) — for downstream
  consumers replacing their submodule / vendored copy
- [NN model conventions](nn-models.md) — model paths, magnitude scales,
  execution provider notes
- [File formats](file-formats.md) — TBC vs CVBS reader matrix
- [Attribution](attribution.md) — original contributors whose work is
  preserved here
