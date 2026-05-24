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
  dependencies: [threads_dep],
  # NOTE: do NOT add -fvisibility=hidden. The version script (ELF) /
  # exports list (Mach-O) is the source of truth for what's exported.
  # -fvisibility=hidden would hide every internal symbol *and* the C ABI
  # before the linker filter even runs.
  link_args: link_args,
  install: true,
  soversion: '0',
  version: meson.project_version())
