# Changelog

All notable changes to this project will be documented in this file. Format:
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versioning:
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Native CoreML is now a standalone inference backend, buildable **without**
  ONNX Runtime. A macOS build with `-Dwith_onnxruntime=false -Dwith_coreml=enabled`
  ships the ldzeug and nnTransform3D decoders running entirely on CoreML, with no
  ORT linked — the original purpose of the native path (an alternative to ORT's
  CoreML execution provider), previously unreachable because the build forced ORT
  to be present. The NN framework (`chd::nn::InferenceEngine`, the decoders, the
  `chd_nn_*` C ABI) is backend-agnostic and now compiles when *either* backend is
  present. Build option `with_nn` is renamed `with_onnxruntime` (it always meant
  the ORT backend); a new internal `CHD_WITH_ORT` gates ORT-only code while
  `CHD_WITH_NN` becomes the "any NN backend" umbrella. `chd_has_feature("nn")` now
  reports the framework (true for either backend); new `chd_has_feature("onnxruntime")`
  reports the ORT backend specifically (`"coreml"` already reported native CoreML).
  Requesting a `.onnx` / `CHD_NN_ORT_*` backend in a CoreML-only build returns
  `CHD_E_NN_BACKEND_UNAVAILABLE`. CUDA/ROCm pipelines remain ORT-family (they
  require the ORT backend).
- Native CoreML backend (macOS) — `chd::nn::CoreMLEngine`, an Objective-C++
  `InferenceEngine` implementation that drives an offline-converted
  `.mlpackage` through `MLModel` / `MLMultiArray`. This is the only route to
  GPU/ANE on macOS for nnTransform3D, whose 3D convolution the ONNX Runtime
  CoreML execution provider gates out to CPU; for ldzeug2 `color_cnn` it also
  beats the EP (~10× — the EP leaves the model's index ops on CPU across many
  partition boundaries). Covers all bundled models — nnTransform3D `chroma_net`
  and both ldzeug2 models (`color_cnn` and `luma_sep`). Models are converted with
  `scripts/convert_coreml.py` (ONNX → onnx2torch → TorchScript → coremltools,
  with pre-passes that fold `Constant`-node weights into initializers and rewrite
  SAME convolution padding), which must emit **fp32** at a **fixed input shape**:
  a flexible (`RangeDim`) shape runs ~30× slower for nnTransform3D and fails at
  runtime for ldzeug2 (`error -7`). An optional
  GPU-resident Layer-2 path for nnTransform3D (`CHD_NNTRANSFORM3D_COREML_FFT=mps`)
  runs a device 3-D FFT via MPSGraph plus Metal magnitude/mask kernels, keeping
  the spectrum in a shared `MTLBuffer` across FFT → conv → IFFT; it is
  macOS-14-gated and falls back transparently to the FFTW Layer-1 path on older
  SDKs/OSes, headless hosts, or any MPS failure. Output matches ONNX Runtime to
  rel-RMS ≈ 2e-6 at every tested shape. Gated by the `with_coreml` build option
  (`chd_has_feature("coreml")`). nnTransform3D model + algorithm by
  **asdfqazsnbb**; ldzeug2 by **jsaowji**.
- Backend-selection NN ABI. `chd_nn_session_opts_t.backend` — a family-grouped
  `chd_nn_backend_t`: `CHD_NN_BACKEND_AUTO`; the ONNX Runtime execution
  providers (`CHD_NN_ORT_*`, including `CHD_NN_ORT_AUTO` for the per-OS EP
  chain); and native non-ORT backends with no `ORT_` infix (`CHD_NN_COREML`) —
  replaces the previous ORT-only provider enum. `chd_nn_model_load_from_file`
  dispatches on it: `CHD_NN_BACKEND_AUTO` infers the backend from the artifact
  (`.onnx` → ONNX Runtime, `.mlpackage`/`.mlmodelc` → native CoreML), and a
  pinned backend forces its path (and requires the matching artifact). New
  `opts.coreml_compute` (`CHD_NN_COREML_CPU_AND_GPU` default / `_ALL` to allow
  the ANE / `_CPU_ONLY`) selects native CoreML compute units.
  `chd_nn_model_get_active_backend` / `chd_nn_backend_is_available` report the
  resolved backend and distinguish the native `CHD_NN_COREML` backend from the
  ORT CoreML EP (`CHD_NN_ORT_COREML`). Backed by a new `chd::nn::InferenceEngine`
  interface with `OrtEngine` and `CoreMLEngine` implementations; the decoders'
  `setNnModel` entry points now take a `shared_ptr<chd::nn::InferenceEngine>`.
- `CHD_PIXEL_RGBS` output format — full-range single-precision float
  `E′R E′G E′B` planes, computed direct from the decoder's component signals
  via the BT.601/H.273 MatrixCoefficients=5/6 Y′CbCr → R′G′B′ matrix with no
  intermediate Y′CbCr integer quantization. String token `"rgbs"` for
  `CHD_OPT_OUTPUT_FORMAT`. Plane access via `chd_frame_get_plane_float` with
  `CHD_PLANE_R`/`G`/`B`.
