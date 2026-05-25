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

# NN framework smoke tests (Ort::Env lifecycle, provider
# availability, chd_nn_model_load error path). The body self-skips when
# the library was built with with_nn=false. Links the shared lib so the
# test goes through the public C ABI; ORT itself is not directly linked.
test_nn_framework = executable('test_nn_framework',
  'unit/test_nn_framework.cpp',
  include_directories: [public_inc, version_inc],
  link_with: libchromadec)
test('nn_framework', test_nn_framework)
