# CI workflows

| Workflow | Purpose | Status |
|---|---|---|
| `build.yml` → `linux` | Build + `meson test` (with encode-orc-driven integration) + symbol-surface check, matrix on x86_64 / arm64 | Active |
| `build.yml` → `linux-asan-ubsan` | ASan + UBSan build + `meson test` (with encode-orc-driven integration), x86_64 only | Active |
| `build.yml` → `linux-tsan` | ThreadSanitizer build + `meson test` (with encode-orc-driven integration), x86_64 only | Active |
| `build.yml` → `macos` | Build + `meson test` (with encode-orc-driven integration) + symbol-surface check, matrix on arm64 / x86_64 | Active |
| `build.yml` → `windows` | MSVC x64 build + `meson test` (with encode-orc-driven integration) + symbol-surface check via `dumpbin`; vcpkg for sqlite3/fftw3, Microsoft ORT zip | Active |
| `gpu-cuda.yml` | NVIDIA A10G validation on AWS spot (`g5.xlarge` in `ap-northeast-2d`), weekly + manual; CUDA + TensorRT EPs against `chroma_net` v2 | Active (`workflow_dispatch` + weekly cron) |
| `gpu-amd.yml` | AMD V520 validation on AWS spot (`g4ad.xlarge` in `us-east-2c`) running inside `rocm/migraphx` Docker, weekly + manual; HIP/hipFFT pipeline + MIGraphX EP | Active (`workflow_dispatch` + weekly cron) |
| ABI check | `abi-compliance-checker` against previous release `.so` | Not yet — added at first tagged release |

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
| `linux` (x86_64) | `onnxruntime-linux-x64-${VER}.tgz` | 1.27.1 |
| `linux` (arm64) | `onnxruntime-linux-aarch64-${VER}.tgz` | 1.27.1 |
| `linux-asan-ubsan` / `linux-tsan` | `onnxruntime-linux-x64-${VER}.tgz` | 1.27.1 |
| `macos` (arm64) | `onnxruntime-osx-arm64-${VER}.tgz` | 1.27.1 |
| `macos` (x86_64) | `onnxruntime-osx-x86_64-${VER}.tgz` | 1.23.2 |
| `windows` | `onnxruntime-win-x64-${VER}.zip` | 1.27.1 |
| `gpu-cuda` | `onnxruntime-linux-x64-gpu_cuda12-${VER}.tgz` | 1.27.1 |
| `gpu-amd` | `onnxruntime-rocm` wheel from repo.radeon.com | 1.23.2 |

The CPU artifact is what the hosted jobs need — GH-hosted runners have no GPU
and `with_cuda=auto` falls back silently. CUDA / TensorRT require the GPU
artifact and a self-hosted GPU runner. The GPU artifact is published per CUDA
major (`gpu_cuda12` / `gpu_cuda13`); we take `gpu_cuda12`, matching the CUDA
on the deep-learning base AMI.

macOS releases include the CoreML execution provider; do not use the
Homebrew `onnxruntime` formula, which historically has shipped without
CoreML.

**macOS x86_64 is pinned to 1.23.2** because Microsoft dropped the
`onnxruntime-osx-x86_64` asset after that release. 1.24.0 onward ships
macOS arm64 only.

All Python tooling in CI runs inside a fresh per-job virtual environment.
