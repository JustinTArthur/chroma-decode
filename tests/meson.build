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
