# Changelog

All notable changes to this project will be documented in this file. Format:
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versioning:
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Initial skeleton: meson build, public C header stubs, repo layout.
- Preserved git history extracted from `ld-decode` via `git filter-repo`
  (paths `tools/ld-chroma-decoder/` + `tools/library/`, ~478 commits)
  staged under `src/legacy/` for incremental porting.
- TBC metadata reader and source video reader, built on the C++17
  standard library and `sqlite3` instead of Qt:
  `chd::metadata::LdDecodeMetaData`, `DropOuts`,
  `SqliteReader`/`SqliteWriter`, `chd::reader::TbcSource` (renamed
  from `SourceVideo`). The C ABI exposes
  `chd_video_open_composite`, `chd_video_get_info`,
  `chd_video_add_extra_source_composite`, `chd_video_free`.
- CVBS file format support (CVBS file format specification, SQLite
  `user_version = 7`): `chd::reader::ISource` abstract field-reader
  interface with three concrete implementations: `TbcSource` (`.tbc`), `chd::reader::CvbsCompositeSource` (`.composite`),
  `chd::reader::CvbsYcSource` (dual-file `.y` + `.c`). Static preset
  tables for the three Video Standards (PAL, NTSC, PAL_M), five
  Sample Encodings (`CVBS_U10_4FSC`, `CVBS_U16_4FSC`,
  `CVBS_TPG21_4FSC`, `RAW_S16_28M`, `RAW_S16_40M`), and six Signal
  States in `src/format/`. CVBS `.meta` sidecar reader at
  `chd::metadata::readCvbsMetadata`. The C ABI exposes
  `chd_video_open_composite`, `chd_video_open_yc`,
  `chd_video_add_extra_source_composite`,
  `chd_video_add_extra_source_yc`. Parameter resolution chain:
  explicit `meta_path` → auto-located `<basename>.meta` →
  caller-supplied `chd_video_params_t` → `CHD_E_METADATA_MISSING`.
- Per-source mutex on `TbcSource::getVideoField` and the new CVBS
  source classes so concurrent field reads from worker threads are
  race-free.
- NN framework: ONNX Runtime integration scaffolding.
  Process-wide `chd::nn::OrtEnvSingleton` (one `Ort::Env` per
  process, lazy `std::call_once` init, opt-in `shutdown()` only
  reachable via `chd_shutdown()`).
  `chd::nn::OrtSession` RAII wrapper, shared across worker threads.
  Provider selection (`chd::nn::provider_select`) with per-OS auto
  chains — Windows: TensorRT → CUDA → DirectML → CPU; Linux: CUDA →
  MIGraphX → CPU; macOS: CoreML → CPU — that walks the chain and
  reports which provider actually attached. The C ABI exposes
  `chd_nn_model_load`, `chd_nn_model_free`,
  `chd_nn_model_get_active_provider`, `chd_nn_provider_is_available`,
  `chd_nn_session_opts_default`. `chd_has_feature("nn"/"cuda"/
  "fftw"/"sqlite")` is wired through to the build-time `with_*`
  options. nnTransform3D and ldzeug2 decoder ports are deferred to
  a follow-up.

### Dependencies
- ONNX Runtime (required when `with_nn=true`, the default). The
  meson build fails loudly if `with_nn=true` and ORT is not
  discoverable via pkg-config or `-Donnxruntime_root`; pass
  `-Dwith_nn=false` to disable.
- Decoder framework: `chd::decoders::Decoder` synchronous interface,
  `SourceField` data container, `chd::output::ComponentFrame` and
  `chd::output::OutputWriter`. Filter library
  (`firfilter` / `iirfilter` / `deemp`) under
  `chd::decoders::filter`.
- Decoders: `chd::decoders::mono::MonoDecoder` (luma-only),
  `chd::decoders::palcolour::PalDecoder` (PALcolour algorithm with
  optional 2D/3D Transform-PAL frequency-domain filter, FFTW3),
  `chd::decoders::comb::NtscDecoder` (NTSC adaptive 1D/2D/3D comb).
  `chd::output::FrameCanvas` debug overlay helper.
- `chd::pipeline::DecoderPool`: thread-pool orchestrator built on
  `std::thread`, `std::atomic`, `std::mutex`, `std::ofstream`, and
  `std::chrono::steady_clock`.
- Unit tests: TBC reader round-trip and end-to-end pipeline sanity
  test.

### Dependencies (legacy)
- SQLite3 (required from this release).
- FFTW3 (required by the Transform-PAL decoders).

[Unreleased]: about:blank
