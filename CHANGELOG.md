# Changelog

All notable changes to this project will be documented in this file. Format:
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versioning:
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Golden-frame comparison against the legacy ld-chroma-decoder
  binary pinned at ld-decode commit f39e59e18 (the last good before
  the `tools/` deletion in a4e403be). When the `CHD_LD_CHROMA_DECODER`
  env var points at a built ld-chroma-decoder binary, the test runs it
  against the same encode-orc fixture used in
  `testEncodeOrcColourBars` and asserts the chd C ABI's YUV444P16
  output matches pixel-for-pixel against the reference. Confirmed
  bit-exact (max|diff|=0 across all 368,600 pixels in all three
  planes) for NTSC 2D comb against the f39e59e18 reference. Skips
  cleanly with PASS when either env var is unset.

### Added
- Integration test driving encode-orc → chd_decode_frame end
  to end (`tests/unit/test_integration.cpp`). When the
  `CHD_ENCODE_ORC` env var points at a built encode-orc binary, the
  test writes a minimal NTSC 75 % colour-bars project YAML, invokes
  encode-orc to synthesize a one-frame TBC + sqlite sidecar, opens
  them via `chd_video_open_composite`, commits CHD_DEC_NTSC_2D, decodes
  frame 0, and asserts real luma + chroma energy (Y range > 30000,
  Cb/Cr swing > 10000 straddling C_ZERO). Skips cleanly with PASS
  when the env var is unset so `meson test` stays green on machines
  that don't have the encoder built. Assets path can be overridden
  via `CHD_ENCODE_ORC_ASSETS`; defaults to the sibling `assets/`
  directory of the encode-orc binary's grandparent (matches the
  upstream layout).

### Added
- True per-worker async parallelism: `chd_decoder_commit`
  now builds `thread_count` decoder instances (one per worker), each with
  its own `std::mutex` and configured against the same post-padding
  `VideoParameters`. NN sessions are shared via `shared_ptr` (Ort::Session
  is thread-safe per ORT docs). `chd_decode_frames_async` spawns
  `min(n, threadCount)` `std::thread` workers that race-pull next-index
  from a `std::atomic<size_t>` — unequal frame costs balance
  automatically; sync `chd_decode_frame` uses worker 0 and can run
  concurrently with async on workers 1..N-1. Replaces the previous
  std::async-per-index + single-mutex pattern that provided API surface
  but no real parallelism. `lastDropoutStats` updates now run under a
  dedicated small mutex (`statsMutex`) since multiple workers can
  publish concurrently.

### Added
- Multi-source dropout correction at the C ABI:
  `chd_decode_frame` now builds a `std::vector<chd::dropout::ExtraSourceFrame>`
  from each `chd_video_extra` on the primary and runs the multi-source
  `DropoutCorrector::correctFrame` overload when extras are attached.
  Each extra carries its own `LdDecodeMetaData` (loaded from a
  `.tbc.db`/`.json` sidecar for TBC extras, synthesized from
  `ISource::parameters()` for CVBS extras), so the corrector can
  pull per-source `Field.dropOuts` + VITS bPSNR-derived quality
  exactly the way the legacy `ld-dropout-correct` did.
- CVBS-primary multi-source support:
  `chd_video_open_composite` and `chd_video_open_yc` now
  synthesize an `LdDecodeMetaData` (alternating-is-first-field,
  no-dropouts) at open time and stash it on the handle, so
  `chd_video_add_extra_source_composite` against a CVBS primary works
  (previously rejected with "primary source has no TBC metadata").
  Discrimination between TBC and CVBS in `chd_video_get_info` now
  reads the new `chd_video::metadataSynthesized` flag instead of
  testing `metadata != nullptr`.

### Changed
- `chd_video::extraSources` is now `std::vector<chd_video_extra>`,
  with each entry owning both the `ISource` and its own
  `LdDecodeMetaData`. Earlier vector-of-`unique_ptr<ISource>` shape
  couldn't carry the per-source dropout / VITS metadata that the
  multi-source `DropoutCorrector` needs.