- `CHD_OPT_OUTPUT_CLAMP` option (string), with values `"none"` (default),
  `"legal_rgb_sdr"`, `"legal_rgb_hdr"`, and `"legal_ycbcr_bt601"`, controlling
  whether sample values are clamped. `legal_rgb_sdr` keeps the output between
  RGB black and RGB white (Y′CbCr → the canonical narrow-range box; R′G′B′ →
  `[0, 1]`). `legal_rgb_hdr` maps to positive-only R′G′B′ with unconstrained
  headroom past SDR white (`R′G′B′ ∈ [0, +∞)`); it is a no-op for the Y′CbCr /
  GRAY formats, which have no clean per-component box for that region.
  `legal_ycbcr_bt601` clamps Y′CbCr to BT.601 §2.5.3 video-allowed codes
  `[256, 65216]` (reserving the sync codeword regions). `none` applies no
  signal-domain clamp (H.273 `Clip1` / bit-depth saturation only). Backed by a
  new `chd_clamp_t` enum
  (`CHD_CLAMP_NONE`/`LEGAL_RGB_SDR`/`LEGAL_RGB_HDR`/`LEGAL_YCBCR_BT601`).
  Applied to integer Y′CbCr, integer RGB, and all float output formats; for
  `rgbs` under `legal_ycbcr_bt601` the per-component bounds reflect the
  forward projection of the BT.601-legal Y′CbCr volume
  (R′∈[-0.863, +1.884], G′∈[-0.667, +1.690], B′∈[-1.073, +2.093]).
- `chd_nn_model_load_from_memory(data, size, opts, out)` — load an ONNX
  model from an in-memory buffer instead of a file, for consumers that
  compile the model into their binary as a byte array (e.g. tbc-tools'
  embedded `chroma_net_v2.onnx`) and want no filesystem dependency. ORT
  copies the bytes during construction, so the buffer need not outlive the
  call. Backed by a new `chd::nn::OrtSession(const void*, size_t, ...)`
  constructor sharing the existing provider-attach prologue.
- Committed synthetic ONNX test fixture (`tests/fixtures/tiny_identity.onnx`,
  136 bytes, regenerable via `gen_tiny_onnx.py`) — a structurally valid but
  meaningless graph that lets the NN tests exercise the model loaders and
  provider attach unconditionally in CI, without shipping the large,
  separately licensed real weights. `test_nn_framework` drives both public C
  ABI loaders against it and asserts they resolve the same active backend;
  `test_nntransform3d` and `test_ldzeug` (color_cnn) bind sessions built both
  from a path and from an in-memory buffer. A shared, path-free helper
  (`tests/unit/nn_test_model.h`) resolves each model from a dedicated env var
  (`$CHD_TEST_NN_MODEL`, `$CHD_TEST_LDZEUG_COLOR_CNN_MODEL`,
  `$CHD_TEST_LDZEUG_LUMA_SEP_FIELD_MODEL`,
  `$CHD_TEST_LDZEUG_LUMA_SEP_FRAME_MODEL`), falling back to the fixture — no
  model paths are hardcoded, so real weights are validated by pointing the env
  vars at them locally or in a dedicated CI lane.

### Changed
- Default output clamp is now `CHD_CLAMP_NONE`, dropping a behaviour
  inherited from the upstream ld-chroma-decoder: the integer Y′CbCr / RGB
  output codes were previously clamped to the BT.601 §2.5.3 video-allowed
  range `[256, 65216]`, reserving the SDI sync codeword regions. Callers
  who need that protection must now opt in via `CHD_OPT_OUTPUT_CLAMP =
  "legal_ycbcr_bt601"`. Most consumers re-encoding through a modern codec
  (or running RGB conversion downstream) want `none`.
- Renumbered `chd_pixel_format_t`: the value of `CHD_PIXEL_GRAY16` /
  `CHD_PIXEL_GRAYS` shift to `4` and `5` (was `3` and `4`) to make room for
  `CHD_PIXEL_RGBS = 3` next to `CHD_PIXEL_RGB48 = 2`, keeping format families
  grouped in both list position and integer value. ABI break (pre-release;
  no deprecation alias).
- Active line ranges are now interpreted as **inclusive** — for both the frame
  lines (`CHD_OPT_FIRST/LAST_ACTIVE_FRAME_LINE`) and the field lines
  (`CHD_OPT_FIRST/LAST_ACTIVE_FIELD_LINE`), the last value is the last line that
  is part of the active region, not one past it. Output height is
  `last - first + 1`. Default active regions now produce a full **486-line
  picture for 525-line (NTSC)** and **576 lines for 625-line (PAL)** systems
  (NTSC frame default shifted up to `39..524`, PAL to `44..619`; field-line
  defaults decremented to match). tbc-tools v4 SQLite sidecars write
  `last_active_frame_line` / `last_active_field_line` as exclusive bounds; the
  SQLite reader now translates both columns to the inclusive scheme on ingest,
  so sidecar-described captures decode to their intended geometry (no behaviour
  change for them).
