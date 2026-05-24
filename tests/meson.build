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
