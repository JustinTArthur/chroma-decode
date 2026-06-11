# Test suite. Grows as the library gains functionality.
#
# Unit tests are plain C++ programs that return 0 on pass; Catch2 is wired up
# later as the test surface grows. Integration tests generate fixtures via
# encode-orc (cloned and built in CI) and compare YUV planes to golden output
# from the legacy ld-chroma-decoder binary built at commit f39e59e18.

test_tbc_reader = executable('test_tbc_reader',
  'unit/test_tbc_reader.cpp',
  include_directories: [public_inc, version_inc],
  link_with: libchromadec,
  dependencies: [sqlite3_dep])
test('tbc_reader', test_tbc_reader)

# JSON sidecar back-compat: open a synthetic `.tbc.json` pair through the
# C ABI and confirm chd_video_open_composite falls through from missing .tbc.db
# to the .tbc.json reader (legacy ld-decode format pre-2025-SQLite-migration).
test_tbc_json_reader = executable('test_tbc_json_reader',
  'unit/test_tbc_json_reader.cpp',
  include_directories: [public_inc, version_inc],
  link_with: libchromadec)
test('tbc_json_reader', test_tbc_json_reader)

# End-to-end pipeline sanity test. Reaches into internal
# headers (the C ABI for decode isn't wired up yet) and
# links against the static libchromadec so internal C++ symbols are
# available — the shared lib's symbol-export list only exposes the
# C ABI (chd_*) by design.
test_encode_orc_color_bars = executable('test_encode_orc_color_bars',
  'unit/test_encode_orc_color_bars.cpp',
  include_directories: [public_inc, version_inc],
  link_with: libchromadec.get_static_lib(),
  dependencies: [sqlite3_dep, threads_dep, fftw3_dep])
test('encode_orc_color_bars', test_encode_orc_color_bars)

# format/ DATA-table sanity tests (preset lookup, sample
# conversion, signal-state mappings). Links the static lib because the
# format/ symbols are internal — the C ABI only exposes chd_* names.
test_format_presets = executable('test_format_presets',
  'unit/test_format_presets.cpp',
  include_directories: [public_inc, version_inc],
  link_with: libchromadec.get_static_lib())
test('format_presets', test_format_presets)

# CVBS reader end-to-end smoke tests. Uses the C ABI's
# chd_video_open_cvbs_* entry points + reaches into the internal
# CvbsCompositeSource / CvbsYcSource for sample-level assertions.
test_cvbs_reader = executable('test_cvbs_reader',
  'unit/test_cvbs_reader.cpp',
  include_directories: [public_inc, version_inc],
  link_with: libchromadec.get_static_lib(),
  dependencies: [sqlite3_dep])
test('cvbs_reader', test_cvbs_reader)

# dropout corrector. Builds a synthetic NTSC field pair, injects
# dropouts, runs DropoutCorrector::correctFrame, and asserts stats + sample
# replacement. Links the static lib because the corrector + metadata
# headers it consumes are internal.
test_dropout_corrector = executable('test_dropout_corrector',
  'unit/test_dropout_corrector.cpp',
  include_directories: [public_inc, version_inc],
  link_with: libchromadec.get_static_lib(),
  dependencies: [sqlite3_dep])
test('dropout_corrector', test_dropout_corrector)

# Multi-source VBI frame alignment. Builds in-memory metadata for a primary
# capture plus a VBI-offset CAV capture and a VBI-less capture, and asserts
# sourceFrameForPrimaryFrame registers by disc frame number (positional
# fallback when no VBI codes are present).
test_multi_source_alignment = executable('test_multi_source_alignment',
  'unit/test_multi_source_alignment.cpp',
  include_directories: [public_inc, version_inc],
  link_with: libchromadec.get_static_lib(),
  dependencies: [sqlite3_dep])
test('multi_source_alignment', test_multi_source_alignment)

# integration test driving real fixtures end-to-end through
# the public C ABI. Skips with PASS when CHD_ENCODE_ORC env var is unset
# (no encode-orc binary on this machine), so plain `meson test` stays
# green; CI / a local dev with encode-orc built will exercise the
# encode-orc → TBC → chd_decode_frame chain and assert real chroma
# content out of the decoder.
test_integration = executable('test_integration',
  'unit/test_integration.cpp',
  include_directories: [public_inc, version_inc],
  link_with: libchromadec)
