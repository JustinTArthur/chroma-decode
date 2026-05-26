project('chromadec', 'cpp', 'c',
        version: '0.1.0',
        license: 'GPL-3.0-or-later',
        meson_version: '>=0.60.0',
        default_options: [
            'cpp_std=c++17',
            'c_std=c11',
            'cuda_std=c++17',
            'warning_level=2',
            'default_library=both',
            'b_ndebug=if-release',
        ])

cpp = meson.get_compiler('cpp')
cc  = meson.get_compiler('c')

# ── Dependencies ──────────────────────────────────────────────────────────────
threads_dep = dependency('threads')

# SQLite3 is required: the TBC metadata sidecars
# live in SQLite files (`*.tbc.db`) and chd_video_open_composite cannot work without
# the skeleton's `required: false` is upgraded here.
sqlite3_dep = dependency('sqlite3', required: true)
fftw3_dep   = dependency('fftw3',   required: false)

# ONNX Runtime discovery: pkg-config first, then -Donnxruntime_root fallback.
ort_dep = dependency('libonnxruntime', required: false)
if not ort_dep.found()
  ort_dep = dependency('onnxruntime', required: false)
endif
if not ort_dep.found() and get_option('with_nn') and get_option('onnxruntime_root') != ''
  ort_root = get_option('onnxruntime_root')
  ort_lib  = cpp.find_library('onnxruntime',
                              dirs: [ort_root / 'lib'],
                              required: false)
  if ort_lib.found()
    ort_dep = declare_dependency(
      dependencies: ort_lib,
      include_directories: include_directories(ort_root / 'include'))
  endif
endif

has_nn     = get_option('with_nn') and ort_dep.found()
has_fftw   = fftw3_dep.found()
has_sqlite = sqlite3_dep.found()

# ── CUDA toolkit discovery ────────────────────────────────────────────────────
# When `with_cuda=auto` (the default), the build tries to find the CUDA
# toolkit + cuFFT and silently falls back to the CPU FFTW path if either
# isn't installed. `enabled` turns missing dependencies into a configure
# error; `disabled` forces the CPU path even where CUDA is available.
#
# The runtime EP-select chain in src/nn/provider_select.cpp is independent
# of this flag: a session can still attach the CUDA / TensorRT EP, the
# library just can't take the device-resident pipeline shortcut without
# this build option — every inference round-trips through host memory.
cuda_opt = get_option('with_cuda')
has_cuda = false
if not cuda_opt.disabled() and has_nn
  cuda_dep = dependency('cuda',
                        version: '>=11.0',
                        modules: ['cudart', 'cufft'],
                        required: cuda_opt)
  if cuda_dep.found()
    cuda_lang_ok = add_languages('cuda', required: cuda_opt, native: false)
    if cuda_lang_ok
      has_cuda = true
    endif
  endif
endif

# If the user explicitly requested NN support but ORT is missing, fail loudly
# rather than silently dropping NN. The default for with_nn is `true`, so
# silent absence would mean every Linux/macOS dev build without ORT installed
# would quietly omit NN — and that bites later when an integration test
# loads a model expecting NN to be present.
if get_option('with_nn') and not ort_dep.found()
  error('with_nn=true but ONNX Runtime was not found via pkg-config or ' +
        '-Donnxruntime_root. Install ORT (Homebrew formula `onnxruntime`, ' +
        'apt `libonnxruntime-dev`, or download Microsoft GitHub release) or ' +
        'pass -Dwith_nn=false to disable NN decoders.')
endif

# ── Public headers + library (include/ defines version_h via configure_file,
#    placed at build/include/chromadec/version.h so <chromadec/version.h> works).
subdir('include')
subdir('src')

if get_option('with_tests') and has_sqlite # tests need sqlite for fixture I/O
  subdir('tests')
endif

if get_option('with_examples')
  subdir('examples')
endif

# ── pkg-config + CMake config installation ────────────────────────────────────
pkg = import('pkgconfig')
pkg.generate(libchromadec,
             name: 'chromadec',
             description: 'Composite video chroma decoding library',
             version: meson.project_version(),
             url: 'https://github.com/JustinTArthur/chroma-decode')

cmake_mod = import('cmake')
cmake_mod.write_basic_package_version_file(
  name: 'chromadec',
  version: meson.project_version(),
  compatibility: 'SameMinorVersion')

cmake_pkg_cfg = configuration_data()
cmake_pkg_cfg.set('VERSION', meson.project_version())
cmake_mod.configure_package_config_file(
  name: 'chromadec',
  input: 'meson/chromadec-config.cmake.in',
  configuration: cmake_pkg_cfg)

# ── Summary ───────────────────────────────────────────────────────────────────
summary({
  'NN decoders (ONNX Runtime)': has_nn,
  'CUDA FFT path':              has_cuda,
  'FFTW3 (Transform-PAL)':      has_fftw,
  'SQLite3 (metadata)':         has_sqlite,
  'Tests':                      get_option('with_tests'),
  'Examples':                   get_option('with_examples'),
  'Debug overlay':              get_option('with_debug_overlay'),
}, section: 'Optional features')
