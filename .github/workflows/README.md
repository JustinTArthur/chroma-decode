# CI workflows

| Workflow | Purpose | Status |
|---|---|---|
| `build.yml` → `linux` | Build + `meson test` + symbol-surface check, matrix on x86_64 / arm64 | Active |
| `build.yml` → `linux-asan-ubsan` | ASan + UBSan sanitizer build + `meson test`, x86_64 only | Active |
| `build.yml` → `macos` | Build + `meson test` + symbol-surface check, matrix on arm64 / x86_64 | Active |
| Windows | MSVC build, ORT 1.26.0 from Microsoft release tarball | Not yet — follow-up |
| ABI check | `abi-compliance-checker` against previous release `.so` | Not yet — added at first tagged release |
| TSan | ThreadSanitizer pass on Linux | Not yet — follow-up |
| Encode-orc fixture generator | Clone, build, and run encode-orc; produce test `.tbc` + `.tbc.db` fixtures for the integration test job | Not yet — follow-up |
| CUDA self-hosted | nnTransform3D CUDA FFT path validation | Not yet — self-hosted GPU runner |

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