test('integration', test_integration,
     timeout: 120)  # encode-orc takes a couple of seconds for one frame

# end-to-end C ABI decode loop (chd_decoder_create →
# chd_decoder_set_option_* → chd_decoder_commit → chd_decode_frame →
# chd_frame_get_plane). Links the shared lib so the test goes through the
# public C ABI; the synthetic-TBC fixture is built in-test (same shape as
# test_encode_orc_color_bars). Also covers chd_decode_frames_async +
# chd_cancel_t.
test_decode_frame_abi = executable('test_decode_frame_abi',
  'unit/test_decode_frame_abi.cpp',
  include_directories: [public_inc, version_inc],
  link_with: libchromadec,
  dependencies: [sqlite3_dep, threads_dep])
test('decode_frame_abi', test_decode_frame_abi)

# Decode-free dropout detection C ABI: CHD_DEC_NONE geometry-only commit,
# chd_decoder_get_output_info, chd_decoder_get_dropout_spans (field-relative
# dropouts mapped into active-output coordinates), and chd_decode_dropout_mask
# (GRAY16 mask). Injects known dropouts into a synthetic NTSC sidecar and
# checks the mapped span coordinates, the painted mask, and that a NONE
# decoder refuses to decode. Links the shared lib (public C ABI only).
test_dropout_spans_abi = executable('test_dropout_spans_abi',
  'unit/test_dropout_spans_abi.cpp',
  include_directories: [public_inc, version_inc],
  link_with: libchromadec,
  dependencies: [sqlite3_dep, threads_dep])
test('dropout_spans_abi', test_dropout_spans_abi)

# Absolute path to the committed test fixtures (tiny_identity.onnx). Passed
# to the NN tests so their model locator can find the synthetic fixture
# regardless of the working directory the test runs from.
nn_fixture_arg = '-DCHD_TEST_FIXTURE_DIR="' + (meson.current_source_dir() / 'fixtures') + '"'

# NN framework smoke tests (Ort::Env lifecycle, provider availability,
# chd_nn_model_load_from_file / _from_memory error paths, and a real-model
# happy path against the synthetic fixture). The body self-skips when the
# library was built with with_nn=false. Links the shared lib so the test
# goes through the public C ABI; ORT itself is not directly linked.
test_nn_framework = executable('test_nn_framework',
  'unit/test_nn_framework.cpp',
  include_directories: [public_inc, version_inc],
  cpp_args: [nn_fixture_arg],
  link_with: libchromadec)
test('nn_framework', test_nn_framework)

# nnTransform3D wiring smoke test. Links the static lib for
# internal-header access (Comb::Configuration, NtscDecoder::setNnModel,
# chd::nn::OrtSession). Constructs a session from the located model (the
# synthetic fixture by default, or real weights via $CHD_TEST_NN_MODEL) and
# binds it — both from a file path and from an in-memory buffer. Gated on the
# ORT backend specifically: these construct chd::nn::OrtSession / OrtEngine and
# link ort_dep, so they don't apply to a native-CoreML-only build.
if has_ort
  test_nntransform3d = executable('test_nntransform3d',
    'unit/test_nntransform3d.cpp',
    include_directories: [public_inc, version_inc],
    # CHD_WITH_NN matches the library build flag so the internal headers'
    # nn-gated methods (Comb::setNnModel, NtscDecoder::setNnModel) are
    # visible to the test.
    cpp_args: ['-DCHD_WITH_NN=1', nn_fixture_arg],
    link_with: libchromadec.get_static_lib(),
    dependencies: [ort_dep, threads_dep, fftw3_dep, sqlite3_dep])
  test('nntransform3d', test_nntransform3d)

  # ldzeug2 NN decoders. Same shape as the nnTransform3D test: links the
  # static lib for internal-header access; binds sessions from the located
  # model (synthetic fixture by default, or real weights via the
  # CHD_TEST_LDZEUG_* env vars), from both a path and an in-memory buffer.
  test_ldzeug = executable('test_ldzeug',
    'unit/test_ldzeug.cpp',
    include_directories: [public_inc, version_inc],
    cpp_args: ['-DCHD_WITH_NN=1', nn_fixture_arg],
    link_with: libchromadec.get_static_lib(),
    dependencies: [ort_dep, threads_dep, fftw3_dep, sqlite3_dep])
  test('ldzeug', test_ldzeug)
endif
