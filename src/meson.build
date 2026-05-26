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
  'format/sample_encoding.cpp',
  'format/signal_state.cpp',
  'format/video_standards.cpp',
  'metadata/core.cpp',
  'metadata/cvbs_metadata_sqlite.cpp',
  'metadata/dropout_extension_sqlite.cpp',
  'metadata/dropouts.cpp',
  'metadata/json_io.cpp',
  'metadata/ld_metadata_json.cpp',
  'metadata/ld_metadata_sqlite.cpp',
  'metadata/sqlite_query.cpp',
  'reader/cvbs_composite_source.cpp',
  'reader/cvbs_yc_source.cpp',
  'reader/tbc_source.cpp',
  'decoders/registry.cpp',
  'decoders/source_field.cpp',
  'decoders/mono/mono_decoder.cpp',
  'decoders/palcolour/palcolour.cpp',
  'decoders/palcolour/pal_decoder.cpp',
  'decoders/palcolour/transform_pal.cpp',
  'decoders/palcolour/transform_pal_2d.cpp',
  'decoders/palcolour/transform_pal_3d.cpp',
  'decoders/comb/comb.cpp',
  'decoders/comb/ntsc_decoder.cpp',
  'dropout/dropout_corrector.cpp',
  'dropout/luma_filters.cpp',
  'pipeline/decoder_pool.cpp',
  'output/component_frame.cpp',
  'output/output_writer.cpp',
  'output/frame_canvas.cpp',
)

# NN framework (Ort::Env singleton, provider selection, model
# load). Only compiled when ORT was discovered. The C ABI shims in
# c_api_nn.cpp + c_api_lifecycle.cpp gate on the CHD_WITH_NN preprocessor
# define so the library still builds + links without NN support.
chromadec_deps = [threads_dep, sqlite3_dep, fftw3_dep]
chromadec_cpp_args = []
if has_nn
  chromadec_sources += files(
    'nn/ort_env.cpp',
    'nn/ort_session.cpp',
    'nn/provider_select.cpp',
    # nnTransform3D decoder (FFT helpers + window). The
    # split3DnnTransform algorithm itself lives in comb.cpp behind a
    # CHD_WITH_NN guard, so it gets pulled in via the existing comb
    # sources without listing it separately here.
    'decoders/nntransform3d/nntransform3d_fft_cpu.cpp',
    'decoders/nntransform3d/nntransform3d_window.cpp',
    # ldzeug2 NN-based NTSC decoders.
    'decoders/ldzeug/ldzeug_base.cpp',
    'decoders/ldzeug/ldzeug_color_cnn.cpp',
    'decoders/ldzeug/ldzeug_luma_sep.cpp',
  )
  chromadec_deps += [ort_dep]
  chromadec_cpp_args += ['-DCHD_WITH_NN=1']
endif
if has_fftw
  chromadec_cpp_args += ['-DCHD_WITH_FFTW=1']
endif
if has_cuda
  chromadec_cpp_args += ['-DCHD_WITH_CUDA=1']
endif

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
  dependencies: chromadec_deps,
  cpp_args: chromadec_cpp_args,
  # NOTE: do NOT add -fvisibility=hidden. The version script (ELF) /
  # exports list (Mach-O) is the source of truth for what's exported.
  # -fvisibility=hidden would hide every internal symbol *and* the C ABI
  # before the linker filter even runs.
  link_args: link_args,
  install: true,
  soversion: '0',
  version: meson.project_version())
