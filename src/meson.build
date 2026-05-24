chromadec_sources = files(
  'abi/c_api_lifecycle.cpp',
  'abi/c_api_video.cpp',
  'abi/c_api_decoder.cpp',
  'abi/c_api_frame.cpp',
  'abi/c_api_nn.cpp',
  'abi/c_api_dropout.cpp',
  'abi/c_api_pipeline.cpp',
  'abi/c_api_errors.cpp',
  'common/error_state.cpp',
  'common/log.cpp',
  'metadata/core.cpp',
  'metadata/dropouts.cpp',
  'metadata/ld_metadata_sqlite.cpp',
  'metadata/sqlite_query.cpp',
  'reader/tbc_source.cpp',
  'decoders/source_field.cpp',
  'decoders/mono/mono_decoder.cpp',
  'decoders/palcolour/palcolour.cpp',
  'decoders/palcolour/pal_decoder.cpp',
  'decoders/palcolour/transform_pal.cpp',
  'decoders/palcolour/transform_pal_2d.cpp',
  'decoders/palcolour/transform_pal_3d.cpp',
  'output/component_frame.cpp',
  'output/output_writer.cpp',
  'output/frame_canvas.cpp',
)

# Symbol export discipline:
# - On ELF (Linux), restrict exported symbols via a version script.
# - On macOS, restrict via -exported_symbols_list.
# - On Windows, a .def file will be added in a follow-up.
link_args = []
host_os = host_machine.system()
if host_os == 'linux' or host_os.endswith('bsd')
  link_args += ['-Wl,--version-script,' + meson.project_source_root() / 'meson/chromadec.map']
elif host_os == 'darwin'
  link_args += ['-Wl,-exported_symbols_list,' + meson.project_source_root() / 'meson/chromadec.exports']
endif

libchromadec = library('chromadec',
  chromadec_sources,
  include_directories: [public_inc, version_inc],
  dependencies: [threads_dep, sqlite3_dep, fftw3_dep],
  # NOTE: do NOT add -fvisibility=hidden. The version script (ELF) /
  # exports list (Mach-O) is the source of truth for what's exported.
  # -fvisibility=hidden would hide every internal symbol *and* the C ABI
  # before the linker filter even runs.
  link_args: link_args,
  install: true,
  soversion: '0',
  version: meson.project_version())