### Added
- C ABI decode loop: `chd_decoder_set_option_*` /
  `chd_decoder_has_option` / `chd_decoder_set_nn_model` /
  `chd_decoder_commit` / `chd_decode_frame` (sync, random-access by
  frame index) / `chd_decode_frames_async` / `chd_cancel_*` and the
  full `chd_frame_*` accessor set (`chd_frame_get_info`,
  `chd_frame_get_plane`, `chd_frame_get_plane_float`,
  `chd_frame_copy_plane_float`, `chd_frame_free`). Supports all four
  pixel formats: `YUV444P16`, `YUV444_FLOAT` (zero-copy float planes
  rendered direct from `ComponentFrame`), `RGB48` (interleaved with
  per-plane byte-offset accessors), and `GRAY16`. Dropout correction
  is wired into the decode worker between `SourceField::loadFields`
  and `Decoder::decodeFrames`; per-frame stats land in
  `chd_decoder_get_last_dropout_stats`. CVBS primaries (which the corrector
  defers from the legacy `LdDecodeMetaData`) get a synthesized
  metadata in-flight so the same code path works for TBC + CVBS.
- Decoder registry (`src/decoders/registry.{h,cpp}`): translates the
  thirteen `chd_decoder_kind_t` enumerators into concrete
  `chd::decoders::Decoder` subclasses + applies caller-supplied
  options from the C ABI option maps. Centralises option-name →
  option-type → applicable-kinds validation behind
  `optionApplies` (backing `chd_decoder_has_option` and every
  `chd_decoder_set_option_*` call) and `kindUsesNn` (gating NN model
  binding). `CHD_DEC_AUTO` resolves to `NTSC_2D` for NTSC primaries
  and `PAL_2D` for PAL/PAL_M.
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
- NN provider attach plumbing (attach layer only):
  full `attachCuda` recipe ported from tbc-tools (preferred 8-option
  set with EXHAUSTIVE cuDNN conv search, fallback to a smaller
  compatibility set, automatic regex-driven filtering of options
  the local ORT build doesn't recognise) + Linux `libcuda.so.1` /
  `libnvidia-ptxjitcompiler.so.1` driver loader that rejects the
  `/stubs/` development libraries. `attachDirectML` for Windows,
  `attachMIGraphX` for Linux/AMD, `attachTensorRT` via the V2
  provider-options API (CUDA driver required). Windows ORT CUDA
  provider probe is intentionally a no-op, following
  vapoursynth-analog patch #5 (LoadLibrary'ing
  `onnxruntime_providers_cuda.dll` ourselves before ORT does
  crashes the provider's overridden `operator new`). The provider
  attach helpers are written by inspection against tbc-tools'
  working code; runtime validation against real hardware is
  deferred to Linux / Windows CI. The cuFFT / `nnTransform3D_kernel.cu`
  GPU processing pipeline (a parallel ~750 LOC ORT+CUDA pipeline,
  distinct from "swap FFT backends") is deferred to a Linux+CUDA
  follow-up session where end-to-end validation is feasible.
- ldzeug2 decoders: `chd::decoders::ldzeug::
  LdzeugColorCnnDecoder` (3-channel CVBS+I-carrier+Q-carrier ⇒
  Y+I+Q, replacing both Y/C separation and chroma demod) and
  `LdzeugLumaSepDecoder` (NN extracts Y; chroma is derived as
  CVBS−Y plus an analytical I/Q demod with optional `c_colorlp_b`
  bandpass). Both share `LdzeugDecoderBase`, take a
  `chd::nn::OrtSession` via `setNnModel`, and select between
  per-field and weaved-frame input via `setMode`. NTSC-only (the
  reference weights bundled by jsaowji are NTSC). Original
  algorithm + models authored by **jsaowji**. Smoke test loads
  three real bundled models (color_cnn_v2_alot,
  luma_sep_2dgray_fields, luma_sep_2d_frame_gray_gray_run2_latest)
  via CoreML on macOS (37/39 nodes attached).
- nnTransform3D CPU decoder: per-tile 3D-FFT + CNN-mask
  + IFFT chroma extraction, ported from tbc-tools' Comb extensions
  (authored by **asdfqazsnbb** and integrated by **harrypm**).
  Implemented as a `Comb::Configuration::nnTransform3D` mode plus
  three new `Comb::FrameBuffer` methods (`split3DnnTransform`,
  `finalizeNnTransform3D`, `fallbackNnTransform3DTo2D`); exposed at
  the public C++ surface as `chd::decoders::comb::NtscDecoder::
  setNnModel(std::shared_ptr<chd::nn::OrtSession>)`. Sibling-decoder
  semantics at the C ABI (`CHD_DEC_NN_TRANSFORM3D` is a distinct
  decoder kind) — full C ABI wiring lands later alongside
  `chd_decoder_create` / `chd_decoder_set_nn_model`. The FFT/window
  helpers live under `src/decoders/nntransform3d/` so the future
  CUDA cuFFT path can swap in without touching the
  algorithm body. Smoke test loads a real `chroma_net.onnx` and
  binds the session; full per-frame decode validation deferred to
  a follow-up.
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
