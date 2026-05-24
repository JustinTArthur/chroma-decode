# Architecture

The implementation plan is the authoritative source. While the project is in
its early phases, that lives at
`an internal planning document` (a Claude Code
plan file). As the work lands, the relevant sections are summarised here.

## High-level shape (planned)

- **Public surface**: pure C ABI under `<chromadec/...>` with opaque handles,
  `chd_status_t` error codes, and a thread-local last-error detail string.
  Symbol prefix `chd_`.
- **Internal implementation**: C++17 under `src/`. The C ABI lives only in
  `src/abi/` shims that translate handles and catch C++ exceptions. No Qt
  anywhere.
- **Build system**: Meson primary; ships pkg-config `.pc` and a CMake package
  config so cmake consumers can `find_package(chromadec CONFIG REQUIRED)`.
- **Decoders**: mono, NTSC comb (1D/2D/3D + adaptive), PALcolour, Transform-PAL
  2D/3D, nnTransform3D (CPU FFTW + optional CUDA), ldzeug2 color_cnn,
  ldzeug2 luma_sep (per-field + per-frame).
- **Readers**: legacy `.tbc` (with `.tbc.db` / `.tbc.json` sidecars), CVBS
  `.composite`, CVBS dual-file YC (`.y` + `.c`), all via a single `ISource`
  interface.
- **NN**: ONNX Runtime; one process-wide `Ort::Env` (RAII singleton); one
  `Ort::Session` per `chd_nn_model_t`, shared across all worker threads
  (sessions are thread-safe per upstream).

## Current state

Bootstrap is in progress. The repo holds:

- Preserved git history from `ld-decode` extracted via `git filter-repo`
  (paths `tools/ld-chroma-decoder/` + `tools/library/`, ~478 commits) staged
  under `src/legacy/` for incremental porting.
- A stub-only public C API (every function returns `CHD_E_INTERNAL`).
- The Meson build, pkg-config, and CMake package-config plumbing.
