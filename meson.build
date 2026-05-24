project('chromadec', 'cpp', 'c',
        version: '0.1.0',
        license: 'GPL-3.0-or-later',
        meson_version: '>=0.60.0',
        default_options: [
            'cpp_std=c++17',
            'c_std=c11',
            'warning_level=2',
            'default_library=both',
            'b_ndebug=if-release',
        ])

cpp = meson.get_compiler('cpp')
cc  = meson.get_compiler('c')

# ── Dependencies ──────────────────────────────────────────────────────────────
threads_dep = dependency('threads')

# Optional at the skeleton stage: real decoders pick these up later.
sqlite3_dep = dependency('sqlite3', required: false)
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
has_cuda   = false  # wired up in a follow-up
has_fftw   = fftw3_dep.found()
has_sqlite = sqlite3_dep.found()

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