- Output padding now defaults to off: `CHD_OPT_PADDING_MULTIPLE` defaults to `1`
  (was `8`). Padding is also now honored uniformly by the `yuv444ps` output
  path, which previously emitted a tight active-region crop regardless of the
  requested padding multiple.
- Renamed `chd_nn_model_load` → `chd_nn_model_load_from_file` so the
  file-based and new memory-based loaders form a symmetric
  `_from_file` / `_from_memory` pair. ABI break (pre-release; no
  deprecation alias).

### Added
- Integration coverage extended to NTSC 3D + Transform PAL.
  test_integration.cpp now drives six chd decoder kinds against
  two encode-orc fixtures (NTSC + PAL 75 % colour bars, 3 frames each):
    CHD_DEC_NTSC_2D            ↔ ntsc2d
    CHD_DEC_NTSC_3D            ↔ ntsc3d
    CHD_DEC_NTSC_3D_NO_ADAPT   ↔ ntsc3dnoadapt
    CHD_DEC_PAL_2D             ↔ pal2d
    CHD_DEC_TRANSFORM_2D       ↔ transform2d
    CHD_DEC_TRANSFORM_3D       ↔ transform3d
  The 3-frame duration is the minimum that gives 3D variants real
  lookbehind + lookahead for the interior frame while still exercising
  the synthetic-black boundary fallback at frames 0 and 2. Encoder
  fixtures are built once per video standard and shared across all
  three variants of that standard, so encode-orc invocation count is 2
  rather than 6. 18-frame golden-frame comparison (6 variants × 3
  frames) confirmed bit-exact against the ld-chroma-decoder f39e59e18
  reference on every frame of every variant.

### Added
- Integration coverage extended to PAL + multi-frame:
  test_integration.cpp now parameterises its fixture spec
  (encode-orc format + asset path + chd decoder kind + legacy `-f`
  flag) and exercises both `ntsc-2d-bars` (525_5994_75_BARS.raw via
  CHD_DEC_NTSC_2D + ntsc2d) and `pal-2d-bars` (625_50_75_BARS.raw via
  CHD_DEC_PAL_2D + pal2d) at duration=3 frames each. Both the
  chroma-content check (every frame asserted to have real Y/Cb/Cr
  energy) and the golden-frame comparison (every frame matched
  pixel-for-pixel against ld-chroma-decoder f39e59e18) run per-frame
  per-fixture. Multi-frame fixtures catch per-frame state leakage
  that a single-frame test would mask; PAL exercises the palcolour
  code path that NTSC fixtures don't touch. Confirmed bit-exact on
  all 6 frames (3 NTSC + 3 PAL) against the legacy reference.

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
  `chd_frame_free`). Supports all five pixel formats: `YUV444P16`,
  `YUV444PS` (zero-copy normalized E′Y E′Cb E′Cr float planes rendered
  direct from `ComponentFrame`), `RGB48` (interleaved with per-plane
  byte-offset accessors), `GRAY16`, and `GRAYS` (E′Y float plane). The
  integer formats are narrow-range BT.601/H.273 quantizations of the float
  signals. Dropout correction
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
  `chd::nn::InferenceEngine` via `setNnModel`, and select between
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
  setNnModel(std::shared_ptr<chd::nn::InferenceEngine>)`. Sibling-decoder
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
  `chd_nn_model_load_from_file`, `chd_nn_model_load_from_memory`,
  `chd_nn_model_free`,
  `chd_nn_model_get_active_backend`, `chd_nn_backend_is_available`,
  `chd_nn_session_opts_default`. `chd_has_feature("nn"/"cuda"/
  "fftw"/"sqlite")` is wired through to the build-time `with_*`
  options. nnTransform3D and ldzeug2 decoder ports are deferred to
  a follow-up.

### Dependencies
- ONNX Runtime (required when `with_nn=true`, the default). The
  meson build fails loudly if `with_nn=true` and ORT is not
  discoverable via pkg-config or `-Donnxruntime_root`; pass
  `-Dwith_nn=false` to disable.
- Apple frameworks for the native CoreML backend (`with_coreml`, `auto`-enabled
  on macOS when `with_nn` is on): `CoreML`, `Foundation`, and — for the
  GPU-resident nnTransform3D Layer-2 FFT path — `Metal` and
  `MetalPerformanceShadersGraph` (built against a macOS 14+ SDK; the Layer-2
  code is `@available`-gated at runtime). No effect on non-Apple builds.
  Producing the `.mlpackage` artifacts is an offline step requiring `coremltools`
  + `onnx2torch` + `torch` (see `scripts/convert_coreml.py`); these are a
  build-time conversion toolchain, not a library runtime dependency, and the
  generated artifacts are not committed.
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
