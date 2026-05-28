# CI workflows

| Workflow | Purpose | Status |
|---|---|---|
| `build.yml` → `linux` | Build + `meson test` (with encode-orc-driven integration) + symbol-surface check, matrix on x86_64 / arm64 | Active |
| `build.yml` → `linux-asan-ubsan` | ASan + UBSan build + `meson test` (with encode-orc-driven integration), x86_64 only | Active |
| `build.yml` → `linux-tsan` | ThreadSanitizer build + `meson test` (with encode-orc-driven integration), x86_64 only | Active (untested on real CI) |
| `build.yml` → `macos` | Build + `meson test` (with encode-orc-driven integration) + symbol-surface check, matrix on arm64 / x86_64 | Active |
| `build.yml` → `windows` | MSVC x64 build + `meson test` (with encode-orc-driven integration) + symbol-surface check via `dumpbin`; vcpkg for sqlite3/fftw3, Microsoft ORT zip | Active |
| ABI check | `abi-compliance-checker` against previous release `.so` | Not yet — added at first tagged release |
| CUDA self-hosted | nnTransform3D CUDA FFT path validation | Not yet — self-hosted GPU runner |

## Reusable actions

- `.github/actions/setup-encode-orc/` — composite action that clones,
  builds (with cache), and exports `CHD_ENCODE_ORC` so
  `test_integration` runs against real NTSC + PAL colour-bars
  TBC fixtures instead of self-skipping. Pinned to a specific
  encode-orc commit via the `commit` input. encode-orc has no
  GitHub Releases — only ephemeral workflow artifacts — so source-build
  with commit-keyed cache is the durable pattern.

## ONNX Runtime artifacts

Downloaded from Microsoft's official GitHub releases on every platform.

| Job | Artifact | Pin |
|---|---|---|
| `linux` (x86_64) | `onnxruntime-linux-x64-${VER}.tgz` | 1.26.0 |
| `linux` (arm64) | `onnxruntime-linux-aarch64-${VER}.tgz` | 1.26.0 |
| `linux-asan-ubsan` | `onnxruntime-linux-x64-${VER}.tgz` | 1.26.0 |
| `macos` (arm64) | `onnxruntime-osx-arm64-${VER}.tgz` | 1.26.0 |
| `macos` (x86_64) | `onnxruntime-osx-x86_64-${VER}.tgz` | 1.23.2 |

The Linux CPU artifact is what these jobs need — GH-hosted runners have no
GPU and `with_cuda=auto` falls back silently. CUDA / TensorRT require the
separate `-gpu` artifact (1.26.0 ships both a default and a `gpu_cuda13`
variant) and a self-hosted GPU runner.

macOS releases include the CoreML execution provider; do not use the
Homebrew `onnxruntime` formula, which historically has shipped without
CoreML.

**macOS x86_64 is pinned to 1.23.2** because Microsoft dropped the
`onnxruntime-osx-x86_64` asset after that release. 1.24.0 onward ships
macOS arm64 only.

All Python tooling in CI runs inside a fresh per-job virtual environment.
