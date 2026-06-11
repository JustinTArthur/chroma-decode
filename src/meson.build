chromadec_sources = files(
  'abi/c_api_lifecycle.cpp',
  'abi/c_api_video.cpp',
  'abi/c_api_decoder.cpp',
  'abi/c_api_frame.cpp',
  'abi/c_api_nn.cpp',
  'abi/c_api_dropout.cpp',
  'abi/c_api_dropout_detect.cpp',
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
  'metadata/vbi_decoder.cpp',
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
  'dropout/multi_source_alignment.cpp',
  'pipeline/decoder_pool.cpp',
  'output/component_frame.cpp',
  'output/output_writer.cpp',
  'output/frame_canvas.cpp',
)

chromadec_deps = [threads_dep, sqlite3_dep, fftw3_dep]
chromadec_cpp_args = []
chromadec_cuda_args = []
# Backend-agnostic NN framework scaffolding: the decoders and the shared
# nnTransform3D FFT/window helpers. Built whenever any NN backend is present
# (has_nn = has_ort or has_coreml). ldzeug runs on either backend through the
# InferenceEngine interface; the nnTransform3D FFT/window helpers back both the
# ORT CPU path and the native CoreML host-FFT path.
if has_nn
  chromadec_sources += files(
    'decoders/nntransform3d/nntransform3d_fft_cpu.cpp',
    'decoders/nntransform3d/nntransform3d_window.cpp',
    'decoders/ldzeug/ldzeug_base.cpp',
    'decoders/ldzeug/ldzeug_color_cnn.cpp',
    'decoders/ldzeug/ldzeug_luma_sep.cpp',
  )
  chromadec_cpp_args += ['-DCHD_WITH_NN=1']
endif
# ONNX Runtime backend: the OrtEngine/OrtSession implementation and EP selection.
if has_ort
  chromadec_sources += files(
    'nn/ort_env.cpp',
    'nn/ort_session.cpp',
    'nn/ort_engine.cpp',
    'nn/provider_select.cpp',
  )
  chromadec_deps += [ort_dep]
  chromadec_cpp_args += ['-DCHD_WITH_ORT=1']
endif
if has_fftw
  chromadec_cpp_args += ['-DCHD_WITH_FFTW=1']
endif
if has_cuda
  # nnTransform3D's device-resident pipeline: cuFFT for the 3D FFTs +
  # ORT IoBinding so the CUDA / TensorRT EP can read inputs and write
  # outputs from device memory without round-tripping through the
  # host. .cu sources are compiled by nvcc (meson dispatches via the
  # `cuda` language declared in the top-level meson.build).
  chromadec_sources += files(
    'decoders/nntransform3d/nntransform3d_pipeline_cuda.cu',
  )
  chromadec_deps += [cuda_dep]
  chromadec_cpp_args += ['-DCHD_WITH_CUDA=1']
  # sm_60 (Pascal) is the floor for native FP64 atomicAdd, which the
  # OLA kernel needs. The resulting PTX JITs up to Ampere/Ada/Hopper
  # on newer GPUs at runtime.
  chromadec_cuda_args += ['-arch=sm_60']
endif

if has_rocm
  # AMD ROCm counterpart of the CUDA path. Meson has no first-class HIP
  # language, so the pipeline source is compiled by hipcc via
  # custom_target and the resulting object is linked into the library.
  # ROCm install prefix + ORT include path are passed explicitly because
  # custom_target doesn't propagate include dirs from dependency objects.
  rocm_compile_args = [
    '-c', '-fPIC', '-std=c++17', '-O2',
    '-I' + meson.project_source_root() / 'include',
    '-I' + meson.project_source_root() / 'src',
    '-I' + meson.project_build_root() / 'include',
  ]
  if get_option('onnxruntime_root') != ''
    rocm_compile_args += '-I' + get_option('onnxruntime_root') / 'include'
  endif
  hip_pipeline_o = custom_target(
    'nntransform3d_pipeline_hip',
    input: 'decoders/nntransform3d/nntransform3d_pipeline_hip.cpp',
    output: 'nntransform3d_pipeline_hip.o',
    command: [hipcc] + rocm_compile_args + ['@INPUT@', '-o', '@OUTPUT@'],
  )
  chromadec_sources += hip_pipeline_o
  chromadec_deps += [hipfft_dep, hip_rt_dep]
  chromadec_cpp_args += ['-DCHD_WITH_ROCM=1']
endif

# Native CoreML path (macOS). The .mm sources are compiled by the `objcpp`
# language; they share the same defines + include dirs as the C++ sources, so
# objcpp_args mirrors cpp_args (plus -std and ARC). Must come after the blocks
# above so chromadec_cpp_args is fully populated before we copy it.
chromadec_objcpp_args = []
if has_coreml
  chromadec_sources += files(
    'nn/coreml_engine.mm',
    'decoders/nntransform3d/nntransform3d_pipeline_coreml.mm',
  )
  chromadec_deps += [coreml_frameworks]
  chromadec_cpp_args += ['-DCHD_WITH_COREML=1']
  chromadec_objcpp_args = chromadec_cpp_args + ['-std=c++17', '-fobjc-arc']
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
  cuda_args: chromadec_cuda_args,
  objcpp_args: chromadec_objcpp_args,
  # NOTE: do NOT add -fvisibility=hidden. The version script (ELF) /
  # exports list (Mach-O) is the source of truth for what's exported.
  # -fvisibility=hidden would hide every internal symbol *and* the C ABI
  # before the linker filter even runs.
  link_args: link_args,
  install: true,
  soversion: '0',
  version: meson.project_version())
